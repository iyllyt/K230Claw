#pragma once

/*
 * tool_hw.h - 硬件工具入口
 * Phase 1-2: 未注册（节省 token）
 * Phase 4+: GPIO(libgpiod), I2C(ioctl), SPI(ioctl)
 */

#include "tool_registry.h"

kc_tool_result_t *tool_gpio_set_execute(const char *input_json);
kc_tool_result_t *tool_gpio_get_execute(const char *input_json);

/* GPIO 引脚安全检查：返回 1 = 允许, 0 = 禁止 */
int kc_gpio_pin_allowed(int pin);
