/*
 * tool_camera.c - camera_capture 工具
 *
 * LLM 调用此工具拍照，返回 JPEG base64 图片。
 * agent_loop.c 中已有 image_base64 → Anthropic image content block 管道。
 */

#include "tool_camera.h"
#include "../hal/hal_camera.h"
#include "../kc_config.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#define TAG "tool_camera"

#ifdef KC_HAS_K230_HW
/* mongoose 已有 base64 编码函数 */
#include "../third_party/mongoose/mongoose.h"
#endif

kc_tool_result_t *tool_camera_capture_execute(const char *input_json)
{
    (void)input_json;

#ifdef KC_HAS_K230_HW
    uint8_t *jpeg = NULL;
    size_t jpeg_len = 0;

    kc_err_t err = hal_camera_capture_jpeg(&jpeg, &jpeg_len);
    if (err != KC_OK) {
        return tool_result_error("Camera capture failed. Check camera connection.");
    }

    /* 保存 JPEG 到文件（可选，用于调试） */
    {
        time_t now = time(NULL);
        char path[256];
        snprintf(path, sizeof(path), "%s/img_%ld.jpg",
                 kc_get_data_dir(), (long)now);
        FILE *f = fopen(path, "wb");
        if (f) {
            fwrite(jpeg, 1, jpeg_len, f);
            fclose(f);
            KC_LOGI(TAG, "saved to %s", path);
        }
    }

    /* Base64 编码 */
    size_t b64_len = (jpeg_len + 2) / 3 * 4 + 1;
    char *b64 = malloc(b64_len);
    if (!b64) {
        free(jpeg);
        return tool_result_error("Out of memory for base64 encoding");
    }

    mg_base64_encode(jpeg, jpeg_len, b64, b64_len);
    free(jpeg);

    kc_tool_result_t *r = tool_result_image(
        "Photo captured successfully.", b64, "image/jpeg");
    free(b64);
    return r;
#else
    return tool_result_error("Camera not available on this platform.");
#endif
}
