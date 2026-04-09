#pragma once

/*
 * tool_web.h - Web Search + Web Fetch 工具
 *
 * web_search: DuckDuckGo HTML 搜索（免费，无需 API key）
 * web_fetch:  libcurl GET + HTML 标签剥离
 */

#include "tool_registry.h"

kc_tool_result_t *tool_web_search_execute(const char *input_json);
kc_tool_result_t *tool_web_fetch_execute(const char *input_json);
