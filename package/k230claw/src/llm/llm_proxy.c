/*
 * llm_proxy.c - LLM API 客户端 (libcurl, OpenAI + Anthropic)
 *
 * 从 MimiClaw llm_proxy.c 移植:
 *   - esp_http_client → libcurl
 *   - http_proxy.c → CURLOPT_PROXY (libcurl 内置)
 *   - NVS → JSON 配置文件
 *   - heap_caps → 标准 malloc
 *
 * 内部统一用 Anthropic 格式作为中间表示（Agent Loop 构建的 messages）。
 * 调 OpenAI 时通过 convert_messages_openai / convert_tools_openai 转换。
 */

#include "llm_proxy.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <curl/curl.h>

#define TAG "llm"

#define LLM_API_KEY_MAX_LEN  320
#define LLM_MODEL_MAX_LEN    64
#define ANTHROPIC_API_VERSION "2023-06-01"

/* ── 静态配置 ── */

static char s_api_key[LLM_API_KEY_MAX_LEN] = {0};
static char s_model[LLM_MODEL_MAX_LEN]     = KC_LLM_DEFAULT_MODEL;
static char s_provider[16]                  = "openai";
static char s_api_url[256]                  = KC_OPENAI_API_URL;
static char s_proxy[256]                    = {0};
static pthread_mutex_t s_llm_mutex = PTHREAD_MUTEX_INITIALIZER;

static bool provider_is_openai(void) {
    return strcmp(s_provider, "openai") == 0;
}

/* ── 辅助函数 ── */

static void safe_copy(char *dst, size_t dst_size, const char *src) {
    if (!dst || dst_size == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t n = strlen(src);
    if (n >= dst_size) n = dst_size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void log_preview(const char *label, const char *data) {
    if (!data) { KC_LOGI(TAG, "%s: <null>", label); return; }
    size_t total = strlen(data);
    size_t show = total > KC_LLM_LOG_PREVIEW_BYTES ? KC_LLM_LOG_PREVIEW_BYTES : total;
    char preview[KC_LLM_LOG_PREVIEW_BYTES + 4];
    memcpy(preview, data, show);
    preview[show] = '\0';
    for (size_t i = 0; i < show; i++)
        if (preview[i] == '\n' || preview[i] == '\r') preview[i] = ' ';
    KC_LOGI(TAG, "%s (%u bytes): %s%s", label, (unsigned)total, preview,
            show < total ? " ..." : "");
}

/* ── libcurl 响应缓冲区 ── */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} resp_buf_t;

static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    resp_buf_t *rb = (resp_buf_t *)userdata;
    size_t bytes = size * nmemb;
    while (rb->len + bytes >= rb->cap) {
        size_t new_cap = rb->cap * 2;
        char *tmp = realloc(rb->data, new_cap);
        if (!tmp) return 0;
        rb->data = tmp;
        rb->cap = new_cap;
    }
    memcpy(rb->data + rb->len, ptr, bytes);
    rb->len += bytes;
    rb->data[rb->len] = '\0';
    return bytes;
}

/* ── 初始化 ── */

kc_err_t llm_proxy_init(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);

    const char *provider = kc_config_get_str("provider", "openai");
    safe_copy(s_provider, sizeof(s_provider), provider);

    /* 根据 provider 读对应的 key 和 url */
    if (provider_is_openai()) {
        const char *key = kc_config_get_str("openai_api_key", "");
        if (key[0]) safe_copy(s_api_key, sizeof(s_api_key), key);
        const char *url = kc_config_get_str("openai_api_url", KC_OPENAI_API_URL);
        safe_copy(s_api_url, sizeof(s_api_url), url);
    } else {
        const char *key = kc_config_get_str("anthropic_api_key", "");
        if (key[0]) safe_copy(s_api_key, sizeof(s_api_key), key);
        const char *url = kc_config_get_str("anthropic_api_url", KC_ANTHROPIC_API_URL);
        safe_copy(s_api_url, sizeof(s_api_url), url);
    }

    const char *model = kc_config_get_str("model", KC_LLM_DEFAULT_MODEL);
    safe_copy(s_model, sizeof(s_model), model);

    const char *proxy = kc_config_get_str("proxy", "");
    if (proxy[0]) safe_copy(s_proxy, sizeof(s_proxy), proxy);

    if (s_api_key[0])
        KC_LOGI(TAG, "LLM proxy initialized (provider: %s, model: %s)", s_provider, s_model);
    else
        KC_LOGW(TAG, "no API key for provider '%s'", s_provider);
    return KC_OK;
}

/* ── OpenAI 工具格式转换 ── */
/* Anthropic {name, description, input_schema} → OpenAI {type:"function", function:{...}} */

static cJSON *convert_tools_openai(const char *tools_json) {
    if (!tools_json) return NULL;
    cJSON *arr = cJSON_Parse(tools_json);
    if (!arr || !cJSON_IsArray(arr)) { cJSON_Delete(arr); return NULL; }

    cJSON *out = cJSON_CreateArray();
    cJSON *tool;
    cJSON_ArrayForEach(tool, arr) {
        cJSON *name   = cJSON_GetObjectItem(tool, "name");
        cJSON *desc   = cJSON_GetObjectItem(tool, "description");
        cJSON *schema = cJSON_GetObjectItem(tool, "input_schema");
        if (!name || !cJSON_IsString(name)) continue;

        cJSON *func = cJSON_CreateObject();
        cJSON_AddStringToObject(func, "name", name->valuestring);
        if (desc && cJSON_IsString(desc))
            cJSON_AddStringToObject(func, "description", desc->valuestring);
        if (schema)
            cJSON_AddItemToObject(func, "parameters", cJSON_Duplicate(schema, 1));

        cJSON *wrap = cJSON_CreateObject();
        cJSON_AddStringToObject(wrap, "type", "function");
        cJSON_AddItemToObject(wrap, "function", func);
        cJSON_AddItemToArray(out, wrap);
    }
    cJSON_Delete(arr);
    return out;
}

/* ── OpenAI 消息格式转换 ── */
/* Anthropic content blocks → OpenAI messages */

static cJSON *convert_messages_openai(const char *system_prompt, cJSON *messages) {
    cJSON *out = cJSON_CreateArray();

    /* system prompt → system message */
    if (system_prompt && system_prompt[0]) {
        cJSON *sys = cJSON_CreateObject();
        cJSON_AddStringToObject(sys, "role", "system");
        cJSON_AddStringToObject(sys, "content", system_prompt);
        cJSON_AddItemToArray(out, sys);
    }

    if (!messages || !cJSON_IsArray(messages)) return out;

    cJSON *msg;
    cJSON_ArrayForEach(msg, messages) {
        cJSON *role    = cJSON_GetObjectItem(msg, "role");
        cJSON *content = cJSON_GetObjectItem(msg, "content");
        if (!role || !cJSON_IsString(role)) continue;

        /* 简单字符串内容 */
        if (content && cJSON_IsString(content)) {
            cJSON *m = cJSON_CreateObject();
            cJSON_AddStringToObject(m, "role", role->valuestring);
            cJSON_AddStringToObject(m, "content", content->valuestring);
            cJSON_AddItemToArray(out, m);
            continue;
        }
        if (!content || !cJSON_IsArray(content)) continue;

        if (strcmp(role->valuestring, "assistant") == 0) {
            cJSON *m = cJSON_CreateObject();
            cJSON_AddStringToObject(m, "role", "assistant");
            char *text_buf = NULL; size_t off = 0;
            cJSON *tool_calls = NULL;
            cJSON *block;
            cJSON_ArrayForEach(block, content) {
                cJSON *btype = cJSON_GetObjectItem(block, "type");
                if (!btype || !cJSON_IsString(btype)) continue;
                if (strcmp(btype->valuestring, "text") == 0) {
                    cJSON *text = cJSON_GetObjectItem(block, "text");
                    if (text && cJSON_IsString(text)) {
                        size_t tlen = strlen(text->valuestring);
                        char *tmp = realloc(text_buf, off + tlen + 1);
                        if (tmp) { text_buf = tmp; memcpy(text_buf + off, text->valuestring, tlen); off += tlen; text_buf[off] = '\0'; }
                    }
                } else if (strcmp(btype->valuestring, "tool_use") == 0) {
                    if (!tool_calls) tool_calls = cJSON_CreateArray();
                    cJSON *id = cJSON_GetObjectItem(block, "id");
                    cJSON *name = cJSON_GetObjectItem(block, "name");
                    cJSON *input = cJSON_GetObjectItem(block, "input");
                    if (!name || !cJSON_IsString(name)) continue;
                    cJSON *tc = cJSON_CreateObject();
                    if (id && cJSON_IsString(id)) cJSON_AddStringToObject(tc, "id", id->valuestring);
                    cJSON_AddStringToObject(tc, "type", "function");
                    cJSON *func = cJSON_CreateObject();
                    cJSON_AddStringToObject(func, "name", name->valuestring);
                    if (input) { char *args = cJSON_PrintUnformatted(input); if (args) { cJSON_AddStringToObject(func, "arguments", args); free(args); } }
                    cJSON_AddItemToObject(tc, "function", func);
                    cJSON_AddItemToArray(tool_calls, tc);
                }
            }
            cJSON_AddStringToObject(m, "content", text_buf ? text_buf : "");
            if (tool_calls) cJSON_AddItemToObject(m, "tool_calls", tool_calls);
            cJSON_AddItemToArray(out, m);
            free(text_buf);

        } else if (strcmp(role->valuestring, "user") == 0) {
            char *text_buf = NULL; size_t off = 0; bool has_text = false;
            cJSON *block;
            cJSON_ArrayForEach(block, content) {
                cJSON *btype = cJSON_GetObjectItem(block, "type");
                if (!btype || !cJSON_IsString(btype)) continue;
                if (strcmp(btype->valuestring, "tool_result") == 0) {
                    cJSON *tool_id  = cJSON_GetObjectItem(block, "tool_use_id");
                    cJSON *tcontent = cJSON_GetObjectItem(block, "content");
                    if (!tool_id || !cJSON_IsString(tool_id)) continue;
                    cJSON *tm = cJSON_CreateObject();
                    cJSON_AddStringToObject(tm, "role", "tool");
                    cJSON_AddStringToObject(tm, "tool_call_id", tool_id->valuestring);
                    /*
                     * OpenAI tool result 只支持字符串 content。
                     * Anthropic 的 content 可能是字符串或数组（含 image block）。
                     * 数组时提取文本部分，图片降级为 "[Image attached]" 描述。
                     */
                    if (tcontent && cJSON_IsString(tcontent)) {
                        cJSON_AddStringToObject(tm, "content", tcontent->valuestring);
                    } else if (tcontent && cJSON_IsArray(tcontent)) {
                        /* 从 content 数组中提取文本，图片降级 */
                        char *combined = NULL; size_t coff = 0;
                        cJSON *sub;
                        cJSON_ArrayForEach(sub, tcontent) {
                            cJSON *stype = cJSON_GetObjectItem(sub, "type");
                            if (!stype || !cJSON_IsString(stype)) continue;
                            if (strcmp(stype->valuestring, "text") == 0) {
                                cJSON *st = cJSON_GetObjectItem(sub, "text");
                                if (st && cJSON_IsString(st)) {
                                    size_t slen = strlen(st->valuestring);
                                    char *tmp = realloc(combined, coff + slen + 1);
                                    if (tmp) { combined = tmp; memcpy(combined + coff, st->valuestring, slen); coff += slen; combined[coff] = '\0'; }
                                }
                            } else if (strcmp(stype->valuestring, "image") == 0) {
                                const char *note = "[Image attached]";
                                size_t nlen = strlen(note);
                                char *tmp = realloc(combined, coff + nlen + 2);
                                if (tmp) { combined = tmp; if (coff > 0) combined[coff++] = '\n'; memcpy(combined + coff, note, nlen); coff += nlen; combined[coff] = '\0'; }
                            }
                        }
                        cJSON_AddStringToObject(tm, "content", combined ? combined : "");
                        free(combined);
                    } else {
                        cJSON_AddStringToObject(tm, "content", "");
                    }
                    cJSON_AddItemToArray(out, tm);
                } else if (strcmp(btype->valuestring, "text") == 0) {
                    cJSON *text = cJSON_GetObjectItem(block, "text");
                    if (text && cJSON_IsString(text)) {
                        size_t tlen = strlen(text->valuestring);
                        char *tmp = realloc(text_buf, off + tlen + 1);
                        if (tmp) { text_buf = tmp; memcpy(text_buf + off, text->valuestring, tlen); off += tlen; text_buf[off] = '\0'; }
                        has_text = true;
                    }
                }
            }
            if (has_text && text_buf) {
                cJSON *um = cJSON_CreateObject();
                cJSON_AddStringToObject(um, "role", "user");
                cJSON_AddStringToObject(um, "content", text_buf);
                cJSON_AddItemToArray(out, um);
            }
            free(text_buf);
        }
    }
    return out;
}

/* ── HTTP 调用 (libcurl) ── */

/* progress callback：关停时中断 curl 传输 */
static int curl_xferinfo_cb(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                             curl_off_t ultotal, curl_off_t ulnow) {
    (void)clientp; (void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;
    return kc_is_shutting_down() ? 1 : 0;  /* 非零 = 中断传输 */
}

static kc_err_t llm_http_call(const char *post_data, resp_buf_t *rb, int *out_status,
                              const char *api_url, const char *api_key,
                              const char *proxy, int is_openai) {
    CURL *curl = curl_easy_init();
    if (!curl) return KC_FAIL;

    curl_easy_setopt(curl, CURLOPT_URL, api_url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, rb);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curl_xferinfo_cb);

    if (proxy[0])
        curl_easy_setopt(curl, CURLOPT_PROXY, proxy);

    /* HTTP 头：根据 provider 不同 */
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    char auth_buf[LLM_API_KEY_MAX_LEN + 64];
    if (is_openai) {
        snprintf(auth_buf, sizeof(auth_buf), "Authorization: Bearer %s", api_key);
        headers = curl_slist_append(headers, auth_buf);
    } else {
        snprintf(auth_buf, sizeof(auth_buf), "x-api-key: %s", api_key);
        headers = curl_slist_append(headers, auth_buf);
        headers = curl_slist_append(headers, "anthropic-version: " ANTHROPIC_API_VERSION);
    }

    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    *out_status = (int)http_code;

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        KC_LOGE(TAG, "curl failed: %s", curl_easy_strerror(res));
        return KC_FAIL;
    }
    return KC_OK;
}

/* ── OpenAI 响应解析 ── */

static kc_err_t parse_openai_response(const char *json_str, llm_response_t *resp) {
    cJSON *root = cJSON_Parse(json_str);
    if (!root) { KC_LOGE(TAG, "JSON parse failed"); return KC_FAIL; }

    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    cJSON *choice0 = (choices && cJSON_IsArray(choices)) ? cJSON_GetArrayItem(choices, 0) : NULL;
    if (!choice0) {
        cJSON *error = cJSON_GetObjectItem(root, "error");
        if (error) { cJSON *em = cJSON_GetObjectItem(error, "message"); if (em && cJSON_IsString(em)) KC_LOGE(TAG, "API error: %s", em->valuestring); }
        cJSON_Delete(root); return KC_FAIL;
    }

    cJSON *finish = cJSON_GetObjectItem(choice0, "finish_reason");
    if (finish && cJSON_IsString(finish))
        resp->tool_use = (strcmp(finish->valuestring, "tool_calls") == 0);

    cJSON *message = cJSON_GetObjectItem(choice0, "message");
    if (!message) { cJSON_Delete(root); return KC_OK; }

    /* 文本 */
    cJSON *content = cJSON_GetObjectItem(message, "content");
    if (content && cJSON_IsString(content)) {
        size_t tlen = strlen(content->valuestring);
        resp->text = calloc(1, tlen + 1);
        if (resp->text) { memcpy(resp->text, content->valuestring, tlen); resp->text_len = tlen; }
    }

    /* 工具调用 */
    cJSON *tool_calls = cJSON_GetObjectItem(message, "tool_calls");
    if (tool_calls && cJSON_IsArray(tool_calls)) {
        cJSON *tc;
        cJSON_ArrayForEach(tc, tool_calls) {
            if (resp->call_count >= KC_MAX_TOOL_CALLS) break;
            llm_tool_call_t *call = &resp->calls[resp->call_count];
            cJSON *id   = cJSON_GetObjectItem(tc, "id");
            cJSON *func = cJSON_GetObjectItem(tc, "function");
            if (id && cJSON_IsString(id)) strncpy(call->id, id->valuestring, sizeof(call->id) - 1);
            if (func) {
                cJSON *name = cJSON_GetObjectItem(func, "name");
                cJSON *args = cJSON_GetObjectItem(func, "arguments");
                if (name && cJSON_IsString(name)) strncpy(call->name, name->valuestring, sizeof(call->name) - 1);
                if (args && cJSON_IsString(args)) { call->input = strdup(args->valuestring); if (call->input) call->input_len = strlen(call->input); }
            }
            resp->call_count++;
        }
        if (resp->call_count > 0) resp->tool_use = true;
    }

    cJSON_Delete(root);
    return KC_OK;
}

/* ── Anthropic 响应解析 ── */

static kc_err_t parse_anthropic_response(const char *json_str, llm_response_t *resp) {
    cJSON *root = cJSON_Parse(json_str);
    if (!root) { KC_LOGE(TAG, "JSON parse failed"); return KC_FAIL; }

    /* 检查 API 错误 */
    cJSON *err_type = cJSON_GetObjectItem(root, "type");
    if (err_type && cJSON_IsString(err_type) && strcmp(err_type->valuestring, "error") == 0) {
        cJSON *error = cJSON_GetObjectItem(root, "error");
        if (error) { cJSON *em = cJSON_GetObjectItem(error, "message"); if (em && cJSON_IsString(em)) KC_LOGE(TAG, "API error: %s", em->valuestring); }
        cJSON_Delete(root); return KC_FAIL;
    }

    /* stop_reason */
    cJSON *stop_reason = cJSON_GetObjectItem(root, "stop_reason");
    if (stop_reason && cJSON_IsString(stop_reason))
        resp->tool_use = (strcmp(stop_reason->valuestring, "tool_use") == 0);

    /* content blocks */
    cJSON *content = cJSON_GetObjectItem(root, "content");
    if (content && cJSON_IsArray(content)) {
        /* 先计算总文本长度 */
        size_t total_text = 0;
        cJSON *block;
        cJSON_ArrayForEach(block, content) {
            cJSON *btype = cJSON_GetObjectItem(block, "type");
            if (btype && strcmp(btype->valuestring, "text") == 0) {
                cJSON *text = cJSON_GetObjectItem(block, "text");
                if (text && cJSON_IsString(text)) total_text += strlen(text->valuestring);
            }
        }

        /* 分配并拷贝文本 */
        if (total_text > 0) {
            resp->text = calloc(1, total_text + 1);
            if (resp->text) {
                cJSON_ArrayForEach(block, content) {
                    cJSON *btype = cJSON_GetObjectItem(block, "type");
                    if (!btype || strcmp(btype->valuestring, "text") != 0) continue;
                    cJSON *text = cJSON_GetObjectItem(block, "text");
                    if (!text || !cJSON_IsString(text)) continue;
                    size_t tlen = strlen(text->valuestring);
                    memcpy(resp->text + resp->text_len, text->valuestring, tlen);
                    resp->text_len += tlen;
                }
                resp->text[resp->text_len] = '\0';
            }
        }

        /* 提取 tool_use blocks */
        cJSON_ArrayForEach(block, content) {
            cJSON *btype = cJSON_GetObjectItem(block, "type");
            if (!btype || strcmp(btype->valuestring, "tool_use") != 0) continue;
            if (resp->call_count >= KC_MAX_TOOL_CALLS) break;
            llm_tool_call_t *call = &resp->calls[resp->call_count];

            cJSON *id   = cJSON_GetObjectItem(block, "id");
            cJSON *name = cJSON_GetObjectItem(block, "name");
            cJSON *input = cJSON_GetObjectItem(block, "input");
            if (id && cJSON_IsString(id)) strncpy(call->id, id->valuestring, sizeof(call->id) - 1);
            if (name && cJSON_IsString(name)) strncpy(call->name, name->valuestring, sizeof(call->name) - 1);
            if (input) { char *s = cJSON_PrintUnformatted(input); if (s) { call->input = s; call->input_len = strlen(s); } }
            resp->call_count++;
        }
    }

    cJSON_Delete(root);
    return KC_OK;
}

/* ── 公共 API ── */

void llm_response_free(llm_response_t *resp) {
    free(resp->text); resp->text = NULL; resp->text_len = 0;
    for (int i = 0; i < resp->call_count; i++) { free(resp->calls[i].input); resp->calls[i].input = NULL; }
    resp->call_count = 0; resp->tool_use = false;
}

kc_err_t llm_chat_tools(const char *system_prompt,
                        cJSON *messages,
                        const char *tools_json,
                        llm_response_t *resp)
{
    memset(resp, 0, sizeof(*resp));

    /* 加锁拷贝配置到局部变量，解锁后使用局部变量 */
    char l_api_key[LLM_API_KEY_MAX_LEN];
    char l_model[LLM_MODEL_MAX_LEN];
    char l_provider[16];
    char l_api_url[256];
    char l_proxy[256];

    pthread_mutex_lock(&s_llm_mutex);
    memcpy(l_api_key, s_api_key, sizeof(l_api_key));
    memcpy(l_model, s_model, sizeof(l_model));
    memcpy(l_provider, s_provider, sizeof(l_provider));
    memcpy(l_api_url, s_api_url, sizeof(l_api_url));
    memcpy(l_proxy, s_proxy, sizeof(l_proxy));
    pthread_mutex_unlock(&s_llm_mutex);

    int is_openai = (strcmp(l_provider, "openai") == 0);

    if (l_api_key[0] == '\0') { KC_LOGE(TAG, "no API key"); return KC_ERR_NO_KEY; }

    /* 构建请求体 */
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "model", l_model);

    if (is_openai) {
        /* OpenAI 格式 */
        cJSON_AddNumberToObject(body, "max_completion_tokens", KC_LLM_MAX_TOKENS);
        cJSON_AddItemToObject(body, "messages", convert_messages_openai(system_prompt, messages));
        if (tools_json) {
            cJSON *tools = convert_tools_openai(tools_json);
            if (tools) { cJSON_AddItemToObject(body, "tools", tools); cJSON_AddStringToObject(body, "tool_choice", "auto"); }
        }
    } else {
        /* Anthropic 格式（原生） */
        cJSON_AddNumberToObject(body, "max_tokens", KC_LLM_MAX_TOKENS);
        cJSON_AddStringToObject(body, "system", system_prompt);
        cJSON_AddItemToObject(body, "messages", cJSON_Duplicate(messages, 1));
        if (tools_json) {
            cJSON *tools = cJSON_Parse(tools_json);
            if (tools) cJSON_AddItemToObject(body, "tools", tools);
        }
    }

    char *post_data = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!post_data) return KC_ERR_NO_MEM;

    KC_LOGI(TAG, "calling %s API (model: %s, %d bytes)", l_provider, l_model, (int)strlen(post_data));
    log_preview("request", post_data);

    /* 响应缓冲区 */
    resp_buf_t rb = {0};
    rb.data = calloc(1, KC_LLM_STREAM_BUF_SIZE);
    if (!rb.data) { free(post_data); return KC_ERR_NO_MEM; }
    rb.cap = KC_LLM_STREAM_BUF_SIZE;

    /* HTTP 调用（含重试） */
    int status = 0;
    kc_err_t err;
    int max_retries = 2;
    int delay_ms = 2000;

    for (int attempt = 0; attempt <= max_retries; attempt++) {
        if (kc_is_shutting_down()) { err = KC_FAIL; break; }
        rb.len = 0;
        status = 0;
        err = llm_http_call(post_data, &rb, &status,
                            l_api_url, l_api_key, l_proxy, is_openai);
        /* 只重试网络错误和 429/502/503 */
        if (err == KC_OK && status != 429 && status != 502 && status != 503)
            break;
        if (attempt < max_retries && !kc_is_shutting_down()) {
            KC_LOGW(TAG, "retry %d/%d in %dms (err=%d, status=%d)",
                    attempt + 1, max_retries, delay_ms, err, status);
            usleep(delay_ms * 1000);
            delay_ms *= 2;
        }
    }
    free(post_data);

    if (err != KC_OK) { log_preview("partial response", rb.data); free(rb.data); return KC_ERR_NET; }
    log_preview("response", rb.data);

    if (status == 401 || status == 403) {
        KC_LOGE(TAG, "auth error %d: %.500s", status, rb.data ? rb.data : "");
        free(rb.data); return KC_ERR_AUTH;
    }
    if (status != 200) {
        KC_LOGE(TAG, "API error %d: %.500s", status, rb.data ? rb.data : "");
        free(rb.data); return KC_ERR_API;
    }

    /* 按 provider 解析响应 */
    err = is_openai ? parse_openai_response(rb.data, resp)
                    : parse_anthropic_response(rb.data, resp);
    free(rb.data);

    if (err == KC_OK)
        KC_LOGI(TAG, "response: %d bytes text, %d tool calls, tool_use=%s",
                (int)resp->text_len, resp->call_count, resp->tool_use ? "yes" : "no");
    return err;
}

/* ── 配置修改 ── */

kc_err_t llm_set_api_key(const char *api_key) {
    pthread_mutex_lock(&s_llm_mutex);
    safe_copy(s_api_key, sizeof(s_api_key), api_key);
    const char *key_name = provider_is_openai() ? "openai_api_key" : "anthropic_api_key";
    pthread_mutex_unlock(&s_llm_mutex);
    kc_config_set_str(key_name, api_key);
    kc_config_save();
    KC_LOGI(TAG, "API key saved");
    return KC_OK;
}

kc_err_t llm_set_model(const char *model) {
    pthread_mutex_lock(&s_llm_mutex);
    safe_copy(s_model, sizeof(s_model), model);
    pthread_mutex_unlock(&s_llm_mutex);
    kc_config_set_str("model", model);
    kc_config_save();
    KC_LOGI(TAG, "model set to: %s", model);
    return KC_OK;
}

kc_err_t llm_set_provider(const char *provider) {
    pthread_mutex_lock(&s_llm_mutex);
    safe_copy(s_provider, sizeof(s_provider), provider);
    /* 切换 provider 后重新加载对应的 key 和 url */
    if (provider_is_openai()) {
        const char *key = kc_config_get_str("openai_api_key", "");
        if (key[0]) safe_copy(s_api_key, sizeof(s_api_key), key);
        const char *url = kc_config_get_str("openai_api_url", KC_OPENAI_API_URL);
        safe_copy(s_api_url, sizeof(s_api_url), url);
    } else {
        const char *key = kc_config_get_str("anthropic_api_key", "");
        if (key[0]) safe_copy(s_api_key, sizeof(s_api_key), key);
        const char *url = kc_config_get_str("anthropic_api_url", KC_ANTHROPIC_API_URL);
        safe_copy(s_api_url, sizeof(s_api_url), url);
    }
    pthread_mutex_unlock(&s_llm_mutex);
    kc_config_set_str("provider", provider);
    kc_config_save();
    KC_LOGI(TAG, "provider set to: %s", provider);
    return KC_OK;
}

void llm_set_api_url(const char *url) {
    if (!url) return;
    pthread_mutex_lock(&s_llm_mutex);
    safe_copy(s_api_url, sizeof(s_api_url), url);
    pthread_mutex_unlock(&s_llm_mutex);
    /* 保存到对应 provider 的配置项 */
    const char *key = provider_is_openai() ? "openai_api_url" : "anthropic_api_url";
    kc_config_set_str(key, url);
    KC_LOGI(TAG, "API URL set to: %s", url);
}

void llm_set_proxy(const char *proxy) {
    pthread_mutex_lock(&s_llm_mutex);
    safe_copy(s_proxy, sizeof(s_proxy), proxy ? proxy : "");
    pthread_mutex_unlock(&s_llm_mutex);
    kc_config_set_str("proxy", proxy ? proxy : "");
    KC_LOGI(TAG, "proxy set to: %s", proxy && proxy[0] ? proxy : "(none)");
}
