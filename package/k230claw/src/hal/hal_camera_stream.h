/*
 * hal_camera_stream.h - 流媒体相机 HAL + YOLOv8 检测叠加
 *
 * 持久化 V4L2 上下文（/dev/video1, NV12），持续捕获帧。
 * 集成 YOLOv8n 多目标检测（/dev/video2, BG3P），在 JPEG 帧上画检测框。
 * 条件编译：KC_HAS_K230_HW。
 */

#pragma once
#include "../kc_hal.h"
#include "yolo_detect.h"
#include <stdint.h>
#include <stddef.h>

/* 帧回调类型 */
typedef void (*stream_frame_cb_t)(const uint8_t *jpeg_data, size_t len, void *user_data);

/* 启动/停止流媒体捕获线程（内部同时启动 YOLOv8 推理） */
kc_err_t hal_camera_stream_start(void);
void     hal_camera_stream_stop(void);

/* 是否正在运行 */
int hal_camera_stream_is_running(void);

/* 注册/注销帧回调 */
void hal_camera_stream_on_frame(stream_frame_cb_t cb, void *user_data);
void hal_camera_stream_remove_frame_cb(stream_frame_cb_t cb);

/* 获取当前帧率 */
float hal_camera_stream_get_fps(void);

/* 获取最近一帧的检测快照（供 /stream_status 使用） */
void hal_camera_stream_get_detections(yolo_snapshot_t *out);
