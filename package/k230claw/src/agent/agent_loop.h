#pragma once

/*
 * agent_loop.h - ReAct 循环
 * 消费 inbound 消息 → 构建提示词 → 调 LLM → 执行工具 → 推送 outbound
 */

#include "../kc_hal.h"

kc_err_t agent_loop_init(void);
kc_err_t agent_loop_start(void);
void agent_loop_stop(void);
