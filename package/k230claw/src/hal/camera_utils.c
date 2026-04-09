/*
 * camera_utils.c - 共享相机工具函数
 *
 * NV12→RGB 转换 + libjpeg 压缩，供 hal_camera 和 hal_camera_stream 共用。
 */

#include "camera_utils.h"
#include <stdlib.h>

#ifdef KC_HAS_K230_HW

#include <stdio.h>
#include <jpeglib.h>

/* NV12 → RGB888 单像素（YUV BT.601） */
static inline void yuv_to_rgb(int y, int u, int v,
                               uint8_t *r, uint8_t *g, uint8_t *b)
{
    int c = y - 16;
    int d = u - 128;
    int e = v - 128;
    int rr = (298 * c + 409 * e + 128) >> 8;
    int gg = (298 * c - 100 * d - 208 * e + 128) >> 8;
    int bb = (298 * c + 516 * d + 128) >> 8;
    *r = (uint8_t)(rr < 0 ? 0 : (rr > 255 ? 255 : rr));
    *g = (uint8_t)(gg < 0 ? 0 : (gg > 255 ? 255 : gg));
    *b = (uint8_t)(bb < 0 ? 0 : (bb > 255 ? 255 : bb));
}

void nv12_to_rgb_downscale(const uint8_t *nv12,
                            int src_w, int src_h,
                            uint8_t *rgb,
                            int dst_w, int dst_h)
{
    const uint8_t *y_plane = nv12;
    const uint8_t *uv_plane = nv12 + src_w * src_h;

    for (int dy = 0; dy < dst_h; dy++) {
        int sy = dy * src_h / dst_h;
        for (int dx = 0; dx < dst_w; dx++) {
            int sx = dx * src_w / dst_w;

            int y_val = y_plane[sy * src_w + sx];
            int uv_offset = (sy / 2) * src_w + (sx & ~1);
            int u_val = uv_plane[uv_offset];
            int v_val = uv_plane[uv_offset + 1];

            int idx = (dy * dst_w + dx) * 3;
            yuv_to_rgb(y_val, u_val, v_val,
                       &rgb[idx], &rgb[idx + 1], &rgb[idx + 2]);
        }
    }
}

int rgb_to_jpeg_mem(const uint8_t *rgb, int w, int h, int quality,
                     uint8_t **jpeg_out, size_t *jpeg_len)
{
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);

    unsigned char *outbuf = NULL;
    unsigned long outsize = 0;
    jpeg_mem_dest(&cinfo, &outbuf, &outsize);

    cinfo.image_width = w;
    cinfo.image_height = h;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);

    jpeg_start_compress(&cinfo, TRUE);
    while (cinfo.next_scanline < cinfo.image_height) {
        const uint8_t *row = rgb + cinfo.next_scanline * w * 3;
        JSAMPROW row_ptr = (JSAMPROW)row;
        jpeg_write_scanlines(&cinfo, &row_ptr, 1);
    }
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);

    *jpeg_out = outbuf;
    *jpeg_len = (size_t)outsize;
    return 0;
}

#else /* !KC_HAS_K230_HW */

/* x86 stub — 仅 hal_camera_stream.c/hal_camera.c 调用（均有 KC_HAS_K230_HW 守卫），此 stub 仅供参考 */
#include <string.h>
void nv12_to_rgb_downscale(const uint8_t *nv12, int src_w, int src_h,
                            uint8_t *rgb, int dst_w, int dst_h) {
    (void)nv12; (void)src_w; (void)src_h; (void)dst_w; (void)dst_h;
    memset(rgb, 0, (size_t)dst_w * dst_h * 3);
}
int rgb_to_jpeg_mem(const uint8_t *rgb, int w, int h, int quality,
                     uint8_t **jpeg_out, size_t *jpeg_len) {
    (void)rgb; (void)w; (void)h; (void)quality;
    *jpeg_out = NULL; *jpeg_len = 0;
    return -1;
}

#endif /* KC_HAS_K230_HW */
