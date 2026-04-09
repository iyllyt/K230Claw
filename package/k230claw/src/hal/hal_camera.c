/*
 * hal_camera.c - V4L2 摄像头 HAL
 *
 * 使用 v4l2-drm 库从 /dev/video1 拍照：
 *   1. V4L2 dump 单帧（NV12, 1920x1080）
 *   2. NV12 → RGB888 转换（CPU）
 *   3. 降采样到 640x480（减小 base64 体积）
 *   4. libjpeg 压缩 quality=75，输出到内存缓冲区
 *
 * 条件编译：KC_HAS_K230_HW — 真机；否则 x86 stub。
 */

#include "hal_camera.h"
#include "camera_utils.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TAG "hal_camera"

#ifdef KC_HAS_K230_HW

#include <v4l2-drm.h>
#include <linux/videodev2.h>

#define CAM_DEVICE      1        /* /dev/video1 */
#define CAM_WIDTH       1920
#define CAM_HEIGHT      1080
#define CAM_BUFFER_NUM  3
#define OUT_WIDTH       640
#define OUT_HEIGHT      480
#define JPEG_QUALITY    75
#define DUMP_TIMEOUT_MS 2000

kc_err_t hal_camera_capture_jpeg(uint8_t **jpeg_out, size_t *jpeg_len)
{
    if (!jpeg_out || !jpeg_len) return KC_ERR_INVALID;

    struct v4l2_drm_context ctx;
    v4l2_drm_default_context(&ctx);
    ctx.device = CAM_DEVICE;
    ctx.display = false;
    ctx.width = CAM_WIDTH;
    ctx.height = CAM_HEIGHT;
    ctx.video_format = V4L2_PIX_FMT_NV12;
    ctx.buffer_num = CAM_BUFFER_NUM;

    if (v4l2_drm_setup(&ctx, 1, NULL) != 0) {
        KC_LOGE(TAG, "V4L2 setup failed");
        return KC_FAIL;
    }

    if (v4l2_drm_start(&ctx) != 0) {
        KC_LOGE(TAG, "V4L2 start failed");
        v4l2_drm_stop(&ctx);
        return KC_FAIL;
    }

    /* 丢弃前几帧让自动曝光稳定 */
    for (int i = 0; i < 3; i++) {
        if (v4l2_drm_dump(&ctx, DUMP_TIMEOUT_MS) == 0)
            v4l2_drm_dump_release(&ctx);
    }

    /* 抓取一帧 */
    if (v4l2_drm_dump(&ctx, DUMP_TIMEOUT_MS) != 0) {
        KC_LOGE(TAG, "V4L2 dump failed");
        v4l2_drm_stop(&ctx);
        return KC_FAIL;
    }

    /* 获取帧数据 */
    unsigned buf_idx = ctx.vbuffer.index;
    const uint8_t *nv12 = (const uint8_t *)ctx.buffers[buf_idx].mmap;

    /* NV12 → RGB888 + 降采样 */
    size_t rgb_size = OUT_WIDTH * OUT_HEIGHT * 3;
    uint8_t *rgb = malloc(rgb_size);
    if (!rgb) {
        v4l2_drm_dump_release(&ctx);
        v4l2_drm_stop(&ctx);
        return KC_ERR_NO_MEM;
    }

    nv12_to_rgb_downscale(nv12, CAM_WIDTH, CAM_HEIGHT,
                          rgb, OUT_WIDTH, OUT_HEIGHT);

    v4l2_drm_dump_release(&ctx);
    v4l2_drm_stop(&ctx);

    /* RGB → JPEG */
    int ret = rgb_to_jpeg_mem(rgb, OUT_WIDTH, OUT_HEIGHT,
                              JPEG_QUALITY, jpeg_out, jpeg_len);
    free(rgb);

    if (ret != 0) {
        KC_LOGE(TAG, "JPEG compression failed");
        return KC_FAIL;
    }

    KC_LOGI(TAG, "captured JPEG: %zu bytes (%dx%d)",
            *jpeg_len, OUT_WIDTH, OUT_HEIGHT);
    return KC_OK;
}

#else /* !KC_HAS_K230_HW */

kc_err_t hal_camera_capture_jpeg(uint8_t **jpeg_out, size_t *jpeg_len)
{
    (void)jpeg_out; (void)jpeg_len;
    KC_LOGW(TAG, "Camera HAL not available (x86 stub)");
    return KC_ERR_NOT_FOUND;
}

#endif /* KC_HAS_K230_HW */
