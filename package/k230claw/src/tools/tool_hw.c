/*
 * tool_hw.c - 硬件工具入口
 * Phase 1-2: 空 stub，未注册到 tool_registry（节省 token）
 * Phase 4+: 将通过 libgpiod 直接控制 GPIO
 */

#include "tool_hw.h"

#include <stdio.h>
#include <string.h>

#define TAG "tool_hw"

#define KC_GPIO_PIN_MIN  0
#define KC_GPIO_PIN_MAX  63

/*
 * GPIO 安全白名单（庐山派 K230 LCKFB）
 * 禁止: IO0-1(boot), IO9-13(CSI), IO21(CSI2), IO22(LCD), IO25(LCD_EN),
 *        IO29-31(CSI PWDN), IO38-39(UART0), IO40-41(I2C1),
 *        IO50-51(UART3), IO54-59(MMC1 SDIO)
 */
int kc_gpio_pin_allowed(int pin) {
    if (pin < KC_GPIO_PIN_MIN || pin > KC_GPIO_PIN_MAX) return 0;
    static const int blocked[] = {
        0, 1, 9, 10, 11, 12, 13, 21, 22, 25,
        29, 30, 31, 38, 39, 40, 41, 50, 51,
        54, 55, 56, 57, 58, 59, -1
    };
    for (int i = 0; blocked[i] >= 0; i++)
        if (pin == blocked[i]) return 0;
    return 1;
}

kc_tool_result_t *tool_gpio_set_execute(const char *input_json)
{
    (void)input_json;
    return tool_result_error(
        "GPIO control is not implemented yet. "
        "Will use libgpiod in a future phase.");
}

kc_tool_result_t *tool_gpio_get_execute(const char *input_json)
{
    (void)input_json;
    return tool_result_error(
        "GPIO read is not implemented yet. "
        "Will use libgpiod in a future phase.");
}
