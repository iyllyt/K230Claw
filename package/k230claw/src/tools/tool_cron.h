#pragma once

/*
 * tool_cron.h - Cron 定时任务工具
 * LLM 可通过此工具创建/列出/删除定时任务
 */

#include "tool_registry.h"

/* 设置当前通道（agent_loop 每轮调用，cron 创建时记住回复通道） */
void tool_cron_set_channel(const char *channel);

kc_tool_result_t *tool_schedule_task_execute(const char *input_json);
kc_tool_result_t *tool_list_tasks_execute(const char *input_json);
kc_tool_result_t *tool_remove_task_execute(const char *input_json);
