#pragma once

/*
 * hal_camera.h - V4L2 摄像头 HAL 封装
 *
 * 使用 v4l2-drm 库拍照，NV12 → libjpeg 压缩。
 * /dev/video1 — NV12 格式，用于拍照（不与人脸检测冲突）。
 */

#include "../kc_hal.h"
#include <stdint.h>
#include <stddef.h>

/*
 * 拍一张 JPEG 照片。
 * 内部流程：V4L2 dump 单帧 → NV12→RGB → 降采样 → libjpeg 压缩。
 * jpeg_out/jpeg_len 由内部 malloc，调用者 free。
 */
kc_err_t hal_camera_capture_jpeg(uint8_t **jpeg_out, size_t *jpeg_len);
