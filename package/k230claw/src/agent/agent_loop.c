/*
 * agent_loop.c - ReAct 循环 (pthread 版)
 *
 * Phase 3 PART 1: 使用 kc_tool_result_t 结构化工具返回值
 *   - 移除固定 TOOL_OUTPUT_SIZE 缓冲区
 *   - 支持 is_error, for_user, image_base64, response_handled, silent
 *   - Anthropic image content block 支持
 */

#include "agent_loop.h"
#include "context_builder.h"
#include "../kc_config.h"
#include "../bus/message_bus.h"
#include "../llm/llm_proxy.h"
#include "../memory/session_mgr.h"
#include "../tools/tool_registry.h"
#include "../tools/tool_cron.h"
#include "../sensors/sensor_state.h"
#include "../skills/skill_loader.h"

#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include "../third_party/cJSON/cJSON.h"

#define TAG "agent"

/* ── Token 估算与上下文裁剪 ── */

static int estimate_tokens(const char *text) {
    if (!text) return 0;
    return (int)(strlen(text) / 4);
}

static int estimate_msg_tokens(cJSON *msg) {
    int tokens = 4;
    cJSON *content = cJSON_GetObjectItem(msg, "content");
    if (content && cJSON_IsString(content))
        tokens += estimate_tokens(content->valuestring);
    else if (content && cJSON_IsArray(content)) {
        cJSON *block;
        cJSON_ArrayForEach(block, content) {
            cJSON *text = cJSON_GetObjectItem(block, "text");
            if (text && cJSON_IsString(text))
                tokens += estimate_tokens(text->valuestring);
            cJSON *tc = cJSON_GetObjectItem(block, "content");
            if (tc && cJSON_IsString(tc))
                tokens += estimate_tokens(tc->valuestring);
            cJSON *input = cJSON_GetObjectItem(block, "input");
            if (input) {
                char *s = cJSON_PrintUnformatted(input);
                if (s) { tokens += estimate_tokens(s); free(s); }
            }
        }
    }
    return tokens;
}

/*
 * 裁剪 messages 使总 token 不超预算。
 * 从最旧端删除完整 turn（user + assistant 配对），
 * 绝不拆分 tool_use/tool_result 序列。
 */
static void trim_context(cJSON *messages, int system_tokens, int budget) {
    int available = budget - system_tokens - KC_LLM_MAX_TOKENS;
    if (available <= 0) available = budget / 2;

    int total = 0;
    int count = cJSON_GetArraySize(messages);
    int original_count = count;
    for (int i = 0; i < count; i++)
        total += estimate_msg_tokens(cJSON_GetArrayItem(messages, i));

    while (total > available && count > 1) {
        cJSON *first = cJSON_GetArrayItem(messages, 0);
        total -= estimate_msg_tokens(first);
        cJSON_DeleteItemFromArray(messages, 0);
        count--;

        while (count > 1) {
            cJSON *next = cJSON_GetArrayItem(messages, 0);
            cJSON *role = cJSON_GetObjectItem(next, "role");
            cJSON *content = cJSON_GetObjectItem(next, "content");
            if (role && cJSON_IsString(role) && strcmp(role->valuestring, "user") == 0 &&
                content && cJSON_IsArray(content)) {
                cJSON *fb = cJSON_GetArrayItem(content, 0);
                cJSON *ftype = fb ? cJSON_GetObjectItem(fb, "type") : NULL;
                if (ftype && cJSON_IsString(ftype) && strcmp(ftype->valuestring, "tool_result") == 0) {
                    total -= estimate_msg_tokens(next);
                    cJSON_DeleteItemFromArray(messages, 0);
                    count--;
                    continue;
                }
            }
            if (role && cJSON_IsString(role) && strcmp(role->valuestring, "assistant") == 0 &&
                content && cJSON_IsArray(content)) {
                total -= estimate_msg_tokens(next);
                cJSON_DeleteItemFromArray(messages, 0);
                count--;
                continue;
            }
            break;
        }
    }

    if (count < original_count)
        KC_LOGI(TAG, "context trimmed: %d→%d messages, ~%d tokens remaining",
                original_count, count, total);
}

/* 构建 assistant content 数组（text + tool_use blocks） */
static cJSON *build_assistant_content(const llm_response_t *resp)
{
    cJSON *content = cJSON_CreateArray();

    if (resp->text && resp->text_len > 0) {
        cJSON *tb = cJSON_CreateObject();
        cJSON_AddStringToObject(tb, "type", "text");
        cJSON_AddStringToObject(tb, "text", resp->text);
        cJSON_AddItemToArray(content, tb);
    }

    for (int i = 0; i < resp->call_count; i++) {
        const llm_tool_call_t *call = &resp->calls[i];
        cJSON *tb = cJSON_CreateObject();
        cJSON_AddStringToObject(tb, "type", "tool_use");
        cJSON_AddStringToObject(tb, "id", call->id);
        cJSON_AddStringToObject(tb, "name", call->name);
        cJSON *input = cJSON_Parse(call->input);
        cJSON_AddItemToObject(tb, "input", input ? input : cJSON_CreateObject());
        cJSON_AddItemToArray(content, tb);
    }
    return content;
}

/*
 * 执行所有工具，构建 Anthropic tool_result blocks。
 * 返回值:
 *   - content: cJSON array of tool_result blocks
 *   - *out_has_for_user: 是否有工具设置了 for_user（需要推送到用户通道）
 *   - *out_response_handled: 是否有工具标记 response_handled
 *   - *out_user_text: for_user 文本（调用者 free）
 */
typedef struct {
    cJSON *content;
    bool   has_for_user;
    bool   response_handled;
    char  *user_text;       /* malloc, caller free */
} tool_exec_result_t;

static tool_exec_result_t build_tool_results(const llm_response_t *resp)
{
    tool_exec_result_t out = {0};
    out.content = cJSON_CreateArray();

    for (int i = 0; i < resp->call_count; i++) {
        const llm_tool_call_t *call = &resp->calls[i];
        const char *input = call->input ? call->input : "{}";

        kc_tool_result_t *result = tool_registry_execute(call->name, input);
        if (!result) {
            result = tool_result_error("Tool execution returned NULL");
        }

        KC_LOGI(TAG, "tool %s: content=%d bytes, is_error=%d, silent=%d, handled=%d",
                call->name,
                result->content ? (int)strlen(result->content) : 0,
                result->is_error, result->silent, result->response_handled);

        /* 构建 Anthropic tool_result block */
        cJSON *rb = cJSON_CreateObject();
        cJSON_AddStringToObject(rb, "type", "tool_result");
        cJSON_AddStringToObject(rb, "tool_use_id", call->id);

        if (result->is_error) {
            cJSON_AddBoolToObject(rb, "is_error", 1);
        }

        if (result->image_base64 && result->image_base64[0]) {
            /*
             * Anthropic tool_result 支持 content 数组，含 image block。
             * 格式: content: [{type:"image", source:{type:"base64", media_type:..., data:...}}, {type:"text", text:...}]
             */
            cJSON *content_arr = cJSON_CreateArray();

            /* 图片 block */
            cJSON *img_block = cJSON_CreateObject();
            cJSON_AddStringToObject(img_block, "type", "image");
            cJSON *source = cJSON_CreateObject();
            cJSON_AddStringToObject(source, "type", "base64");
            cJSON_AddStringToObject(source, "media_type",
                                    result->image_media_type ? result->image_media_type : "image/jpeg");
            cJSON_AddStringToObject(source, "data", result->image_base64);
            cJSON_AddItemToObject(img_block, "source", source);
            cJSON_AddItemToArray(content_arr, img_block);

            /* 文本 block（如有） */
            if (result->content && result->content[0]) {
                cJSON *text_block = cJSON_CreateObject();
                cJSON_AddStringToObject(text_block, "type", "text");
                cJSON_AddStringToObject(text_block, "text", result->content);
                cJSON_AddItemToArray(content_arr, text_block);
            }

            cJSON_AddItemToObject(rb, "content", content_arr);
        } else {
            /* 纯文本结果 */
            cJSON_AddStringToObject(rb, "content",
                                    result->content ? result->content : "");
        }

        cJSON_AddItemToArray(out.content, rb);

        /* 处理 for_user: 推送到用户通道 */
        if (result->for_user && result->for_user[0] && !result->silent) {
            out.has_for_user = true;
            /* 多个工具的 for_user 拼接 */
            if (out.user_text) {
                size_t old_len = strlen(out.user_text);
                size_t new_len = strlen(result->for_user);
                char *combined = realloc(out.user_text, old_len + new_len + 2);
                if (combined) {
                    combined[old_len] = '\n';
                    memcpy(combined + old_len + 1, result->for_user, new_len + 1);
                    out.user_text = combined;
                }
            } else {
                out.user_text = strdup(result->for_user);
            }
        }

        /* response_handled: 任一工具标记即生效 */
        if (result->response_handled)
            out.response_handled = true;

        tool_result_free(result);
    }
    return out;
}

/* 追加当前轮次上下文和传感器状态到 system prompt */
static const char *channel_display_name(const char *ch)
{
    if (strcmp(ch, "cli") == 0)       return "Serial CLI (K230 console)";
    if (strcmp(ch, "websocket") == 0) return "Web UI (browser)";
    if (strcmp(ch, "telegram") == 0)  return "Telegram Bot";
    if (strcmp(ch, "qq") == 0)        return "QQ Bot";
    if (strcmp(ch, "system") == 0)    return "System (internal event)";
    return ch;
}

static void append_turn_context(char *prompt, size_t size, const kc_msg_t *msg)
{
    size_t off = strlen(prompt);
    if (off >= size - 1) return;
    off += snprintf(prompt + off, size - off,
        "\n## Current Turn Context\n"
        "The user is currently talking to you from: **%s**.\n"
        "- source_channel: %s\n"
        "- source_chat_id: %s\n"
        "You already know which interface the user is on. "
        "Do NOT run commands to figure it out.\n",
        msg->channel[0] ? channel_display_name(msg->channel) : "unknown",
        msg->channel[0] ? msg->channel : "(unknown)",
        msg->chat_id[0] ? msg->chat_id : "(empty)");

    /* 传感器状态注入 */
    char *sensor_summary = sensor_state_summary();
    if (sensor_summary) {
        off = strlen(prompt);
        if (off < size - 1)
            snprintf(prompt + off, size - off, "\n%s", sensor_summary);
        free(sensor_summary);
    }
}

/* 发送 outbound 消息到当前通道 */
static void send_outbound(const kc_msg_t *src, const char *text)
{
    if (!text || !text[0]) return;
    kc_msg_t out = {0};
    strncpy(out.channel, src->channel, sizeof(out.channel) - 1);
    strncpy(out.chat_id, src->chat_id, sizeof(out.chat_id) - 1);
    out.content = strdup(text);
    if (!out.content) return;
    if (message_bus_push_outbound(&out) != KC_OK)
        free(out.content);
}

/* ── 主循环线程 ── */

static void *agent_loop_task(void *arg)
{
    (void)arg;
    KC_LOGI(TAG, "agent loop started");

    char *system_prompt = calloc(1, KC_CONTEXT_BUF_SIZE);

    if (!system_prompt) {
        KC_LOGE(TAG, "failed to allocate system_prompt buffer");
        return NULL;
    }

    while (!kc_is_shutting_down()) {
        kc_msg_t msg;
        if (message_bus_pop_inbound(&msg, UINT32_MAX) != KC_OK) continue;

        /* 每轮重新获取（角色切换可能改变可用工具） */
        const char *tools_json = tool_registry_get_tools_json();
        if (kc_is_shutting_down()) { free(msg.content); break; }

        KC_LOGI(TAG, "processing message from %s:%s (type=%d)", msg.channel, msg.chat_id, msg.msg_type);

        /* 0. 设置当前通道上下文（供 cron 等工具使用） */
        tool_cron_set_channel(msg.channel);

        /* 1. 构建 system prompt */
        context_build_system_prompt(system_prompt, KC_CONTEXT_BUF_SIZE);
        append_turn_context(system_prompt, KC_CONTEXT_BUF_SIZE, &msg);

        /* 1.5. 技能触发：匹配用户消息关键词，注入完整技能内容 */
        if (msg.msg_type == KC_MSG_TYPE_CHAT && msg.content) {
            char *skill_content = skill_loader_match_triggers(msg.content);
            if (skill_content) {
                size_t off = strlen(system_prompt);
                snprintf(system_prompt + off, KC_CONTEXT_BUF_SIZE - off,
                         "\n## Active Skill\n%s\n", skill_content);
                free(skill_content);
            }
        }

        /* 2. 加载会话历史（直接返回 cJSON，无截断风险） */
        cJSON *messages = session_get_history(msg.chat_id, KC_AGENT_MAX_HISTORY);

        /* 3. 追加当前消息（记录新消息起始位置，用于会话保存） */
        int history_msg_count = cJSON_GetArraySize(messages);
        cJSON *user_msg = cJSON_CreateObject();
        cJSON_AddStringToObject(user_msg, "role", "user");

        if (msg.msg_type == KC_MSG_TYPE_EVENT) {
            /* 事件消息：包装为系统事件通知 */
            char event_text[512];
            snprintf(event_text, sizeof(event_text),
                     "[System Event: %s] %s",
                     msg.event_name[0] ? msg.event_name : "unknown",
                     msg.content ? msg.content : "");
            cJSON_AddStringToObject(user_msg, "content", event_text);
        } else {
            cJSON_AddStringToObject(user_msg, "content", msg.content);
        }
        cJSON_AddItemToArray(messages, user_msg);

        /* 4. 上下文裁剪 */
        {
            const char *budget_str = kc_config_get_str("context_budget", "");
            int budget = budget_str[0] ? atoi(budget_str) : KC_CONTEXT_TOKEN_BUDGET;
            if (budget < 10000) budget = KC_CONTEXT_TOKEN_BUDGET;
            int sys_tokens = estimate_tokens(system_prompt);
            trim_context(messages, sys_tokens, budget);
        }

        /* 5. ReAct 循环 */
        char *final_text = NULL;
        bool is_error = false;
        bool response_handled = false;
        int iteration = 0;

        while (iteration < KC_AGENT_MAX_TOOL_ITER) {
            llm_response_t resp;
            kc_err_t err = llm_chat_tools(system_prompt, messages, tools_json, &resp);
            if (err != KC_OK) {
                KC_LOGE(TAG, "LLM call failed (err=%d)", err);
                const char *errmsg;
                switch (err) {
                case KC_ERR_NO_KEY: errmsg = "No API key configured. Check your config file."; break;
                case KC_ERR_AUTH:   errmsg = "Invalid API key (authentication failed)."; break;
                case KC_ERR_NET:    errmsg = "Network error. Use /reconnect to recheck."; break;
                case KC_ERR_API:    errmsg = "API returned an error. Check logs for details."; break;
                default:            errmsg = "Sorry, I encountered an error."; break;
                }
                free(final_text);
                final_text = strdup(errmsg);
                is_error = true;
                break;
            }

            if (!resp.tool_use) {
                if (resp.text && resp.text_len > 0)
                    final_text = strdup(resp.text);
                llm_response_free(&resp);
                break;
            }

            KC_LOGI(TAG, "tool iteration %d: %d calls", iteration + 1, resp.call_count);

            /* assistant 消息 (text + tool_use) */
            cJSON *asst = cJSON_CreateObject();
            cJSON_AddStringToObject(asst, "role", "assistant");
            cJSON_AddItemToObject(asst, "content", build_assistant_content(&resp));
            cJSON_AddItemToArray(messages, asst);

            /* 执行工具，构建 tool_result */
            tool_exec_result_t exec = build_tool_results(&resp);

            cJSON *result_msg = cJSON_CreateObject();
            cJSON_AddStringToObject(result_msg, "role", "user");
            cJSON_AddItemToObject(result_msg, "content", exec.content);
            cJSON_AddItemToArray(messages, result_msg);

            /* for_user: 工具直接输出给用户的文本 */
            if (exec.has_for_user && exec.user_text) {
                send_outbound(&msg, exec.user_text);
            }
            free(exec.user_text);

            /* response_handled: 工具已完成用户请求，可跳过后续 LLM 回复 */
            if (exec.response_handled) {
                response_handled = true;
                llm_response_free(&resp);
                break;
            }

            llm_response_free(&resp);
            iteration++;
        }

        /* 5.5 迭代上限兜底：追加 assistant 消息，避免 session 末尾是孤立的 tool_result */
        if (iteration >= KC_AGENT_MAX_TOOL_ITER && !final_text && !is_error) {
            final_text = strdup("I've reached the maximum number of tool iterations. Please try again or simplify your request.");
            /* 追加到 messages 中，确保 session 保存时序列完整 */
            cJSON *asst = cJSON_CreateObject();
            cJSON_AddStringToObject(asst, "role", "assistant");
            cJSON_AddStringToObject(asst, "content", final_text);
            cJSON_AddItemToArray(messages, asst);
        }

        /* 6. 保存新消息到会话文件（含完整 tool_use/tool_result 结构） */
        if (!is_error && messages) {
            int total_msgs = cJSON_GetArraySize(messages);
            /* 只保存本轮新增的消息（从 history_msg_count 开始） */
            for (int i = history_msg_count; i < total_msgs; i++) {
                cJSON *m = cJSON_GetArrayItem(messages, i);
                session_append_message(msg.chat_id, m);
            }
            /* 如果 LLM 有最终文本回复但不在 messages 中，单独保存 */
            if (final_text && final_text[0] && !response_handled) {
                session_append(msg.chat_id, "assistant", final_text);
            }
        }

        cJSON_Delete(messages);

        if (response_handled) {
            /* 工具已直接回复用户，不再发 LLM 回复 */
        } else if (final_text && final_text[0]) {
            send_outbound(&msg, final_text);
        } else {
            send_outbound(&msg, "Sorry, I encountered an error.");
        }
        free(final_text);

        free(msg.content);
    }

    free(system_prompt);
    return NULL;
}

static pthread_t s_agent_tid;

kc_err_t agent_loop_init(void)
{
    KC_LOGI(TAG, "agent loop initialized");
    return KC_OK;
}

kc_err_t agent_loop_start(void)
{
    if (pthread_create(&s_agent_tid, NULL, agent_loop_task, NULL) != 0) {
        KC_LOGE(TAG, "failed to create agent thread");
        return KC_FAIL;
    }
    KC_LOGI(TAG, "agent loop thread started");
    return KC_OK;
}

void agent_loop_stop(void)
{
    KC_LOGI(TAG, "stopping agent loop...");
    pthread_join(s_agent_tid, NULL);
    KC_LOGI(TAG, "agent loop stopped");
}
