/*
 * yolo_detect.cpp - YOLOv8n 多目标检测（C++ 实现）
 *
 * 借鉴 ai_demo/object_detect_yolov8n/ob_det.cc 推理流程，
 * 用 kpu_wrapper C API 替代 AIBase C++ 基类，去掉 OpenCV 依赖。
 *
 * 输入: BG3P 帧数据（3-plane BGR, 1920×1080）
 * 输出: COCO 80 类检测结果（xyxy 坐标映射到原始分辨率）
 *
 * 条件编译：KC_HAS_K230_HW
 */

#include "yolo_detect.h"

#ifdef KC_HAS_K230_HW

#include "kpu_wrapper.h"
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <algorithm>

#define TAG "yolo_det"

/* ── COCO 80 类名称表（与 ai_demo/ob_det.h 一致） ── */

static const char *coco_class_names[YOLO_NUM_CLASSES] = {
    "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck", "boat",
    "traffic light", "fire hydrant", "stop sign", "parking meter", "bench", "bird", "cat",
    "dog", "horse", "sheep", "cow", "elephant", "bear", "zebra", "giraffe", "backpack",
    "umbrella", "handbag", "tie", "suitcase", "frisbee", "skis", "snowboard", "sports ball",
    "kite", "baseball bat", "baseball glove", "skateboard", "surfboard", "tennis racket",
    "bottle", "wine glass", "cup", "fork", "knife", "spoon", "bowl", "banana", "apple",
    "sandwich", "orange", "broccoli", "carrot", "hot dog", "pizza", "donut", "cake",
    "chair", "couch", "potted plant", "bed", "dining table", "toilet", "tv", "laptop",
    "mouse", "remote", "keyboard", "cell phone", "microwave", "oven", "toaster", "sink",
    "refrigerator", "book", "clock", "vase", "scissors", "teddy bear", "hair drier", "toothbrush"
};

/* ── 全局状态 ── */

static struct {
    kpu_model_t *model;
    kpu_ai2d_t  *ai2d;
    float        score_thres;
    float        nms_thres;
    int          rows_det;           /* 总锚点数 */
    int          dimensions_det;     /* 84 = 4 + 80 */
    float       *output_transposed;  /* 转置后的输出缓冲 */
    int          ready;
} s_yolo;

/* ── NMS + IoU（纯 C 实现，参考 ob_det.cc:236-296） ── */

typedef struct {
    int   x, y, w, h;
    float confidence;
    int   index;
    int   class_id;
} bbox_t;

static float calc_iou(int x1, int y1, int w1, int h1,
                       int x2, int y2, int w2, int h2)
{
    int xx1 = x1 > x2 ? x1 : x2;
    int yy1 = y1 > y2 ? y1 : y2;
    int xx2 = (x1 + w1 - 1) < (x2 + w2 - 1) ? (x1 + w1 - 1) : (x2 + w2 - 1);
    int yy2 = (y1 + h1 - 1) < (y2 + h2 - 1) ? (y1 + h1 - 1) : (y2 + h2 - 1);

    int iw = xx2 - xx1 + 1;
    int ih = yy2 - yy1 + 1;
    if (iw <= 0 || ih <= 0) return 0.0f;

    float inter = (float)(iw * ih);
    float area1 = (float)(w1 * h1);
    float area2 = (float)(w2 * h2);
    return inter / (area1 + area2 - inter);
}

/* NMS: 输入 bbox 数组，输出保留的索引 */
static int nms(bbox_t *bboxes, int count, float nms_thresh, int *kept)
{
    /* 按置信度升序排列 */
    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            if (bboxes[i].confidence > bboxes[j].confidence) {
                bbox_t tmp = bboxes[i];
                bboxes[i] = bboxes[j];
                bboxes[j] = tmp;
            }
        }
    }

    int n_kept = 0;
    int size = count;
    for (int i = size - 1; i >= 0; i--) {
        if (bboxes[i].confidence < 0) continue;
        kept[n_kept++] = bboxes[i].index;

        for (int j = i - 1; j >= 0; j--) {
            if (bboxes[j].confidence < 0) continue;
            float iou = calc_iou(bboxes[i].x, bboxes[i].y, bboxes[i].w, bboxes[i].h,
                                  bboxes[j].x, bboxes[j].y, bboxes[j].w, bboxes[j].h);
            if (iou > nms_thresh) {
                bboxes[j].confidence = -1.0f;
            }
        }
    }
    return n_kept;
}

/* ── 公共 API ── */

extern "C" {

kc_err_t yolo_detect_init(const char *kmodel_path,
                           float score_thres, float nms_thres)
{
    if (s_yolo.ready) return KC_OK;

    memset(&s_yolo, 0, sizeof(s_yolo));
    s_yolo.score_thres = score_thres;
    s_yolo.nms_thres = nms_thres;

    /* 加载 kmodel */
    kc_err_t err = kpu_model_load(kmodel_path, &s_yolo.model);
    if (err != KC_OK) {
        KC_LOGE(TAG, "failed to load kmodel: %s", kmodel_path);
        return err;
    }

    /* 检查输入输出 */
    int in_count = kpu_model_input_count(s_yolo.model);
    int out_count = kpu_model_output_count(s_yolo.model);
    if (in_count < 1 || out_count < 1) {
        KC_LOGE(TAG, "unexpected model: %d inputs, %d outputs", in_count, out_count);
        kpu_model_free(s_yolo.model);
        s_yolo.model = NULL;
        return KC_FAIL;
    }

    int in_shape[4], in_ndim;
    kpu_model_input_shape(s_yolo.model, 0, in_shape, &in_ndim);
    KC_LOGI(TAG, "input shape: [%d,%d,%d,%d] ndim=%d",
            in_shape[0], in_shape[1], in_shape[2], in_shape[3], in_ndim);

    /* 计算 YOLOv8 检测网格（参考 ob_det.cc:35-42） */
    int model_w = in_shape[3];  /* 320 */
    int model_h = in_shape[2];  /* 320 */
    int count_0 = (model_w / 8)  * (model_h / 8);
    int count_1 = (model_w / 16) * (model_h / 16);
    int count_2 = (model_w / 32) * (model_h / 32);
    s_yolo.rows_det = count_0 + count_1 + count_2;
    s_yolo.dimensions_det = YOLO_NUM_CLASSES + 4;

    KC_LOGI(TAG, "grid: %d+%d+%d=%d anchors, dims=%d",
            count_0, count_1, count_2, s_yolo.rows_det, s_yolo.dimensions_det);

    s_yolo.output_transposed = new float[s_yolo.rows_det * s_yolo.dimensions_det];

    /* 创建 ai2d 预处理器（BG3P → 模型输入，letterbox resize） */
    err = kpu_ai2d_create_resize(&s_yolo.ai2d,
        3, 1080, 1920,       /* 输入: BG3P 3通道 1920×1080 */
        model_h, model_w,    /* 输出: 320×320 */
        114, 114, 114);      /* padding 灰色 */
    if (err != KC_OK) {
        KC_LOGE(TAG, "failed to create ai2d");
        kpu_model_free(s_yolo.model);
        s_yolo.model = NULL;
        return err;
    }

    s_yolo.ready = 1;
    KC_LOGI(TAG, "YOLOv8n detector initialized (%s, score=%.2f, nms=%.2f)",
            kmodel_path, score_thres, nms_thres);
    return KC_OK;
}

void yolo_detect_deinit(void)
{
    if (!s_yolo.ready) return;

    kpu_ai2d_free(s_yolo.ai2d);
    s_yolo.ai2d = NULL;

    kpu_model_free(s_yolo.model);
    s_yolo.model = NULL;

    delete[] s_yolo.output_transposed;
    s_yolo.output_transposed = NULL;

    s_yolo.ready = 0;
    KC_LOGI(TAG, "YOLOv8n detector deinitialized");
}

kc_err_t yolo_detect_run(const void *frame_data,
                          int src_w, int src_h,
                          yolo_snapshot_t *out)
{
    if (!s_yolo.ready || !frame_data || !out)
        return KC_ERR_INVALID;

    memset(out, 0, sizeof(*out));

    /* 1. ai2d 预处理：BG3P → 320×320 模型输入 */
    size_t data_len = 3 * src_h * src_w;
    kc_err_t err = kpu_ai2d_run(s_yolo.ai2d, s_yolo.model, 0,
                                 frame_data, data_len);
    if (err != KC_OK) {
        KC_LOGE(TAG, "ai2d preprocess failed");
        return err;
    }

    /* 2. 推理 */
    err = kpu_model_run(s_yolo.model);
    if (err != KC_OK) {
        KC_LOGE(TAG, "inference failed");
        return err;
    }

    /* 3. 后处理（参考 ob_det.cc:97-181） */
    float *ori_data = kpu_model_output_data(s_yolo.model, 0);
    if (!ori_data) {
        KC_LOGE(TAG, "cannot get output data");
        return KC_FAIL;
    }

    float x_factor = (float)src_w / (float)YOLO_MODEL_INPUT_W;
    float y_factor = (float)src_h / (float)YOLO_MODEL_INPUT_H;

    int rows = s_yolo.rows_det;
    int dims = s_yolo.dimensions_det;
    float *data = s_yolo.output_transposed;

    /* 转置: [dims, rows] → [rows, dims]（NCW → NWC） */
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < dims; c++) {
            data[r * dims + c] = ori_data[c * rows + r];
        }
    }

    /* 收集候选框 */
    bbox_t  candidates[512];
    int     n_candidates = 0;

    for (int i = 0; i < rows && n_candidates < 512; i++) {
        float *row = data + i * dims;

        /* 类别分数从索引 4 开始，找最大值（替代 OpenCV minMaxLoc） */
        float *scores = row + 4;
        float max_score = scores[0];
        int   max_id = 0;
        for (int c = 1; c < YOLO_NUM_CLASSES; c++) {
            if (scores[c] > max_score) {
                max_score = scores[c];
                max_id = c;
            }
        }

        if (max_score > s_yolo.score_thres) {
            float cx = row[0];
            float cy = row[1];
            float w  = row[2];
            float h  = row[3];

            int left   = (int)((cx - 0.5f * w) * x_factor);
            int top    = (int)((cy - 0.5f * h) * y_factor);
            int width  = (int)(w * x_factor);
            int height = (int)(h * y_factor);

            bbox_t &b = candidates[n_candidates];
            b.x = left;
            b.y = top;
            b.w = width;
            b.h = height;
            b.confidence = max_score;
            b.class_id = max_id;
            b.index = n_candidates;
            n_candidates++;
        }
    }

    if (n_candidates == 0) return KC_OK;

    /* 4. NMS */
    int kept[512];
    int n_kept = nms(candidates, n_candidates, s_yolo.nms_thres, kept);

    /* 5. 填充输出 */
    int max_out = n_kept < YOLO_MAX_DETECTIONS ? n_kept : YOLO_MAX_DETECTIONS;
    out->count = max_out;
    for (int i = 0; i < max_out; i++) {
        /* NMS 输出的是原始 candidates 数组的 index */
        int idx = kept[i];
        /* 但 nms 内部排序打乱了，需回查 */
        /* 实际上 kept 里存的是 bboxes[j].index = 原始 index */
        yolo_detection_t &d = out->detections[i];
        d.class_id = candidates[idx].class_id;
        d.confidence = candidates[idx].confidence;
        d.x = candidates[idx].x;
        d.y = candidates[idx].y;
        d.w = candidates[idx].w;
        d.h = candidates[idx].h;
        snprintf(d.class_name, sizeof(d.class_name), "%s",
                 coco_class_names[d.class_id]);
    }

    return KC_OK;
}

int yolo_detect_is_ready(void)
{
    return s_yolo.ready;
}

} /* extern "C" */

#else /* !KC_HAS_K230_HW */

/* x86 stubs */
extern "C" {

kc_err_t yolo_detect_init(const char *kmodel_path, float score_thres, float nms_thres) {
    (void)kmodel_path; (void)score_thres; (void)nms_thres;
    return KC_ERR_NOT_FOUND;
}
void     yolo_detect_deinit(void) {}
kc_err_t yolo_detect_run(const void *frame_data, int src_w, int src_h, yolo_snapshot_t *out) {
    (void)frame_data; (void)src_w; (void)src_h; (void)out;
    return KC_ERR_NOT_FOUND;
}
int      yolo_detect_is_ready(void) { return 0; }

} /* extern "C" */

#endif /* KC_HAS_K230_HW */
