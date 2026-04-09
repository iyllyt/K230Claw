/*
 * tool_registry.c - 工具注册与分发
 *
 * Phase 3 PART 1: 工具框架升级
 *   - kc_tool_result_t 便捷构造函数
 *   - 动态返回值，无固定缓冲区限制
 *   - 工具按字母序排列（稳定 LLM KV cache）
 */

#include "tool_registry.h"
#include "tool_get_time.h"
#include "tool_files.h"
#include "tool_shell.h"
#include "tool_cron.h"
#include "tool_web.h"
#include "../roles/role_mgr.h"
#ifdef KC_HAS_K230_HW
#include "tool_camera.h"
#include "tool_audio.h"
#include "tool_face.h"
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../third_party/cJSON/cJSON.h"

#define TAG "tools"
#define MAX_TOOLS 32

/* ── kc_tool_result_t 构造与释放 ── */

static kc_tool_result_t *result_alloc(void) {
    kc_tool_result_t *r = calloc(1, sizeof(kc_tool_result_t));
    return r;
}

kc_tool_result_t *tool_result_text(const char *text) {
    kc_tool_result_t *r = result_alloc();
    if (!r) return NULL;
    r->content = text ? strdup(text) : strdup("");
    return r;
}

kc_tool_result_t *tool_result_error(const char *msg) {
    kc_tool_result_t *r = result_alloc();
    if (!r) return NULL;
    r->content = msg ? strdup(msg) : strdup("Unknown error");
    r->is_error = true;
    return r;
}

kc_tool_result_t *tool_result_silent(const char *text) {
    kc_tool_result_t *r = result_alloc();
    if (!r) return NULL;
    r->content = text ? strdup(text) : strdup("");
    r->silent = true;
    return r;
}

kc_tool_result_t *tool_result_user(const char *text) {
    kc_tool_result_t *r = result_alloc();
    if (!r) return NULL;
    r->content = text ? strdup(text) : strdup("");
    r->for_user = text ? strdup(text) : strdup("");
    return r;
}

kc_tool_result_t *tool_result_image(const char *text, const char *b64, const char *mime) {
    kc_tool_result_t *r = result_alloc();
    if (!r) return NULL;
    r->content = text ? strdup(text) : strdup("");
    r->image_base64 = b64 ? strdup(b64) : NULL;
    r->image_media_type = mime ? strdup(mime) : strdup("image/jpeg");
    return r;
}

kc_tool_result_t *tool_result_handled(const char *text) {
    kc_tool_result_t *r = result_alloc();
    if (!r) return NULL;
    r->content = text ? strdup(text) : strdup("Response delivered to user.");
    r->response_handled = true;
    return r;
}

void tool_result_free(kc_tool_result_t *r) {
    if (!r) return;
    free(r->content);
    free(r->for_user);
    free(r->image_base64);
    free(r->image_media_type);
    free(r);
}

/* ── 工具注册表 ── */

static kc_tool_t s_tools[MAX_TOOLS];
static int s_tool_count = 0;
static char *s_tools_json = NULL;
static bool s_disabled[MAX_TOOLS];

kc_err_t tool_registry_register(const kc_tool_t *tool) {
    if (s_tool_count >= MAX_TOOLS) return KC_FAIL;
    s_tools[s_tool_count++] = *tool;
    return KC_OK;
}

/* 生成 Anthropic 格式的工具 JSON（跳过 disabled 工具） */
void tool_registry_rebuild(void) {
    free(s_tools_json);
    cJSON *arr = cJSON_CreateArray();

    for (int i = 0; i < s_tool_count; i++) {
        if (s_disabled[i]) continue;
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", s_tools[i].name);
        cJSON_AddStringToObject(item, "description", s_tools[i].description);
        cJSON *schema = cJSON_Parse(s_tools[i].input_schema_json);
        cJSON_AddItemToObject(item, "input_schema", schema ? schema : cJSON_CreateObject());
        cJSON_AddItemToArray(arr, item);
    }

    s_tools_json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
}

kc_tool_result_t *tool_registry_execute(const char *name, const char *input_json) {
    for (int i = 0; i < s_tool_count; i++) {
        if (strcmp(s_tools[i].name, name) == 0) {
            if (s_disabled[i]) {
                char buf[128];
                snprintf(buf, sizeof(buf),
                    "Tool '%s' is disabled in the current role.", name);
                return tool_result_error(buf);
            }
            return s_tools[i].execute(input_json);
        }
    }
    char buf[128];
    snprintf(buf, sizeof(buf), "Unknown tool: %s", name);
    return tool_result_error(buf);
}

const char *tool_registry_get_tools_json(void) {
    return s_tools_json ? s_tools_json : "[]";
}

/* ── 角色系统：工具启用/禁用 ── */

void tool_registry_set_disabled(const char *name, bool disabled) {
    if (!name) return;
    for (int i = 0; i < s_tool_count; i++) {
        if (strcmp(s_tools[i].name, name) == 0) {
            s_disabled[i] = disabled;
            return;
        }
    }
}

void tool_registry_reset_all_enabled(void) {
    memset(s_disabled, 0, sizeof(s_disabled));
}

/* ── switch_role 工具 ── */

static kc_tool_result_t *tool_switch_role_execute(const char *input_json)
{
    cJSON *root = cJSON_Parse(input_json);
    const char *action = NULL;
    const char *name = NULL;
    if (root) {
        cJSON *j = cJSON_GetObjectItem(root, "action");
        if (j) action = j->valuestring;
        j = cJSON_GetObjectItem(root, "name");
        if (j) name = j->valuestring;
    }

    if (!action) {
        cJSON_Delete(root);
        return tool_result_error("Missing 'action' parameter (list or switch).");
    }

    if (strcmp(action, "list") == 0) {
        char *list = role_mgr_list();
        kc_tool_result_t *r = tool_result_text(list ? list : "No roles available.");
        free(list);
        cJSON_Delete(root);
        return r;
    }

    if (strcmp(action, "switch") == 0) {
        if (!name || name[0] == '\0') {
            cJSON_Delete(root);
            return tool_result_error("Missing 'name' parameter for switch.");
        }
        kc_err_t err = role_mgr_switch(name);
        cJSON_Delete(root);
        if (err == KC_OK) {
            char buf[256];
            snprintf(buf, sizeof(buf), "Switched to role: %s", name);
            return tool_result_text(buf);
        } else if (err == KC_ERR_NOT_FOUND) {
            char buf[256];
            snprintf(buf, sizeof(buf), "Role not found: %s", name);
            return tool_result_error(buf);
        }
        return tool_result_error("Failed to switch role.");
    }

    cJSON_Delete(root);
    return tool_result_error("Unknown action. Use 'list' or 'switch'.");
}

/* ── 工具注册（按字母序）── */

kc_err_t tool_registry_init(void) {
    /* get_current_time */
    static const kc_tool_t tool_get_time = {
        .name = "get_current_time",
        .description = "Get the current local date and time.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{},\"required\":[]}",
        .execute = tool_get_time_execute
    };

    /* list_dir */
    static const kc_tool_t tool_list_dir = {
        .name = "list_dir",
        .description = "List files in a directory. Only paths under the data directory are allowed.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Directory path to list (default: data directory)\"}},\"required\":[]}",
        .execute = tool_list_dir_execute
    };

    /* read_file */
    static const kc_tool_t tool_read_file = {
        .name = "read_file",
        .description = "Read a text file. Only paths under the data directory are allowed.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Absolute file path\"}},\"required\":[\"path\"]}",
        .execute = tool_read_file_execute
    };

    /* run_command */
    static const kc_tool_t tool_run_command = {
        .name = "run_command",
        .description = "Execute a Linux shell command. Safe commands (ls, cat, df, etc.) run directly. "
                        "Other commands require user confirmation: call once without 'confirmed' to ask, "
                        "then call again with \"confirmed\": true after the user approves.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\",\"description\":\"The command to execute\"},\"confirmed\":{\"type\":\"boolean\",\"description\":\"Set to true after user approves a non-whitelisted command\"}},\"required\":[\"command\"]}",
        .execute = tool_shell_execute
    };

    /* write_file */
    static const kc_tool_t tool_write_file = {
        .name = "write_file",
        .description = "Write content to a text file. Only paths under the data directory are allowed.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Absolute file path\"},\"content\":{\"type\":\"string\",\"description\":\"Content to write\"}},\"required\":[\"path\",\"content\"]}",
        .execute = tool_write_file_execute
    };

    /* list_tasks */
    static const kc_tool_t tool_list_tasks = {
        .name = "list_tasks",
        .description = "List all scheduled tasks (cron jobs).",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{},\"required\":[]}",
        .execute = tool_list_tasks_execute
    };

    /* remove_task */
    static const kc_tool_t tool_remove_task = {
        .name = "remove_task",
        .description = "Remove a scheduled task by its ID.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"number\",\"description\":\"Task ID to remove\"}},\"required\":[\"id\"]}",
        .execute = tool_remove_task_execute
    };

    /* schedule_task */
    static const kc_tool_t tool_schedule_task = {
        .name = "schedule_task",
        .description = "Schedule a task. Type 'once' fires after delay_seconds. Type 'interval' repeats every interval_seconds. "
                        "The message will be sent to the AI when the task fires.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\",\"description\":\"Task name\"},\"type\":{\"type\":\"string\",\"enum\":[\"once\",\"interval\"],\"description\":\"once=fire once after delay, interval=repeat\"},\"delay_seconds\":{\"type\":\"number\",\"description\":\"For once: seconds from now until fire\"},\"interval_seconds\":{\"type\":\"number\",\"description\":\"For interval: seconds between fires (min 10)\"},\"message\":{\"type\":\"string\",\"description\":\"Message content when task fires\"}},\"required\":[\"type\",\"message\"]}",
        .execute = tool_schedule_task_execute
    };

    /* switch_role */
    static const kc_tool_t tool_switch_role = {
        .name = "switch_role",
        .description = "Switch operating role or list available roles. "
                        "Roles control which sensors are active, which tools are available, and behavior. "
                        "Use action 'list' to see roles, 'switch' to change.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":"
            "{\"action\":{\"type\":\"string\",\"enum\":[\"list\",\"switch\"],\"description\":\"list=show roles, switch=activate a role\"},"
            "\"name\":{\"type\":\"string\",\"description\":\"Role name (required for switch)\"}}"
            ",\"required\":[\"action\"]}",
        .execute = tool_switch_role_execute
    };

    /* web_fetch */
    static const kc_tool_t tool_web_fetch = {
        .name = "web_fetch",
        .description = "Fetch a web page and return its text content (HTML tags stripped).",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\",\"description\":\"URL to fetch\"},\"max_length\":{\"type\":\"number\",\"description\":\"Max output length in chars (default 4096)\"}},\"required\":[\"url\"]}",
        .execute = tool_web_fetch_execute
    };

    /* web_search */
    static const kc_tool_t tool_web_search = {
        .name = "web_search",
        .description = "Search the web using DuckDuckGo. Returns titles, URLs, and snippets.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"Search query\"},\"max_results\":{\"type\":\"number\",\"description\":\"Max results (1-10, default 5)\"}},\"required\":[\"query\"]}",
        .execute = tool_web_search_execute
    };

    /* 硬件工具（条件编译，按字母序插入） */
#ifdef KC_HAS_K230_HW
    static const kc_tool_t tool_camera = {
        .name = "camera_capture",
        .description = "Take a photo using the onboard camera. Returns a JPEG image for visual analysis.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{},\"required\":[]}",
        .execute = tool_camera_capture_execute
    };

    static const kc_tool_t tool_record_audio = {
        .name = "record_audio",
        .description = "Record audio from the microphone, then transcribe to text via cloud STT. "
                        "Use when the user wants to speak instead of type.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"duration\":{\"type\":\"number\",\"description\":\"Recording duration in seconds (1-30, default 5)\"}},\"required\":[]}",
        .execute = tool_record_audio_execute
    };

    static const kc_tool_t tool_register_face = {
        .name = "register_face",
        .description = "Register the owner's face for recognition. After registration, the face detection sensor "
                        "will only recognize the registered person and ignore strangers. "
                        "The user must be in front of the camera when calling this tool.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{},\"required\":[]}",
        .execute = tool_register_face_execute
    };

    static const kc_tool_t tool_speak_text = {
        .name = "speak_text",
        .description = "Speak Chinese text aloud through the 3.5mm audio output using local TTS. "
                        "Use to give verbal responses to the user.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\",\"description\":\"Chinese text to speak aloud\"}},\"required\":[\"text\"]}",
        .execute = tool_speak_text_execute
    };
#endif

    /* 注册（按字母序） */
#ifdef KC_HAS_K230_HW
    tool_registry_register(&tool_camera);
#endif
    tool_registry_register(&tool_get_time);
    tool_registry_register(&tool_list_dir);
    tool_registry_register(&tool_list_tasks);
    tool_registry_register(&tool_read_file);
#ifdef KC_HAS_K230_HW
    tool_registry_register(&tool_record_audio);
    tool_registry_register(&tool_register_face);
#endif
    tool_registry_register(&tool_remove_task);
    tool_registry_register(&tool_run_command);
    tool_registry_register(&tool_schedule_task);
#ifdef KC_HAS_K230_HW
    tool_registry_register(&tool_speak_text);
#endif
    tool_registry_register(&tool_switch_role);
    tool_registry_register(&tool_web_fetch);
    tool_registry_register(&tool_web_search);
    tool_registry_register(&tool_write_file);

    tool_registry_rebuild();
    KC_LOGI(TAG, "tool registry initialized with %d tools", s_tool_count);
    return KC_OK;
}
