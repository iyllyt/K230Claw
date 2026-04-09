/*
 * ws_server.c - Web 聊天界面（mongoose 嵌入式 HTTP/WebSocket）
 *
 * mongoose 单线程事件循环，在独立 pthread 中运行。
 * HTTP 请求返回内嵌 HTML 聊天页面。
 * WebSocket 消息推入 inbound 队列，outbound 通过 send_text 回调发送。
 *
 * 编译条件: 需要 third_party/mongoose/mongoose.c/h
 *   如果 mongoose 不存在，此模块编译为空 stub。
 */

#include "ws_server.h"
#include "chat_html.h"
#include "stream_html.h"
#include "../kc_config.h"
#include "../bus/message_bus.h"
#include "../llm/llm_proxy.h"
#include "../memory/session_mgr.h"
#include "../third_party/cJSON/cJSON.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>

#define TAG "ws"
#define WS_PORT      "8080"
#define WS_CHAT_ID   "web"

/* ── 条件编译：检查 mongoose 是否存在 ── */

#ifdef KC_HAS_MONGOOSE

#include "../third_party/mongoose/mongoose.h"

static struct mg_mgr s_mgr;
static pthread_t s_ws_tid;

/* 跟踪活跃 WebSocket 连接（简单链表） */
#define MAX_WS_CLIENTS  8

static struct mg_connection *s_ws_clients[MAX_WS_CLIENTS];
static int s_ws_count = 0;
static pthread_mutex_t s_ws_mutex = PTHREAD_MUTEX_INITIALIZER;

static void ws_add_client(struct mg_connection *c) {
    pthread_mutex_lock(&s_ws_mutex);
    if (s_ws_count < MAX_WS_CLIENTS)
        s_ws_clients[s_ws_count++] = c;
    pthread_mutex_unlock(&s_ws_mutex);
}

static void ws_remove_client(struct mg_connection *c) {
    pthread_mutex_lock(&s_ws_mutex);
    for (int i = 0; i < s_ws_count; i++) {
        if (s_ws_clients[i] == c) {
            s_ws_clients[i] = s_ws_clients[--s_ws_count];
            break;
        }
    }
    pthread_mutex_unlock(&s_ws_mutex);
}

/* ── 流媒体客户端管理 ── */

#include "../hal/hal_camera_stream.h"

#define MAX_STREAM_CLIENTS 4

static struct mg_connection *s_stream_conns[MAX_STREAM_CLIENTS];
static int s_stream_count = 0;
static pthread_mutex_t s_stream_mutex = PTHREAD_MUTEX_INITIALIZER;

static void stream_add_conn(struct mg_connection *c) {
    pthread_mutex_lock(&s_stream_mutex);
    if (s_stream_count < MAX_STREAM_CLIENTS)
        s_stream_conns[s_stream_count++] = c;
    pthread_mutex_unlock(&s_stream_mutex);
    KC_LOGI(TAG, "stream client connected (%d total)", s_stream_count);
}

static void stream_remove_conn(struct mg_connection *c) {
    pthread_mutex_lock(&s_stream_mutex);
    for (int i = 0; i < s_stream_count; i++) {
        if (s_stream_conns[i] == c) {
            s_stream_conns[i] = s_stream_conns[--s_stream_count];
            break;
        }
    }
    pthread_mutex_unlock(&s_stream_mutex);
    KC_LOGI(TAG, "stream client disconnected (%d remaining)", s_stream_count);
}

/* 帧回调：将 JPEG 数据以 MJPEG multipart 格式发送给所有流媒体客户端 */
static void stream_frame_cb(const uint8_t *jpeg, size_t len, void *ud) {
    (void)ud;
    char header[256];
    int hlen = snprintf(header, sizeof(header),
        "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n", len);

    pthread_mutex_lock(&s_stream_mutex);
    for (int i = 0; i < s_stream_count; i++) {
        struct mg_connection *c = s_stream_conns[i];
        if (!c || c->is_draining || c->is_closing) {
            s_stream_conns[i] = s_stream_conns[--s_stream_count];
            i--;
            continue;
        }
        mg_send(c, header, hlen);
        mg_send(c, jpeg, len);
        mg_send(c, "\r\n", 2);
    }
    pthread_mutex_unlock(&s_stream_mutex);
}

/* 流媒体路由处理 */
static void handle_stream_request(struct mg_connection *c) {
    if (!hal_camera_stream_is_running()) {
        mg_http_reply(c, 503, "Content-Type: text/plain\r\n",
                      "Stream not active. Switch to navigator role first.\r\n");
        return;
    }
    /* 发送 MJPEG multipart headers */
    mg_printf(c,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "\r\n");
    stream_add_conn(c);

    /* 首个客户端时注册帧回调 */
    if (s_stream_count == 1) {
        hal_camera_stream_on_frame(stream_frame_cb, NULL);
        KC_LOGI(TAG, "stream frame callback registered");
    }
}

/* 流媒体状态 JSON */
static void handle_stream_status(struct mg_connection *c) {
    yolo_snapshot_t snap;
    hal_camera_stream_get_detections(&snap);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "detections", snap.count);

    /* 找最高置信度的类别作为 top_class */
    const char *top_class = "";
    float top_conf = 0;
    for (int i = 0; i < snap.count; i++) {
        if (snap.detections[i].confidence > top_conf) {
            top_conf = snap.detections[i].confidence;
            top_class = snap.detections[i].class_name;
        }
    }
    cJSON_AddStringToObject(root, "top_class", top_class);
    cJSON_AddNumberToObject(root, "fps", hal_camera_stream_get_fps());

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json) {
        mg_http_reply(c, 200,
            "Content-Type: application/json\r\n"
            "Access-Control-Allow-Origin: *\r\n",
            "%s", json);
        free(json);
    }
}

/* ── 配置消息处理 ── */

/* API key 脱敏：长度≤7 → "***"，否则前3+***+后4 */
static void mask_key(const char *raw, char *out, size_t out_sz) {
    size_t len = raw ? strlen(raw) : 0;
    if (len == 0) {
        out[0] = '\0';
    } else if (len <= 7) {
        snprintf(out, out_sz, "***");
    } else {
        snprintf(out, out_sz, "%.3s***%.4s", raw, raw + len - 4);
    }
}

/* 判断是否为脱敏值（含 ***） */
static int is_masked(const char *val) {
    return val && strstr(val, "***") != NULL;
}

static void ws_send_json(struct mg_connection *c, cJSON *json) {
    char *str = cJSON_PrintUnformatted(json);
    if (str) {
        mg_ws_send(c, str, strlen(str), WEBSOCKET_OP_TEXT);
        free(str);
    }
}

/* 对 model_list 中的 api_key 做脱敏（返回新 cJSON 数组，调用者 free） */
static cJSON *mask_model_list(cJSON *list) {
    cJSON *out = cJSON_CreateArray();
    if (!list || !cJSON_IsArray(list)) return out;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, list) {
        cJSON *dup = cJSON_Duplicate(item, 1);
        cJSON *key_item = cJSON_GetObjectItem(dup, "api_key");
        if (cJSON_IsString(key_item) && key_item->valuestring[0]) {
            char masked[128];
            mask_key(key_item->valuestring, masked, sizeof(masked));
            cJSON_SetValuestring(key_item, masked);
        }
        cJSON_AddItemToArray(out, dup);
    }
    return out;
}

static void handle_get_config(struct mg_connection *c) {
    char masked_oai[128], masked_ant[128];
    const char *oai_key = kc_config_get_str("openai_api_key", "");
    const char *ant_key = kc_config_get_str("anthropic_api_key", "");
    mask_key(oai_key, masked_oai, sizeof(masked_oai));
    mask_key(ant_key, masked_ant, sizeof(masked_ant));

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "type", "config");
    cJSON *data = cJSON_AddObjectToObject(resp, "data");
    cJSON_AddStringToObject(data, "provider",
        kc_config_get_str("provider", "openai"));
    cJSON_AddStringToObject(data, "model",
        kc_config_get_str("model", KC_LLM_DEFAULT_MODEL));
    cJSON_AddStringToObject(data, "openai_api_key", masked_oai);
    cJSON_AddStringToObject(data, "openai_api_url",
        kc_config_get_str("openai_api_url", KC_OPENAI_API_URL));
    cJSON_AddStringToObject(data, "anthropic_api_key", masked_ant);
    cJSON_AddStringToObject(data, "anthropic_api_url",
        kc_config_get_str("anthropic_api_url", KC_ANTHROPIC_API_URL));
    cJSON_AddStringToObject(data, "proxy",
        kc_config_get_str("proxy", ""));

    /* model_list（脱敏） */
    cJSON *ml = kc_config_get_json("model_list");
    cJSON *masked_ml = mask_model_list(ml);
    cJSON_Delete(ml);
    cJSON_AddItemToObject(data, "model_list", masked_ml);

    ws_send_json(c, resp);
    cJSON_Delete(resp);
}

static void handle_set_config(struct mg_connection *c, cJSON *data) {
    if (!data) {
        cJSON *err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "type", "config_error");
        cJSON_AddStringToObject(err, "message", "missing data");
        ws_send_json(c, err);
        cJSON_Delete(err);
        return;
    }

    /*
     * 先写入 kc_config（数据层），再同步到 llm_proxy 运行时变量。
     * API key/url 先写 kc_config，最后调 llm_set_provider 会从 kc_config 重新加载。
     */

    /* 1. API key：跳过脱敏值 */
    const char *oai_key = cJSON_GetStringValue(
        cJSON_GetObjectItem(data, "openai_api_key"));
    if (oai_key && oai_key[0] && !is_masked(oai_key))
        kc_config_set_str("openai_api_key", oai_key);

    const char *ant_key = cJSON_GetStringValue(
        cJSON_GetObjectItem(data, "anthropic_api_key"));
    if (ant_key && ant_key[0] && !is_masked(ant_key))
        kc_config_set_str("anthropic_api_key", ant_key);

    /* 2. API URL */
    const char *oai_url = cJSON_GetStringValue(
        cJSON_GetObjectItem(data, "openai_api_url"));
    if (oai_url && oai_url[0])
        kc_config_set_str("openai_api_url", oai_url);

    const char *ant_url = cJSON_GetStringValue(
        cJSON_GetObjectItem(data, "anthropic_api_url"));
    if (ant_url && ant_url[0])
        kc_config_set_str("anthropic_api_url", ant_url);

    /* 3. Proxy */
    const char *proxy = cJSON_GetStringValue(
        cJSON_GetObjectItem(data, "proxy"));
    if (proxy)
        llm_set_proxy(proxy);

    /* 4. Model */
    const char *model = cJSON_GetStringValue(
        cJSON_GetObjectItem(data, "model"));
    if (model && model[0])
        llm_set_model(model);

    /* 5. Provider（最后处理：会从 kc_config 重新读取 key 和 url） */
    const char *provider = cJSON_GetStringValue(
        cJSON_GetObjectItem(data, "provider"));
    if (provider && provider[0])
        llm_set_provider(provider);

    kc_config_save();
    KC_LOGI(TAG, "config updated via WebSocket");

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "type", "config_saved");
    ws_send_json(c, resp);
    cJSON_Delete(resp);
}

/* 应用预设：从 model_list 中选一组配置激活 */
static void handle_apply_preset(struct mg_connection *c, cJSON *data) {
    if (!data) goto err;
    cJSON *idx_item = cJSON_GetObjectItem(data, "index");
    if (!cJSON_IsNumber(idx_item)) goto err;
    int idx = idx_item->valueint;

    cJSON *ml = kc_config_get_json("model_list");
    if (!ml || !cJSON_IsArray(ml)) { cJSON_Delete(ml); goto err; }
    int sz = cJSON_GetArraySize(ml);
    if (idx < 0 || idx >= sz) { cJSON_Delete(ml); goto err; }

    cJSON *preset = cJSON_GetArrayItem(ml, idx);
    const char *provider = cJSON_GetStringValue(cJSON_GetObjectItem(preset, "provider"));
    const char *model    = cJSON_GetStringValue(cJSON_GetObjectItem(preset, "model"));
    const char *api_key  = cJSON_GetStringValue(cJSON_GetObjectItem(preset, "api_key"));
    const char *api_url  = cJSON_GetStringValue(cJSON_GetObjectItem(preset, "api_url"));

    if (!provider || !model) { cJSON_Delete(ml); goto err; }

    /* api_url 为空时使用 provider 默认值 */
    const char *effective_url = (api_url && api_url[0]) ? api_url :
        (strcmp(provider, "anthropic") == 0 ? KC_ANTHROPIC_API_URL : KC_OPENAI_API_URL);

    /* 先写 kc_config，再触发 llm_proxy 运行时同步 */
    if (api_key && api_key[0]) {
        if (strcmp(provider, "anthropic") == 0)
            kc_config_set_str("anthropic_api_key", api_key);
        else
            kc_config_set_str("openai_api_key", api_key);
    }
    if (strcmp(provider, "anthropic") == 0)
        kc_config_set_str("anthropic_api_url", effective_url);
    else
        kc_config_set_str("openai_api_url", effective_url);

    /* llm_set_provider 从 kc_config 读取最新的 key/url */
    llm_set_provider(provider);
    llm_set_model(model);
    if (api_key && api_key[0])
        llm_set_api_key(api_key);
    llm_set_api_url(effective_url);

    kc_config_save();

    const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(preset, "name"));
    KC_LOGI(TAG, "preset applied: %s (%s/%s)", name ? name : "?", provider, model);

    cJSON_Delete(ml);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "type", "preset_applied");
    ws_send_json(c, resp);
    cJSON_Delete(resp);
    return;

err: {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "type", "config_error");
        cJSON_AddStringToObject(e, "message", "invalid preset index");
        ws_send_json(c, e);
        cJSON_Delete(e);
    }
}

/* 保存预设列表（整个 model_list 替换） */
static void handle_save_presets(struct mg_connection *c, cJSON *data) {
    if (!data || !cJSON_IsArray(data)) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "type", "config_error");
        cJSON_AddStringToObject(e, "message", "model_list must be array");
        ws_send_json(c, e);
        cJSON_Delete(e);
        return;
    }

    /*
     * 客户端发来的 api_key 可能是脱敏值（***）。
     * 对于含 *** 的 key，按 name+model 匹配从旧列表恢复原始值。
     * （不能按索引匹配，因为用户可能删除/重排了条目）
     */
    cJSON *old_ml = kc_config_get_json("model_list");
    cJSON *new_ml = cJSON_Duplicate(data, 1);
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, new_ml) {
        cJSON *key_item = cJSON_GetObjectItem(item, "api_key");
        if (cJSON_IsString(key_item) && is_masked(key_item->valuestring)) {
            /* 按 name+model 在旧列表中查找对应条目 */
            const char *new_name = cJSON_GetStringValue(
                cJSON_GetObjectItem(item, "name"));
            const char *new_model = cJSON_GetStringValue(
                cJSON_GetObjectItem(item, "model"));
            if (old_ml && cJSON_IsArray(old_ml) && new_name && new_model) {
                cJSON *old_item = NULL;
                cJSON_ArrayForEach(old_item, old_ml) {
                    const char *on = cJSON_GetStringValue(
                        cJSON_GetObjectItem(old_item, "name"));
                    const char *om = cJSON_GetStringValue(
                        cJSON_GetObjectItem(old_item, "model"));
                    if (on && om &&
                        strcmp(on, new_name) == 0 &&
                        strcmp(om, new_model) == 0) {
                        const char *old_key = cJSON_GetStringValue(
                            cJSON_GetObjectItem(old_item, "api_key"));
                        if (old_key)
                            cJSON_SetValuestring(key_item, old_key);
                        break;
                    }
                }
            }
        }
    }

    cJSON_Delete(old_ml);
    kc_config_set_json("model_list", new_ml);
    kc_config_save();
    KC_LOGI(TAG, "model_list saved (%d presets)", cJSON_GetArraySize(new_ml));

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "type", "presets_saved");
    ws_send_json(c, resp);
    cJSON_Delete(resp);
}

/* 事件处理 */
static void ev_handler(struct mg_connection *c, int ev, void *ev_data) {
    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;

        if (mg_match(hm->uri, mg_str("/ws"), NULL)) {
            mg_ws_upgrade(c, hm, NULL);
            ws_add_client(c);
            KC_LOGI(TAG, "WebSocket client connected (%d total)", s_ws_count);
        } else if (mg_match(hm->uri, mg_str("/stream.html"), NULL)) {
            mg_http_reply(c, 200, "Content-Type: text/html\r\n",
                          "%s", s_stream_html);
        } else if (mg_match(hm->uri, mg_str("/stream_status"), NULL)) {
            handle_stream_status(c);
        } else if (mg_match(hm->uri, mg_str("/stream"), NULL)) {
            handle_stream_request(c);
        } else {
            /* 返回聊天页面 */
            mg_http_reply(c, 200, "Content-Type: text/html\r\n",
                          "%s", s_chat_html);
        }

    } else if (ev == MG_EV_WS_MSG) {
        struct mg_ws_message *wm = (struct mg_ws_message *)ev_data;

        char *msg_data = malloc(wm->data.len + 1);
        if (!msg_data) return;
        memcpy(msg_data, wm->data.buf, wm->data.len);
        msg_data[wm->data.len] = '\0';

        cJSON *root = cJSON_Parse(msg_data);
        free(msg_data);
        if (!root) return;

        /* 检查 type 字段：配置操作 or 对话 */
        const char *type = cJSON_GetStringValue(
            cJSON_GetObjectItem(root, "type"));

        if (type && strcmp(type, "get_config") == 0) {
            handle_get_config(c);
        } else if (type && strcmp(type, "set_config") == 0) {
            handle_set_config(c, cJSON_GetObjectItem(root, "data"));
        } else if (type && strcmp(type, "apply_preset") == 0) {
            handle_apply_preset(c, cJSON_GetObjectItem(root, "data"));
        } else if (type && strcmp(type, "save_presets") == 0) {
            handle_save_presets(c, cJSON_GetObjectItem(root, "data"));
        } else {
            /* 对话消息（向后兼容：无 type 字段 = 对话） */
            const char *text = cJSON_GetStringValue(
                cJSON_GetObjectItem(root, "text"));
            if (text && text[0]) {
                /* Web 端 / 命令处理 */
                if (text[0] == '/') {
                    const char *reply = NULL;
                    char reply_buf[256];
                    if (strcmp(text, "/clearscr") == 0) {
                        /* 通知前端清空聊天气泡 */
                        cJSON *r = cJSON_CreateObject();
                        cJSON_AddStringToObject(r, "type", "clear_screen");
                        ws_send_json(c, r);
                        cJSON_Delete(r);
                        reply = NULL;  /* 不再发文本回复 */
                    } else if (strcmp(text, "/clearhis") == 0) {
                        session_clear(WS_CHAT_ID);
                        reply = "Context cleared.";
                    } else if (strcmp(text, "/reconnect") == 0) {
                        kc_check_network();
                        snprintf(reply_buf, sizeof(reply_buf),
                                 "Status: %s", kc_is_online() ? "Online" : "Offline");
                        reply = reply_buf;
                    } else if (strcmp(text, "/web") == 0) {
                        reply = "Web UI: http://localhost:8080";
                    } else if (strcmp(text, "/help") == 0) {
                        reply = "Commands:\n"
                                "  /clearscr - Clear chat screen\n"
                                "  /clearhis - Clear conversation context\n"
                                "  /reconnect - Retry network\n"
                                "  /help - Show this help\n"
                                "  (other) - Send to AI";
                    } else {
                        snprintf(reply_buf, sizeof(reply_buf),
                                 "Unknown command: %s\nType /help for available commands.", text);
                        reply = reply_buf;
                    }
                    /* 回复给请求者 */
                    if (reply) {
                        cJSON *r = cJSON_CreateObject();
                        cJSON_AddStringToObject(r, "text", reply);
                        ws_send_json(c, r);
                        cJSON_Delete(r);
                    }
                } else {
                    /* 前置检查：API key 缺失时立即报错，不等 LLM 超时 */
                    const char *cur_provider = kc_config_get_str("provider", "openai");
                    const char *cur_key = (strcmp(cur_provider, "anthropic") == 0)
                        ? kc_config_get_str("anthropic_api_key", "")
                        : kc_config_get_str("openai_api_key", "");
                    if (cur_key[0] == '\0') {
                        cJSON *r = cJSON_CreateObject();
                        cJSON_AddStringToObject(r, "text",
                            "No API key configured. Open Settings (gear icon) to set your API key.");
                        ws_send_json(c, r);
                        cJSON_Delete(r);
                    } else {
                        /* 普通对话消息 → 推入 inbound 队列 */
                        kc_msg_t msg = {0};
                        strncpy(msg.channel, KC_CHAN_WEBSOCKET,
                                sizeof(msg.channel) - 1);
                        strncpy(msg.chat_id, WS_CHAT_ID,
                                sizeof(msg.chat_id) - 1);
                        msg.content = strdup(text);
                        if (msg.content) {
                            if (message_bus_push_inbound(&msg) != KC_OK)
                                free(msg.content);
                        }
                    }
                }
            }
        }
        cJSON_Delete(root);

    } else if (ev == MG_EV_CLOSE) {
        if (c->is_websocket) {
            ws_remove_client(c);
            KC_LOGI(TAG, "WebSocket client disconnected (%d remaining)", s_ws_count);
        } else {
            stream_remove_conn(c);
        }
    }
}

/* WebSocket 线程 */
static void *ws_thread(void *arg) {
    (void)arg;
    KC_LOGI(TAG, "WebSocket server started on port %s", WS_PORT);

    while (!kc_is_shutting_down()) {
        mg_mgr_poll(&s_mgr, 200);
    }

    mg_mgr_free(&s_mgr);
    KC_LOGI(TAG, "WebSocket server stopped");
    return NULL;
}

/* ── 通道接口 ── */

static kc_err_t ws_start(kc_channel_t *self) {
    (void)self;
    mg_mgr_init(&s_mgr);

    char listen_url[64];
    snprintf(listen_url, sizeof(listen_url), "http://0.0.0.0:%s", WS_PORT);
    struct mg_connection *c = mg_http_listen(&s_mgr, listen_url, ev_handler, NULL);
    if (!c) {
        KC_LOGE(TAG, "failed to bind port %s", WS_PORT);
        return KC_FAIL;
    }

    pthread_create(&s_ws_tid, NULL, ws_thread, NULL);
    return KC_OK;
}

static kc_err_t ws_stop(kc_channel_t *self) {
    (void)self;
    KC_LOGI(TAG, "stopping WebSocket server...");
    pthread_join(s_ws_tid, NULL);
    return KC_OK;
}

static kc_err_t ws_send_text(kc_channel_t *self, const char *chat_id, const char *text) {
    (void)self; (void)chat_id;
    if (!text) return KC_ERR_INVALID;

    /* 构造 JSON: {"text": "..."} */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "text", text);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return KC_ERR_NO_MEM;

    /* 广播到所有 WebSocket 客户端 */
    pthread_mutex_lock(&s_ws_mutex);
    for (int i = 0; i < s_ws_count; i++) {
        mg_ws_send(s_ws_clients[i], json, strlen(json), WEBSOCKET_OP_TEXT);
    }
    pthread_mutex_unlock(&s_ws_mutex);

    free(json);
    return KC_OK;
}

static kc_channel_t s_ws_channel = {
    .name       = KC_CHAN_WEBSOCKET,
    .start      = ws_start,
    .stop       = ws_stop,
    .send_text  = ws_send_text,
    .send_image = NULL,   /* TODO: 未来可通过 WebSocket 发送 base64 图片 */
    .ctx        = NULL,
};

kc_channel_t *ws_channel_get(void) {
    return &s_ws_channel;
}

#else /* !KC_HAS_MONGOOSE */

/* Stub: mongoose 不可用时的空实现 */

static kc_err_t ws_start_stub(kc_channel_t *self) {
    (void)self;
    KC_LOGW(TAG, "WebSocket server not available (mongoose not compiled)");
    return KC_OK;
}

static kc_err_t ws_stop_stub(kc_channel_t *self) {
    (void)self;
    return KC_OK;
}

static kc_err_t ws_send_text_stub(kc_channel_t *self, const char *chat_id, const char *text) {
    (void)self; (void)chat_id; (void)text;
    return KC_ERR_INVALID;
}

static kc_channel_t s_ws_channel = {
    .name       = KC_CHAN_WEBSOCKET,
    .start      = ws_start_stub,
    .stop       = ws_stop_stub,
    .send_text  = ws_send_text_stub,
    .send_image = NULL,
    .ctx        = NULL,
};

kc_channel_t *ws_channel_get(void) {
    return &s_ws_channel;
}

#endif /* KC_HAS_MONGOOSE */
