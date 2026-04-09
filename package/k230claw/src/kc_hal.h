#pragma once

/*
 * kc_hal.h - K230Claw 平台抽象层
 * 提供日志宏、错误码定义，替代 ESP-IDF 的 esp_log.h / esp_err.h
 */

#include <stdio.h>
#include <stdlib.h>

/* ── 错误码 ── */
typedef int kc_err_t;

#define KC_OK            0
#define KC_FAIL         -1
#define KC_ERR_TIMEOUT  -2
#define KC_ERR_NO_MEM   -3
#define KC_ERR_INVALID  -4
#define KC_ERR_NOT_FOUND -5
#define KC_ERR_NO_KEY    -6   /* API key 未配置 */
#define KC_ERR_AUTH      -7   /* API key 无效 (401/403) */
#define KC_ERR_NET       -8   /* 网络连接失败 */
#define KC_ERR_API       -9   /* API 返回非 200 错误 */

/* ── 日志宏 ── */
/* 用法: KC_LOGI("main", "started on port %d", port); */
#define KC_LOGE(tag, fmt, ...) \
    fprintf(stderr, "[E][%s] " fmt "\n", tag, ##__VA_ARGS__)

#define KC_LOGW(tag, fmt, ...) \
    fprintf(stderr, "[W][%s] " fmt "\n", tag, ##__VA_ARGS__)

#define KC_LOGI(tag, fmt, ...) \
    fprintf(stderr, "[I][%s] " fmt "\n", tag, ##__VA_ARGS__)

#define KC_LOGD(tag, fmt, ...) \
    do { /* debug 默认关闭，编译时可打开 */ } while(0)

/* 编译时加 -DKC_LOG_DEBUG 启用 debug 日志 */
#ifdef KC_LOG_DEBUG
#undef KC_LOGD
#define KC_LOGD(tag, fmt, ...) \
    fprintf(stderr, "[D][%s] " fmt "\n", tag, ##__VA_ARGS__)
#endif

/* ── 错误检查宏 ── */
#define KC_ERROR_CHECK(x) do {                              \
    kc_err_t __err = (x);                                   \
    if (__err != KC_OK) {                                    \
        fprintf(stderr, "[FATAL] %s:%d err=%d\n",           \
                __FILE__, __LINE__, __err);                  \
        abort();                                             \
    }                                                        \
} while(0)

/* 非致命版本，仅打印警告 */
#define KC_WARN_CHECK(x) do {                               \
    kc_err_t __err = (x);                                   \
    if (__err != KC_OK) {                                    \
        fprintf(stderr, "[W] %s:%d err=%d\n",               \
                __FILE__, __LINE__, __err);                  \
    }                                                        \
} while(0)

/* ── 网络状态 ── */
int kc_is_online(void);
void kc_check_network(void);

/* ── 关停标志 ── */
int kc_is_shutting_down(void);
