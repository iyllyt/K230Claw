#pragma once

/*
 * tool_registry.h - 工具注册与分发
 *
 * Phase 3 PART 1: 工具框架升级
 *   - kc_tool_result_t 结构化返回值（参考 PicoClaw ToolResult）
 *   - 动态分配替代固定缓冲区
 *   - ForLLM/ForUser/IsError/Silent/Image/ResponseHandled 语义
 */

#include "../kc_hal.h"
#include <stddef.h>
#include <stdbool.h>

/* ── 工具执行结果 ── */

typedef struct {
    char *content;           /* ForLLM: 发给 LLM 的文本（malloc，调用者 free） */
    char *for_user;          /* ForUser: 直接发给用户的文本（NULL = LLM 自行转述） */
    char *image_base64;      /* Base64 图片数据（NULL = 无图片，预留摄像头/截图） */
    char *image_media_type;  /* MIME 类型，如 "image/jpeg"（image_base64 非空时有效） */
    bool  is_error;          /* true → LLM API 中标记为 is_error */
    bool  silent;            /* true → 不发消息给用户（静默工具如文件读写） */
    bool  response_handled;  /* true → 工具已直接回复用户，agent loop 可跳过后续 LLM 回复 */
} kc_tool_result_t;

/* 便捷构造函数（内部 strdup，调用者用 tool_result_free 释放） */
kc_tool_result_t *tool_result_text(const char *text);
kc_tool_result_t *tool_result_error(const char *msg);
kc_tool_result_t *tool_result_silent(const char *text);
kc_tool_result_t *tool_result_user(const char *text);
kc_tool_result_t *tool_result_image(const char *text, const char *b64, const char *mime);
kc_tool_result_t *tool_result_handled(const char *text);
void              tool_result_free(kc_tool_result_t *r);

/* ── 工具注册 ── */

typedef kc_tool_result_t *(*kc_tool_execute_fn)(const char *input_json);

typedef struct {
    const char *name;
    const char *description;
    const char *input_schema_json;
    kc_tool_execute_fn execute;
} kc_tool_t;

kc_err_t tool_registry_init(void);
kc_err_t tool_registry_register(const kc_tool_t *tool);

/* 获取 Anthropic 格式的工具 JSON 数组字符串（编译时生成，static 生命周期） */
const char *tool_registry_get_tools_json(void);

/* 执行工具，返回结构化结果（调用者 tool_result_free） */
kc_tool_result_t *tool_registry_execute(const char *name, const char *input_json);

/* ── 角色系统：工具启用/禁用 ── */

/* 按名称禁用/启用单个工具 */
void tool_registry_set_disabled(const char *name, bool disabled);

/* 重置所有工具为启用状态 */
void tool_registry_reset_all_enabled(void);

/* 重建 tools JSON（角色切换后调用） */
void tool_registry_rebuild(void);
