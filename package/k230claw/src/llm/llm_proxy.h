#pragma once

/*
 * llm_proxy.h - LLM API 客户端
 * 支持 OpenAI 和 Anthropic 双 provider，用 libcurl 替代 esp_http_client
 * API 与 MimiClaw 保持一致
 */

#include "../kc_hal.h"
#include "../kc_config.h"
#include "../third_party/cJSON/cJSON.h"
#include <stddef.h>
#include <stdbool.h>

/* 单个工具调用 */
typedef struct {
    char id[64];        /* "call_xxx" 或 "toolu_xxx" */
    char name[32];      /* 工具名 */
    char *input;        /* 堆分配的 JSON 字符串 */
    size_t input_len;
} llm_tool_call_t;

/* LLM 响应（两种 provider 共用） */
typedef struct {
    char *text;                              /* 累积文本 */
    size_t text_len;
    llm_tool_call_t calls[KC_MAX_TOOL_CALLS];
    int call_count;
    bool tool_use;                           /* 是否需要执行工具 */
} llm_response_t;

/* 初始化，从配置文件加载 API key / model / provider / proxy */
kc_err_t llm_proxy_init(void);

/* 发送带工具的聊天请求（非流式） */
kc_err_t llm_chat_tools(const char *system_prompt,
                        cJSON *messages,
                        const char *tools_json,
                        llm_response_t *resp);

/* 释放响应中的堆内存 */
void llm_response_free(llm_response_t *resp);

/* 运行时修改配置并保存 */
kc_err_t llm_set_api_key(const char *api_key);
kc_err_t llm_set_model(const char *model);
kc_err_t llm_set_provider(const char *provider);
void     llm_set_api_url(const char *url);
void     llm_set_proxy(const char *proxy);
