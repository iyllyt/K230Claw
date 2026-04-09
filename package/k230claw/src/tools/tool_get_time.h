#pragma once

/*
 * tool_get_time.h - 获取当前时间
 * K230 Linux 有系统时钟，直接用 time() + strftime()
 */

#include "tool_registry.h"

kc_tool_result_t *tool_get_time_execute(const char *input_json);
