#pragma once

/*
 * context_builder.h - 系统提示词构建
 * 拼接基础提示词 + SOUL.md + USER.md + 长期记忆 + 每日笔记 + 技能摘要
 */

#include "../kc_hal.h"
#include <stddef.h>

kc_err_t context_build_system_prompt(char *buf, size_t size);
