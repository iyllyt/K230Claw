#pragma once

/*
 * tool_shell.h - 终端命令执行工具
 * 白名单 + 用户确认机制 + fork/exec/waitpid
 */

#include "tool_registry.h"

kc_tool_result_t *tool_shell_execute(const char *input_json);
