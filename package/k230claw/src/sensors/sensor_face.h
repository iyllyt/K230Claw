#pragma once

/*
 * sensor_face.h - 人脸识别传感器
 *
 * 后台持续 V4L2 + KPU 人脸检测 + 识别。
 * 使用 /dev/video2 (BG3P 格式)，不与拍照工具冲突。
 *
 * 功能：
 *   1. 人脸检测（face_detection_320.kmodel）
 *   2. 人脸识别（face_recognition.kmodel）— 只识别注册的主人
 *   3. 注册主人人脸（register_face 工具调用）
 *
 * 状态变化事件：
 *   person_detected — 主人出现
 *   person_left     — 主人离开（3 秒消抖）
 *   陌生人完全忽略，不推任何事件。
 */

#include "sensor_mgr.h"

/* 获取人脸识别传感器实例（注册到 sensor_mgr） */
kc_sensor_t *face_detector_get(void);

#ifdef KC_HAS_K230_HW

/* 请求注册主人人脸（阻塞等待，timeout_sec 秒超时） */
kc_err_t face_sensor_register(int timeout_sec);

/* 查询是否已有主人 embedding */
int face_sensor_has_owner(void);

#else

static inline kc_err_t face_sensor_register(int t) { (void)t; return KC_FAIL; }
static inline int face_sensor_has_owner(void) { return 0; }

#endif
