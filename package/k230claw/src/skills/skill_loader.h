#pragma once

/*
 * skill_loader.h - 技能文件加载器
 *
 * Phase 3 PART 8: 支持 frontmatter triggers 关键词触发。
 * 用户消息匹配触发词时，完整技能内容注入上下文。
 * 未匹配的技能只显示摘要。
 *
 * 技能文件格式 (skills/xxx.md):
 *   ---
 *   triggers: 天气, weather, 气温
 *   ---
 *   # 天气查询技能
 *   ...完整内容...
 */

#include "../kc_hal.h"
#include <stddef.h>

kc_err_t skill_loader_init(void);

/* 构建技能摘要（无触发词匹配时，仅标题+描述） */
size_t skill_loader_build_summary(char *buf, size_t size);

/*
 * 检查用户消息是否匹配任何技能的 triggers，
 * 如果匹配，返回完整技能内容（调用者 free）。
 * 未匹配返回 NULL。
 */
char *skill_loader_match_triggers(const char *user_message);
