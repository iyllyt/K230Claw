#pragma once

/*
 * sensor_mgr.h - 后台传感器管理器
 *
 * 参考 PicoClaw MaixCam 通道模式：后台 pthread 持续运行，
 * 状态变化时推事件到 inbound 队列。
 *
 * 本次只搭框架，不实现具体传感器。
 * 未来传感器实现（如人脸识别）只需：
 *   1. 实现 kc_sensor_t 接口
 *   2. 调用 sensor_mgr_register() 注册
 *   3. 在 start 回调中启动 pthread，检测变化时调 sensor_push_event()
 */

#include "../kc_hal.h"

/* 传感器接口 */
typedef struct kc_sensor {
    const char *name;   /* "face_detector", "motion", "temperature" 等 */

    /* 生命周期 */
    kc_err_t (*start)(struct kc_sensor *self);
    kc_err_t (*stop)(struct kc_sensor *self);

    /* 传感器私有数据 */
    void *ctx;
} kc_sensor_t;

/* 初始化传感器管理器 */
kc_err_t sensor_mgr_init(void);

/* 注册传感器（不 start，仅注册） */
kc_err_t sensor_mgr_register(kc_sensor_t *sensor);

/* 启动所有已注册传感器 */
kc_err_t sensor_mgr_start_all(void);

/* 停止所有已注册传感器 */
void sensor_mgr_stop_all(void);

/* 按名称启动/停止单个传感器 */
kc_err_t sensor_mgr_start_by_name(const char *name);
kc_err_t sensor_mgr_stop_by_name(const char *name);

/* 查询传感器是否正在运行（1=运行中，0=已停止/-1=未找到） */
int sensor_mgr_is_running(const char *name);

/*
 * 传感器线程调用此函数推送事件到 inbound 队列（线程安全）。
 * 内置节流：相同 event_name 的事件在 min_interval_sec 内不重复推送。
 *
 * event_name: 事件名称，如 "person_detected", "person_left"
 * payload: 事件描述文本（推入 msg.content）
 */
kc_err_t sensor_push_event(const char *event_name, const char *payload);
