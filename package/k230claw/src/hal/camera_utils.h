/*
 * camera_utils.h - 共享相机工具函数
 *
 * NV12→RGB 转换 + libjpeg 压缩，供 hal_camera 和 hal_camera_stream 共用。
 */

#pragma once
#include <stdint.h>
#include <stddef.h>

/* NV12 → RGB888 + 降采样（nearest neighbor） */
void nv12_to_rgb_downscale(const uint8_t *nv12,
    int src_w, int src_h, uint8_t *rgb, int dst_w, int dst_h);

/* libjpeg 压缩 RGB888 到内存缓冲区，返回 0 成功 */
int rgb_to_jpeg_mem(const uint8_t *rgb, int w, int h, int quality,
    uint8_t **jpeg_out, size_t *jpeg_len);
