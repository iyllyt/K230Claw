#pragma once

/*
 * session_mgr.h - JSONL 会话历史管理
 *
 * Phase 3 PART 4: 支持保存完整 cJSON 消息（含 tool_use/tool_result content 数组）。
 * 向后兼容旧的扁平格式。
 */

#include "../kc_hal.h"
#include "../third_party/cJSON/cJSON.h"
#include <stddef.h>

kc_err_t session_mgr_init(void);

/* 追加简单文本消息（向后兼容） */
kc_err_t session_append(const char *chat_id, const char *role, const char *content);

/* 追加完整 cJSON 消息（保留 tool_use/tool_result 结构） */
kc_err_t session_append_message(const char *chat_id, cJSON *message);

kc_err_t session_get_history_json(const char *chat_id, char *buf, size_t size, int max_msgs);

/* 直接返回 cJSON 数组（调用者 cJSON_Delete），避免 JSON 序列化截断 */
cJSON *session_get_history(const char *chat_id, int max_msgs);

kc_err_t session_clear(const char *chat_id);
void session_list(void);
