#pragma once

/*
 * tool_files.h - 文件操作工具
 * 路径安全验证 + data_dir 沙箱
 */

#include "tool_registry.h"

kc_tool_result_t *tool_read_file_execute(const char *input_json);
kc_tool_result_t *tool_write_file_execute(const char *input_json);
kc_tool_result_t *tool_list_dir_execute(const char *input_json);
