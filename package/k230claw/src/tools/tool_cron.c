/*
 * tool_cron.c - Cron 定时任务工具
 *
 * 三个工具:
 *   schedule_task: 创建定时任务（once 或 interval）
 *   list_tasks: 列出所有定时任务
 *   remove_task: 删除定时任务
 */

#include "tool_cron.h"
#include "../cron/cron_service.h"
#include "../third_party/cJSON/cJSON.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TAG "tool_cron"

/* 当前通道（由 agent_loop 每轮设置） */
static __thread const char *s_current_channel = NULL;

void tool_cron_set_channel(const char *channel)
{
    s_current_channel = channel;
}

kc_tool_result_t *tool_schedule_task_execute(const char *input_json)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) return tool_result_error("Invalid JSON input");

    const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(root, "name"));
    const char *type_str = cJSON_GetStringValue(cJSON_GetObjectItem(root, "type"));
    const char *message = cJSON_GetStringValue(cJSON_GetObjectItem(root, "message"));

    if (!message || !message[0]) {
        cJSON_Delete(root);
        return tool_result_error("Missing 'message' parameter");
    }

    int type = 0; /* once */
    if (type_str && strcmp(type_str, "interval") == 0) type = 1;

    time_t trigger_or_interval;

    if (type == 0) {
        /* once: 需要 delay_seconds（从现在起多少秒后） */
        cJSON *delay = cJSON_GetObjectItem(root, "delay_seconds");
        if (!delay || !cJSON_IsNumber(delay) || delay->valuedouble <= 0) {
            cJSON_Delete(root);
            return tool_result_error("For 'once' type, provide 'delay_seconds' (positive number)");
        }
        trigger_or_interval = time(NULL) + (time_t)delay->valuedouble;
    } else {
        /* interval: 需要 interval_seconds */
        cJSON *interval = cJSON_GetObjectItem(root, "interval_seconds");
        if (!interval || !cJSON_IsNumber(interval) || interval->valuedouble < 10) {
            cJSON_Delete(root);
            return tool_result_error("For 'interval' type, provide 'interval_seconds' (minimum 10)");
        }
        trigger_or_interval = (time_t)interval->valuedouble;
    }

    int id = cron_add_job(name ? name : "unnamed", type, trigger_or_interval, message, s_current_channel);
    cJSON_Delete(root);

    if (id < 0) return tool_result_error("Failed to add task (limit reached)");

    char buf[128];
    snprintf(buf, sizeof(buf), "Scheduled task #%d created successfully.", id);
    return tool_result_text(buf);
}

kc_tool_result_t *tool_list_tasks_execute(const char *input_json)
{
    (void)input_json;
    char *json = cron_list_jobs();
    if (!json) return tool_result_text("No scheduled tasks.");

    kc_tool_result_t *r = tool_result_text(json);
    free(json);
    return r;
}

kc_tool_result_t *tool_remove_task_execute(const char *input_json)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) return tool_result_error("Invalid JSON input");

    cJSON *id_item = cJSON_GetObjectItem(root, "id");
    if (!id_item || !cJSON_IsNumber(id_item)) {
        cJSON_Delete(root);
        return tool_result_error("Missing 'id' parameter (number)");
    }

    int id = (int)id_item->valuedouble;
    cJSON_Delete(root);

    if (cron_remove_job(id) != KC_OK)
        return tool_result_error("Task not found");

    char buf[64];
    snprintf(buf, sizeof(buf), "Task #%d removed.", id);
    return tool_result_text(buf);
}
