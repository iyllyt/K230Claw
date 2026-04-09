/*
 * yolo_detect.h - YOLOv8n 多目标检测
 *
 * C API 封装：加载 yolov8n kmodel，对 BG3P 帧做推理，
 * 输出 COCO 80 类物体检测结果。
 *
 * 参考 ai_demo/object_detect_yolov8n/ob_det.cc
 * 条件编译：KC_HAS_K230_HW
 */

#pragma once
#include "../kc_hal.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define YOLO_MAX_DETECTIONS  50
#define YOLO_NUM_CLASSES     80
#define YOLO_MODEL_INPUT_W   320
#define YOLO_MODEL_INPUT_H   320

/* 单个检测结果 */
typedef struct {
    int   class_id;
    char  class_name[32];
    float confidence;
    int   x, y, w, h;           /* 原始图像坐标系 (src_w × src_h) */
} yolo_detection_t;

/* 检测快照（一帧所有检测结果） */
typedef struct {
    int             count;
    yolo_detection_t detections[YOLO_MAX_DETECTIONS];
} yolo_snapshot_t;

/*
 * 初始化 YOLOv8 检测器。
 * kmodel_path: yolov8n_320.kmodel 路径
 * score_thres: 置信度阈值（建议 0.5）
 * nms_thres:   NMS IoU 阈值（建议 0.6）
 */
kc_err_t yolo_detect_init(const char *kmodel_path,
                           float score_thres, float nms_thres);

/* 释放模型 */
void     yolo_detect_deinit(void);

/*
 * 对一帧 BG3P 数据运行 YOLOv8 检测。
 * nv12_data: BG3P 格式的原始帧数据
 * src_w/src_h: 原始分辨率（1920×1080）
 * out: 输出检测结果（调用者分配）
 */
kc_err_t yolo_detect_run(const void *frame_data,
                          int src_w, int src_h,
                          yolo_snapshot_t *out);

/* 检测器是否已初始化 */
int      yolo_detect_is_ready(void);

#ifdef __cplusplus
}
#endif
