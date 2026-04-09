#pragma once

/*
 * sensor_state.h - 传感器持续状态存储
 *
 * 线程安全的 key-value 存储，agent loop 和传感器线程都可访问。
 * 用途：传感器设置当前状态（如 user_present=true），
 *       agent loop 构建 context 时读取状态。
 *
 * 例：
 *   sensor_state_set("user_present", "true");
 *   sensor_state_set("user_last_seen", "2026-04-08 15:30:00");
 *   const char *val = sensor_state_get("user_present", "unknown");
 */

#include "../kc_hal.h"

/* 初始化状态存储 */
kc_err_t sensor_state_init(void);

/* 设置状态（线程安全，内部 strdup） */
kc_err_t sensor_state_set(const char *key, const char *value);

/* 读取状态（线程安全，返回值为内部缓冲区，下次调用可能覆盖） */
const char *sensor_state_get(const char *key, const char *default_val);

/* 获取所有状态的摘要文本（调用者 free，用于注入 context） */
char *sensor_state_summary(void);
