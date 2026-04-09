/*
 * tool_get_time.c - 获取当前时间
 * K230 Linux 有系统时钟和 NTP，直接读取即可
 */

#include "tool_get_time.h"
#include "../kc_config.h"

#include <time.h>

#define TAG "tool_time"

kc_tool_result_t *tool_get_time_execute(const char *input_json)
{
    (void)input_json;

    char buf[128];
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);

    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z (%A)", &local);
    KC_LOGI(TAG, "time: %s", buf);
    return tool_result_text(buf);
}
