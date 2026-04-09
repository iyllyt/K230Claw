/*
 * sensor_face.c - 人脸识别传感器
 *
 * 后台线程：V4L2 dump → ai2d 预处理 → KPU 检测 → NMS + landmarks
 *           → Umeyama 仿射 → KPU 识别 → 特征比对
 *
 * /dev/video2 (BG3P, 1920x1080)，~10fps
 *
 * 检测模型：face_detection_320.kmodel
 *   输入: (1,3,320,320)，输出: 9 个（loc×3 + conf×3 + landmarks×3）
 *
 * 识别模型：face_recognition.kmodel
 *   输入: (1,3,112,112) 对齐人脸，输出: 特征向量
 *
 * 状态变化事件：
 *   person_detected — 主人出现
 *   person_left     — 主人离开（3 秒消抖）
 *   陌生人完全忽略。
 *
 * 条件编译：KC_HAS_K230_HW
 */

#include "sensor_face.h"
#include "sensor_state.h"
/* hal_camera_stream.h 不再需要 — navigator 角色用 YOLOv8 独立检测 */
#include "../kc_config.h"

#ifdef KC_HAS_K230_HW

#include "../hal/kpu_wrapper.h"
#include <v4l2-drm.h>
#include <linux/videodev2.h>

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <unistd.h>

#define TAG "face_recog"

/* ── 配置 ── */
#define FACE_CAM_DEVICE     2
#define FACE_CAM_WIDTH      1920
#define FACE_CAM_HEIGHT     1080
#define FACE_CAM_BUFFERS    3
#define FACE_NET_SIZE       320
#define FACE_NUM_ANCHORS    4200
#define FACE_OBJ_THRESH     0.5f
#define FACE_NMS_THRESH     0.4f
#define FACE_ABSENT_FRAMES  30          /* ~3 秒 @10fps 消抖 */
#define FACE_FPS_DELAY_US   100000      /* 100ms = ~10fps */
#define FACE_RECOG_SIZE     112         /* 识别模型输入尺寸 */
#define FACE_RECOG_INTERVAL 50          /* 每 50 帧重新验证 */
#define FACE_RECOG_THRESH   75.0f       /* 识别阈值 (0-100) */
#define LAND_SIZE           10          /* 5 点 × 2 坐标 */
#define MAX_FACES           10          /* 最多返回的人脸数 */

/* anchors_320.c 中定义 */
extern float kAnchors320[4200][4];

/* ── 数据结构 ── */

typedef struct {
    float points[10]; /* lx,ly, rx,ry, nx,ny, lmx,lmy, rmx,rmy */
} face_landmarks_t;

typedef struct {
    int index;
    float confidence;
} nms_obj_t;

typedef struct {
    int anchor_idx;
    float confidence;
    float box[4];     /* cx, cy, w, h (解码后) */
    face_landmarks_t landmarks;
} face_result_t;

/* ── Umeyama 仿射变换（112x112 标准人脸模板） ── */

static const float s_umeyama_dst[10] = {
    38.2946f, 51.6963f,   /* 左眼 */
    73.5318f, 51.5014f,   /* 右眼 */
    56.0252f, 71.7366f,   /* 鼻子 */
    41.5493f, 92.3655f,   /* 左嘴角 */
    70.7299f, 92.2041f    /* 右嘴角 */
};

static void svd22(const float a[4], float u[4], float s[2], float v[4])
{
    s[0] = (sqrtf(powf(a[0]-a[3],2) + powf(a[1]+a[2],2)) +
            sqrtf(powf(a[0]+a[3],2) + powf(a[1]-a[2],2))) / 2;
    s[1] = fabsf(s[0] - sqrtf(powf(a[0]-a[3],2) + powf(a[1]+a[2],2)));
    v[2] = (s[0] > s[1]) ? sinf(atan2f(2*(a[0]*a[1]+a[2]*a[3]),
            a[0]*a[0]-a[1]*a[1]+a[2]*a[2]-a[3]*a[3]) / 2) : 0;
    v[0] = sqrtf(1 - v[2]*v[2]);
    v[1] = -v[2];
    v[3] = v[0];
    u[0] = (s[0] != 0) ? -(a[0]*v[0]+a[1]*v[2])/s[0] : 1;
    u[2] = (s[0] != 0) ? -(a[2]*v[0]+a[3]*v[2])/s[0] : 0;
    u[1] = (s[1] != 0) ?  (a[0]*v[1]+a[1]*v[3])/s[1] : -u[2];
    u[3] = (s[1] != 0) ?  (a[2]*v[1]+a[3]*v[3])/s[1] :  u[0];
    v[0] = -v[0];
    v[2] = -v[2];
}

/*
 * Umeyama 算法：从 5 个源点到 112x112 标准模板，
 * 计算 2x3 仿射矩阵 dst[0..5] = [a00,a01,a02, a10,a11,a12]
 */
static void image_umeyama_112(const float *src, float *dst)
{
    int i, j, k;
    float src_mean[2] = {0, 0};
    float dst_mean[2] = {0, 0};

    for (i = 0; i < 5; i++) {
        src_mean[0] += src[2*i];
        src_mean[1] += src[2*i+1];
        dst_mean[0] += s_umeyama_dst[2*i];
        dst_mean[1] += s_umeyama_dst[2*i+1];
    }
    src_mean[0] /= 5; src_mean[1] /= 5;
    dst_mean[0] /= 5; dst_mean[1] /= 5;

    float src_dm[5][2], dst_dm[5][2];
    for (i = 0; i < 5; i++) {
        src_dm[i][0] = src[2*i]   - src_mean[0];
        src_dm[i][1] = src[2*i+1] - src_mean[1];
        dst_dm[i][0] = s_umeyama_dst[2*i]   - dst_mean[0];
        dst_dm[i][1] = s_umeyama_dst[2*i+1] - dst_mean[1];
    }

    float A[2][2] = {{0,0},{0,0}};
    for (i = 0; i < 2; i++)
        for (k = 0; k < 2; k++)
            for (j = 0; j < 5; j++)
                A[i][k] += dst_dm[j][i] * src_dm[j][k];
    A[0][0] /= 5; A[0][1] /= 5;
    A[1][0] /= 5; A[1][1] /= 5;

    float U[4], S[2], V[4];
    svd22(&A[0][0], U, S, V);

    /* T = U * V^T (2x2 rotation) */
    float T[3][3] = {{1,0,0},{0,1,0},{0,0,1}};
    T[0][0] = U[0]*V[0] + U[1]*V[1];
    T[0][1] = U[0]*V[0+1] + U[1]*V[1+1]; /* U[0]*V[1]+U[1]*V[3] */
    T[1][0] = U[2]*V[0] + U[3]*V[1];     /* U[2]*V[0]+U[3]*V[2] */
    T[1][1] = U[2]*V[0+1] + U[3]*V[1+1]; /* U[2]*V[1]+U[3]*V[3] */

    /* 重新计算——严格匹配 SDK 代码 */
    T[0][0] = U[0]*V[0] + U[1]*V[2]; /* 注意 V layout: v[0]v[1]/v[2]v[3] = 行优先 2x2 */
    T[0][1] = U[0]*V[1] + U[1]*V[3];
    T[1][0] = U[2]*V[0] + U[3]*V[2];
    T[1][1] = U[2]*V[1] + U[3]*V[3];

    /* scale */
    float src_dm_mean[2] = {0, 0};
    float src_dm_var[2] = {0, 0};
    for (i = 0; i < 5; i++) {
        src_dm_mean[0] += src_dm[i][0];
        src_dm_mean[1] += src_dm[i][1];
    }
    src_dm_mean[0] /= 5; src_dm_mean[1] /= 5;

    for (i = 0; i < 5; i++) {
        src_dm_var[0] += (src_dm_mean[0]-src_dm[i][0])*(src_dm_mean[0]-src_dm[i][0]);
        src_dm_var[1] += (src_dm_mean[1]-src_dm[i][1])*(src_dm_mean[1]-src_dm[i][1]);
    }
    src_dm_var[0] /= 5; src_dm_var[1] /= 5;

    float scale = 1.0f / (src_dm_var[0]+src_dm_var[1]) * (S[0]+S[1]);
    T[0][2] = dst_mean[0] - scale*(T[0][0]*src_mean[0] + T[0][1]*src_mean[1]);
    T[1][2] = dst_mean[1] - scale*(T[1][0]*src_mean[0] + T[1][1]*src_mean[1]);
    T[0][0] *= scale; T[0][1] *= scale;
    T[1][0] *= scale; T[1][1] *= scale;

    /* 输出 2x3 仿射矩阵 */
    dst[0] = T[0][0]; dst[1] = T[0][1]; dst[2] = T[0][2];
    dst[3] = T[1][0]; dst[4] = T[1][1]; dst[5] = T[1][2];
}

/* ── 特征比对 ── */

static void l2_normalize(const float *src, float *dst, int len)
{
    float sum = 0;
    for (int i = 0; i < len; i++) sum += src[i] * src[i];
    sum = sqrtf(sum);
    if (sum < 1e-10f) sum = 1e-10f;
    for (int i = 0; i < len; i++) dst[i] = src[i] / sum;
}

static float cosine_similarity(const float *a, const float *b, int len)
{
    float dot = 0;
    for (int i = 0; i < len; i++) dot += a[i] * b[i];
    return (0.5f + 0.5f * dot) * 100.0f;
}

/* ── 后处理 ── */

static int nms_compare(const void *a, const void *b)
{
    float diff = ((nms_obj_t *)a)->confidence -
                 ((nms_obj_t *)b)->confidence;
    return diff < 0 ? 1 : (diff > 0 ? -1 : 0);
}

static void local_softmax(float *x, float *out, int len)
{
    float max_val = x[0];
    for (int i = 1; i < len; i++)
        if (x[i] > max_val) max_val = x[i];

    float sum = 0;
    for (int i = 0; i < len; i++) {
        out[i] = expf(x[i] - max_val);
        sum += out[i];
    }
    for (int i = 0; i < len; i++)
        out[i] /= sum;
}

static void deal_conf(float *conf, nms_obj_t *objs, int size, int *obj_cnt)
{
    float c[2];
    for (int ww = 0; ww < size; ww++) {
        for (int hh = 0; hh < 2; hh++) {
            for (int cc = 0; cc < 2; cc++)
                c[cc] = conf[(hh * 2 + cc) * size + ww];
            float out[2];
            local_softmax(c, out, 2);
            objs[*obj_cnt].index = *obj_cnt;
            objs[*obj_cnt].confidence = out[1];
            (*obj_cnt)++;
        }
    }
}

static void deal_loc(float *loc, float *boxes, int size, int *obj_cnt)
{
    for (int ww = 0; ww < size; ww++) {
        for (int hh = 0; hh < 2; hh++) {
            for (int cc = 0; cc < 4; cc++)
                boxes[(*obj_cnt) * 4 + cc] =
                    loc[(hh * 4 + cc) * size + ww];
            (*obj_cnt)++;
        }
    }
}

/* 新增：提取 landmarks（CHW → per-anchor） */
static void deal_landms(float *landms, float *landmarks, int size, int *obj_cnt)
{
    for (int ww = 0; ww < size; ww++) {
        for (int hh = 0; hh < 2; hh++) {
            for (int cc = 0; cc < LAND_SIZE; cc++)
                landmarks[(*obj_cnt) * LAND_SIZE + cc] =
                    landms[(hh * LAND_SIZE + cc) * size + ww];
            (*obj_cnt)++;
        }
    }
}

static float overlap(float x1, float w1, float x2, float w2)
{
    float l = (x1 - w1/2 > x2 - w2/2) ? x1 - w1/2 : x2 - w2/2;
    float r = (x1 + w1/2 < x2 + w2/2) ? x1 + w1/2 : x2 + w2/2;
    return r - l;
}

static float box_iou_center(float *a, float *b)
{
    float w = overlap(a[0], a[2], b[0], b[2]);
    float h = overlap(a[1], a[3], b[1], b[3]);
    if (w < 0 || h < 0) return 0;
    float inter = w * h;
    float uni = a[2]*a[3] + b[2]*b[3] - inter;
    return inter / uni;
}

static void decode_box(float *raw, int idx, float *decoded)
{
    float *a = kAnchors320[idx];
    decoded[0] = a[0] + raw[idx*4+0] * 0.1f * a[2];
    decoded[1] = a[1] + raw[idx*4+1] * 0.1f * a[3];
    decoded[2] = a[2] * expf(raw[idx*4+2] * 0.2f);
    decoded[3] = a[3] * expf(raw[idx*4+3] * 0.2f);
}

/* 解码 landmarks：anchor + raw * 0.1 * anchor_size */
static void decode_landmarks(float *raw_landmarks, int idx,
                              face_landmarks_t *out)
{
    float *a = kAnchors320[idx];
    for (int ll = 0; ll < 5; ll++) {
        out->points[2*ll+0] = a[0] + raw_landmarks[idx*LAND_SIZE+2*ll+0] * 0.1f * a[2];
        out->points[2*ll+1] = a[1] + raw_landmarks[idx*LAND_SIZE+2*ll+1] * 0.1f * a[3];
    }
}

/*
 * 完整后处理：返回检测到的人脸列表（含 landmarks）。
 * 返回值：人脸数量。
 */
static int face_post_process(kpu_model_t *model,
                              float *raw_landmarks,
                              face_result_t *results, int max_results)
{
    static nms_obj_t objs[FACE_NUM_ANCHORS];
    static float boxes[FACE_NUM_ANCHORS * 4];

    int min_size = 200;
    int sizes[3] = { 16*min_size/2, 4*min_size/2, 1*min_size/2 };

    /* 提取置信度 (output 3,4,5) */
    int obj_cnt = 0;
    for (int s = 0; s < 3; s++) {
        float *conf = kpu_model_output_data(model, 3 + s);
        if (conf) deal_conf(conf, objs, sizes[s], &obj_cnt);
    }

    /* 提取位置 (output 0,1,2) */
    obj_cnt = 0;
    for (int s = 0; s < 3; s++) {
        float *loc = kpu_model_output_data(model, s);
        if (loc) deal_loc(loc, boxes, sizes[s], &obj_cnt);
    }

    /* 提取 landmarks (output 6,7,8) */
    obj_cnt = 0;
    for (int s = 0; s < 3; s++) {
        float *landms = kpu_model_output_data(model, 6 + s);
        if (landms) deal_landms(landms, raw_landmarks, sizes[s], &obj_cnt);
    }

    /* 排序 */
    qsort(objs, FACE_NUM_ANCHORS, sizeof(nms_obj_t), nms_compare);

    /* NMS */
    int face_count = 0;
    float max_src_size = FACE_CAM_WIDTH > FACE_CAM_HEIGHT ?
                         (float)FACE_CAM_WIDTH : (float)FACE_CAM_HEIGHT;

    for (int i = 0; i < FACE_NUM_ANCHORS && face_count < max_results; i++) {
        if (objs[i].confidence < FACE_OBJ_THRESH) continue;

        float box_a[4];
        decode_box(boxes, objs[i].index, box_a);

        /* 填充结果 */
        face_result_t *r = &results[face_count];
        r->anchor_idx = objs[i].index;
        r->confidence = objs[i].confidence;
        for (int d = 0; d < 4; d++) r->box[d] = box_a[d];

        /* 解码 landmarks 并缩放到像素坐标 */
        decode_landmarks(raw_landmarks, objs[i].index, &r->landmarks);
        for (int ll = 0; ll < 5; ll++) {
            r->landmarks.points[2*ll+0] *= max_src_size;
            r->landmarks.points[2*ll+1] *= max_src_size;
        }

        face_count++;

        /* NMS：抑制重叠框 */
        for (int j = i + 1; j < FACE_NUM_ANCHORS; j++) {
            if (objs[j].confidence < FACE_OBJ_THRESH) continue;
            float box_b[4];
            decode_box(boxes, objs[j].index, box_b);
            if (box_iou_center(box_a, box_b) >= FACE_NMS_THRESH)
                objs[j].confidence = 0;
        }
    }

    return face_count;
}

/* 找最大人脸 */
static int find_largest_face(face_result_t *results, int count)
{
    int best = 0;
    float best_area = 0;
    for (int i = 0; i < count; i++) {
        float area = results[i].box[2] * results[i].box[3];
        if (area > best_area) {
            best_area = area;
            best = i;
        }
    }
    return best;
}

/* ── Embedding 持久化 ── */

static char s_embed_path[256] = {0};

static void get_embed_path(void)
{
    if (s_embed_path[0]) return;
    snprintf(s_embed_path, sizeof(s_embed_path),
             "%s/face_embedding.bin", kc_get_data_dir());
}

static int save_embedding(const float *embed, int dim)
{
    get_embed_path();
    FILE *f = fopen(s_embed_path, "wb");
    if (!f) return -1;
    fwrite(&dim, sizeof(int), 1, f);
    fwrite(embed, sizeof(float), dim, f);
    fclose(f);
    KC_LOGI(TAG, "embedding saved: %s (%d dims)", s_embed_path, dim);
    return 0;
}

static float *load_embedding(int *dim_out)
{
    get_embed_path();
    FILE *f = fopen(s_embed_path, "rb");
    if (!f) return NULL;

    int dim = 0;
    if (fread(&dim, sizeof(int), 1, f) != 1 || dim <= 0 || dim > 4096) {
        fclose(f);
        return NULL;
    }

    float *embed = (float *)malloc(dim * sizeof(float));
    if (!embed) { fclose(f); return NULL; }

    if ((int)fread(embed, sizeof(float), dim, f) != dim) {
        free(embed);
        fclose(f);
        return NULL;
    }
    fclose(f);

    *dim_out = dim;
    KC_LOGI(TAG, "embedding loaded: %s (%d dims)", s_embed_path, dim);
    return embed;
}

/* ── 传感器上下文 ── */

typedef struct {
    pthread_t thread;
    volatile int stopping;
    kpu_model_t *det_model;
    kpu_model_t *recog_model;
    kpu_ai2d_t *ai2d;           /* 检测用 resize */
    int feature_dim;
    float *owner_embedding;     /* 主人特征（L2 归一化后） */
    int has_owner;
    volatile int register_req;
    volatile int register_done;
    volatile int register_ok;
    float raw_landmarks[FACE_NUM_ANCHORS * LAND_SIZE];
} face_ctx_t;

static face_ctx_t s_face_ctx;

/*
 * 对指定人脸运行识别模型，返回特征向量。
 * 调用者 free 返回的 feature。
 */
static float *run_recognition(face_ctx_t *ctx,
                               const void *frame_data, size_t frame_size,
                               face_result_t *face, int *feat_dim)
{
    /* 计算仿射矩阵 */
    float affine[6];
    image_umeyama_112(face->landmarks.points, affine);

    /* ai2d 仿射变换 + 设为识别模型输入 */
    kc_err_t err = kpu_ai2d_run_affine(ctx->recog_model, 0,
        frame_data, frame_size,
        3, FACE_CAM_HEIGHT, FACE_CAM_WIDTH,
        FACE_RECOG_SIZE, FACE_RECOG_SIZE,
        affine);
    if (err != KC_OK) {
        KC_LOGW(TAG, "affine run failed");
        return NULL;
    }

    /* KPU 推理 */
    if (kpu_model_run(ctx->recog_model) != KC_OK) {
        KC_LOGW(TAG, "recognition run failed");
        return NULL;
    }

    /* 获取特征向量 */
    float *raw = kpu_model_output_data(ctx->recog_model, 0);
    int dim = kpu_model_output_size(ctx->recog_model, 0);
    if (!raw || dim <= 0) return NULL;

    /* L2 归一化后复制 */
    float *feature = (float *)malloc(dim * sizeof(float));
    if (!feature) return NULL;
    l2_normalize(raw, feature, dim);

    *feat_dim = dim;
    return feature;
}

static void *face_thread(void *arg)
{
    face_ctx_t *ctx = (face_ctx_t *)arg;
    int user_present = 0;
    int absent_count = 0;
    int is_owner = 0;
    int verify_counter = 0;

    /* 获取阈值 */
    const char *thresh_str = kc_config_get_str("face_recog_threshold", "");
    float recog_threshold = thresh_str[0] ? (float)atof(thresh_str) : FACE_RECOG_THRESH;

    /* 1. 打开 V4L2 */
    struct v4l2_drm_context v4l2;
    v4l2_drm_default_context(&v4l2);
    v4l2.device = FACE_CAM_DEVICE;
    v4l2.display = false;
    v4l2.width = FACE_CAM_WIDTH;
    v4l2.height = FACE_CAM_HEIGHT;
    v4l2.video_format = v4l2_fourcc('B', 'G', '3', 'P');
    v4l2.buffer_num = FACE_CAM_BUFFERS;

    if (v4l2_drm_setup(&v4l2, 1, NULL) != 0) {
        KC_LOGE(TAG, "V4L2 setup failed");
        return NULL;
    }
    if (v4l2_drm_start(&v4l2) != 0) {
        KC_LOGE(TAG, "V4L2 start failed");
        v4l2_drm_stop(&v4l2);
        return NULL;
    }

    KC_LOGI(TAG, "face recognition started on /dev/video%d (owner=%s)",
            FACE_CAM_DEVICE, ctx->has_owner ? "yes" : "no");

    /* 2. 检测+识别循环 */
    while (!ctx->stopping && !kc_is_shutting_down()) {
        int ret = v4l2_drm_dump(&v4l2, 1000);
        if (ret != 0) {
            usleep(FACE_FPS_DELAY_US);
            continue;
        }

        unsigned buf_idx = v4l2.vbuffer.index;
        void *buf_data = v4l2.buffers[buf_idx].mmap;
        size_t buf_size = 3 * FACE_CAM_WIDTH * FACE_CAM_HEIGHT;

        /* ai2d 预处理 + 检测推理 */
        kc_err_t err = kpu_ai2d_run(ctx->ai2d, ctx->det_model, 0,
                                     buf_data, buf_size);
        if (err != KC_OK) {
            v4l2_drm_dump_release(&v4l2);
            usleep(FACE_FPS_DELAY_US);
            continue;
        }

        if (kpu_model_run(ctx->det_model) != KC_OK) {
            v4l2_drm_dump_release(&v4l2);
            usleep(FACE_FPS_DELAY_US);
            continue;
        }

        /* 后处理（含 landmarks） */
        face_result_t results[MAX_FACES];
        int face_count = face_post_process(ctx->det_model,
            ctx->raw_landmarks, results, MAX_FACES);

        /* === 注册请求处理 === */
        if (ctx->register_req && face_count > 0) {
            int largest = find_largest_face(results, face_count);
            int feat_dim = 0;
            float *feature = run_recognition(ctx, buf_data, buf_size,
                                              &results[largest], &feat_dim);
            if (feature) {
                /* 保存为主人 embedding */
                free(ctx->owner_embedding);
                ctx->owner_embedding = feature;
                ctx->feature_dim = feat_dim;
                ctx->has_owner = 1;
                save_embedding(feature, feat_dim);
                ctx->register_ok = 1;
                is_owner = 1;
                KC_LOGI(TAG, "owner registered! dim=%d", feat_dim);
            } else {
                ctx->register_ok = 0;
                KC_LOGW(TAG, "registration failed (recognition error)");
            }
            ctx->register_req = 0;
            ctx->register_done = 1;
        }

        /* === 识别逻辑 === */
        if (face_count > 0 && ctx->has_owner && ctx->recog_model) {
            int need_verify = 0;

            /* 需要验证的时机 */
            if (!user_present) {
                need_verify = 1; /* 首次出现 */
            } else {
                verify_counter++;
                if (verify_counter >= FACE_RECOG_INTERVAL) {
                    need_verify = 1; /* 定期重新验证 */
                    verify_counter = 0;
                }
            }

            if (need_verify) {
                int largest = find_largest_face(results, face_count);
                int feat_dim = 0;
                float *feature = run_recognition(ctx, buf_data, buf_size,
                                                  &results[largest], &feat_dim);
                if (feature && feat_dim == ctx->feature_dim) {
                    float score = cosine_similarity(
                        feature, ctx->owner_embedding, feat_dim);
                    is_owner = (score > recog_threshold);
                    KC_LOGI(TAG, "verify: score=%.1f %s",
                            score, is_owner ? "OWNER" : "stranger");
                    free(feature);
                } else {
                    is_owner = 0;
                    free(feature);
                }
            }
        } else if (face_count == 0) {
            is_owner = 0;
            verify_counter = 0;
        }

        v4l2_drm_dump_release(&v4l2);

        /* === 状态机（只对主人推事件） === */
        if (face_count > 0 && is_owner && !user_present) {
            user_present = 1;
            absent_count = 0;
            sensor_state_set("user_present", "true");
            char count_str[8];
            snprintf(count_str, sizeof(count_str), "%d", face_count);
            sensor_state_set("face_count", count_str);
            sensor_push_event("person_detected",
                "The owner appeared in front of the camera.");
            KC_LOGI(TAG, "owner detected");
        } else if ((!is_owner || face_count == 0) && user_present) {
            absent_count++;
            if (absent_count > FACE_ABSENT_FRAMES) {
                user_present = 0;
                is_owner = 0;
                sensor_state_set("user_present", "false");
                sensor_state_set("face_count", "0");
                sensor_push_event("person_left",
                    "The owner left the camera view.");
                KC_LOGI(TAG, "owner left");
            }
        } else if (face_count > 0 && is_owner) {
            absent_count = 0;
            char count_str[8];
            snprintf(count_str, sizeof(count_str), "%d", face_count);
            sensor_state_set("face_count", count_str);
        }

        usleep(FACE_FPS_DELAY_US);
    }

    v4l2_drm_stop(&v4l2);
    KC_LOGI(TAG, "face recognition stopped");
    return NULL;
}

/* ── 传感器接口 ── */

static kc_err_t face_start(kc_sensor_t *self)
{
    (void)self;
    face_ctx_t *ctx = &s_face_ctx;

    /* 加载检测模型 */
    const char *det_kmodel = kc_config_get_str("face_kmodel",
        "/root/app/face_detection/face_detection_320.kmodel");

    if (kpu_model_load(det_kmodel, &ctx->det_model) != KC_OK) {
        KC_LOGE(TAG, "failed to load detection model: %s", det_kmodel);
        return KC_FAIL;
    }

    /* 加载识别模型 */
    const char *recog_kmodel = kc_config_get_str("face_recog_kmodel",
        "/root/app/face_verification/face_recognition.kmodel");

    if (kpu_model_load(recog_kmodel, &ctx->recog_model) != KC_OK) {
        KC_LOGW(TAG, "recognition model not found: %s (detection only)",
                recog_kmodel);
        ctx->recog_model = NULL;
    } else {
        /* 获取特征维度 */
        int shape[4] = {0};
        int ndim = 0;
        kpu_model_output_shape(ctx->recog_model, 0, shape, &ndim);
        ctx->feature_dim = (ndim >= 2) ? shape[1] : shape[0];
        KC_LOGI(TAG, "recognition model: feature_dim=%d", ctx->feature_dim);
    }

    /* 创建 ai2d (1920x1080 → 320x320, padding 104/117/123) */
    if (kpu_ai2d_create_resize(&ctx->ai2d,
            3, FACE_CAM_HEIGHT, FACE_CAM_WIDTH,
            FACE_NET_SIZE, FACE_NET_SIZE,
            104, 117, 123) != KC_OK) {
        KC_LOGE(TAG, "ai2d create failed");
        kpu_model_free(ctx->det_model);
        if (ctx->recog_model) kpu_model_free(ctx->recog_model);
        return KC_FAIL;
    }

    /* 加载已有的主人 embedding */
    ctx->owner_embedding = load_embedding(&ctx->feature_dim);
    ctx->has_owner = (ctx->owner_embedding != NULL);
    if (ctx->has_owner) {
        /* L2 归一化存储的 embedding */
        float *norm = (float *)malloc(ctx->feature_dim * sizeof(float));
        if (norm) {
            l2_normalize(ctx->owner_embedding, norm, ctx->feature_dim);
            free(ctx->owner_embedding);
            ctx->owner_embedding = norm;
        }
    }

    if (!ctx->has_owner) {
        KC_LOGI(TAG, "no owner registered, use register_face to register");
    }

    /* 初始化状态 */
    sensor_state_set("user_present", "false");
    sensor_state_set("face_count", "0");

    /* 初始化注册标志 */
    ctx->register_req = 0;
    ctx->register_done = 0;
    ctx->register_ok = 0;

    /* 启动线程 */
    ctx->stopping = 0;
    if (pthread_create(&ctx->thread, NULL, face_thread, ctx) != 0) {
        KC_LOGE(TAG, "thread create failed");
        kpu_ai2d_free(ctx->ai2d);
        kpu_model_free(ctx->det_model);
        if (ctx->recog_model) kpu_model_free(ctx->recog_model);
        free(ctx->owner_embedding);
        return KC_FAIL;
    }

    KC_LOGI(TAG, "face recognition sensor started");
    return KC_OK;
}

static kc_err_t face_stop(kc_sensor_t *self)
{
    (void)self;
    face_ctx_t *ctx = &s_face_ctx;
    ctx->stopping = 1;
    pthread_join(ctx->thread, NULL);

    if (ctx->ai2d) { kpu_ai2d_free(ctx->ai2d); ctx->ai2d = NULL; }
    if (ctx->det_model) { kpu_model_free(ctx->det_model); ctx->det_model = NULL; }
    if (ctx->recog_model) { kpu_model_free(ctx->recog_model); ctx->recog_model = NULL; }
    free(ctx->owner_embedding); ctx->owner_embedding = NULL;
    ctx->has_owner = 0;

    KC_LOGI(TAG, "face recognition sensor stopped");
    return KC_OK;
}

static kc_sensor_t s_face_sensor = {
    .name = "face_detector",
    .start = face_start,
    .stop = face_stop,
    .ctx = &s_face_ctx
};

kc_sensor_t *face_detector_get(void)
{
    return &s_face_sensor;
}

/* ── 外部 API（供 register_face 工具调用） ── */

kc_err_t face_sensor_register(int timeout_sec)
{
    face_ctx_t *ctx = &s_face_ctx;

    if (!ctx->recog_model) {
        KC_LOGE(TAG, "recognition model not loaded");
        return KC_FAIL;
    }

    /* 发起注册请求 */
    ctx->register_done = 0;
    ctx->register_ok = 0;
    ctx->register_req = 1;

    /* 轮询等待完成 */
    int waited = 0;
    while (!ctx->register_done && waited < timeout_sec * 10) {
        usleep(100000); /* 100ms */
        waited++;
    }

    if (!ctx->register_done) {
        ctx->register_req = 0;
        KC_LOGW(TAG, "registration timed out (no face detected in %ds)", timeout_sec);
        return KC_ERR_TIMEOUT;
    }

    return ctx->register_ok ? KC_OK : KC_FAIL;
}

int face_sensor_has_owner(void)
{
    return s_face_ctx.has_owner;
}

#else /* !KC_HAS_K230_HW */

kc_sensor_t *face_detector_get(void) { return NULL; }

#endif /* KC_HAS_K230_HW */
