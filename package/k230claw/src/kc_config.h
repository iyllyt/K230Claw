#pragma once

/*
 * kc_config.h - K230Claw 编译时常量
 * 对应 MimiClaw 的 mimi_config.h，去掉 ESP32/WiFi/NVS 相关项
 */

#include "kc_hal.h"

/* ── LLM ── */
#define KC_LLM_DEFAULT_MODEL        "gpt-4o"
#define KC_OPENAI_API_URL           "https://api.openai.com/v1/chat/completions"
#define KC_ANTHROPIC_API_URL        "https://api.anthropic.com/v1/messages"
#define KC_LLM_MAX_TOKENS           4096
#define KC_LLM_STREAM_BUF_SIZE     (64 * 1024)   /* 64KB, Linux 内存充裕 */
#define KC_LLM_LOG_PREVIEW_BYTES    200

/* ── Agent Loop ── */
#define KC_AGENT_MAX_TOOL_ITER      10
#define KC_MAX_TOOL_CALLS           4
#define KC_AGENT_MAX_HISTORY        20
#define KC_AGENT_SEND_WORKING_STATUS 1

/* ── 消息总线 ── */
#define KC_BUS_QUEUE_LEN            16

/* ── 上下文/记忆 ── */
#define KC_CONTEXT_BUF_SIZE        (32 * 1024)    /* 32KB */
#define KC_SESSION_MAX_MSGS         20
#define KC_CONTEXT_TOKEN_BUDGET     100000  /* 上下文 token 预算，可通过 config.json 的 context_budget 覆盖 */
#define KC_SESSION_COMPACT_INTERVAL 50      /* 每 N 次 append 触发一次 compact */

/* ── 时区 ── */
#define KC_TIMEZONE                 "Asia/Shanghai"

/* ── 文件路径 ── */
#define KC_DATA_DIR_DEFAULT         "/var/lib/k230claw"
#define KC_CONFIG_FILE              "/etc/k230claw/k230claw.conf"

/* 运行时路径（由 kc_paths_init 初始化） */
const char *kc_get_data_dir(void);
const char *kc_get_memory_dir(void);
const char *kc_get_session_dir(void);
const char *kc_get_config_dir(void);
const char *kc_get_skills_dir(void);
const char *kc_get_memory_file(void);
const char *kc_get_soul_file(void);
const char *kc_get_user_file(void);

/* 在 kc_config_init 之后调用，读取 data_dir 配置 */
void kc_paths_init(void);

/* ── 消息通道名 ── */
#define KC_CHAN_CLI                  "cli"
#define KC_CHAN_TELEGRAM             "telegram"
#define KC_CHAN_WEBSOCKET            "websocket"
#define KC_CHAN_SYSTEM               "system"

/* ── 运行时配置 API (kc_config.c) ── */
kc_err_t kc_config_init(const char *config_path);
const char *kc_config_get_str(const char *key, const char *default_val);
kc_err_t kc_config_set_str(const char *key, const char *value);
kc_err_t kc_config_save(void);

/* cJSON 对象级别操作（用于 model_list 等复杂结构） */
struct cJSON;  /* 前置声明 */
struct cJSON *kc_config_get_json(const char *key);       /* 返回副本，调用者负责 cJSON_Delete */
void kc_config_set_json(const char *key, struct cJSON *val); /* val 会被 detach 后挂入配置树 */

