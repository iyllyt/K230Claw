/*
 * tool_face.c - register_face 工具
 *
 * LLM 调用此工具注册主人的人脸。
 * 通过 face_sensor_register() 请求传感器线程在下一帧
 * 执行检测+识别+保存 embedding。
 */

#include "tool_face.h"
#include "../sensors/sensor_face.h"

#include <stdlib.h>
#include <string.h>

#define TAG "tool_face"

#ifdef KC_HAS_K230_HW

kc_tool_result_t *tool_register_face_execute(const char *input_json)
{
    (void)input_json;

    kc_err_t err = face_sensor_register(10); /* 10 秒超时 */

    if (err == KC_OK) {
        return tool_result_text(
            "Face registered successfully! "
            "I can now recognize you and will only respond to your face.");
    } else if (err == KC_ERR_TIMEOUT) {
        return tool_result_error(
            "Registration timed out. No face detected within 10 seconds. "
            "Make sure you are in front of the camera and try again.");
    } else {
        return tool_result_error(
            "Face registration failed. "
            "The recognition model may not be loaded. "
            "Check that face_recognition.kmodel exists on the device.");
    }
}

#else /* !KC_HAS_K230_HW */

kc_tool_result_t *tool_register_face_execute(const char *input_json)
{
    (void)input_json;
    return tool_result_error("Face registration not available on this platform.");
}

#endif
