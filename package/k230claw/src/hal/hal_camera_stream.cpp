/*
 * hal_camera_stream.cpp - 流媒体相机 HAL + YOLOv8 检测
 *
 * 两个线程：
 *   1. 推理线程: /dev/video2 (BG3P) → YOLOv8n KPU 推理 → yolo_snapshot
 *   2. 流媒体线程: /dev/video1 (NV12) → RGB → 画检测框 → JPEG → 帧回调
 *
 * 条件编译：KC_HAS_K230_HW
 */

extern "C" {
#include "hal_camera_stream.h"
#include "camera_utils.h"
#include "../kc_config.h"
}

#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <unistd.h>
#include <pthread.h>

#define TAG "hal_stream"

#ifdef KC_HAS_K230_HW

extern "C" {
#include <v4l2-drm.h>
#include <linux/videodev2.h>
}

#define STREAM_CAM_DEVICE     1            /* /dev/video1 NV12 显示流 */
#define AI_CAM_DEVICE         2            /* /dev/video2 BG3P AI 推理 */
#define STREAM_CAM_WIDTH      1920
#define STREAM_CAM_HEIGHT     1080
#define STREAM_CAM_BUF_NUM    3
#define STREAM_OUT_W          640
#define STREAM_OUT_H          480
#define STREAM_JPEG_QUALITY   70
#define STREAM_FPS_DELAY_US   200000       /* 200ms ≈ 5fps */
#define MAX_FRAME_CBS         4
#define DUMP_TIMEOUT_MS       1000

/* YOLOv8 默认配置 */
#define YOLO_DEFAULT_KMODEL   "/root/app/object_detect_yolov8n/yolov8n_320.kmodel"
#define YOLO_DEFAULT_SCORE    0.5f
#define YOLO_DEFAULT_NMS      0.6f

/* ── COCO 80 类颜色盘（RGB，供画框使用） ── */
static const uint8_t s_colors[][3] = {
    {220,20,60},{119,11,32},{0,0,142},{0,0,230},{106,0,228},{0,60,100},{0,80,100},{0,0,70},
    {0,0,192},{250,170,30},{100,170,30},{220,220,0},{175,116,175},{250,0,30},{165,42,42},{255,77,255},
    {0,226,252},{182,182,255},{0,82,0},{120,166,157},{110,76,0},{174,57,255},{199,100,0},{72,0,118},
    {255,179,240},{0,125,92},{209,0,151},{188,208,182},{0,220,176},{255,99,164},{92,0,73},{133,129,255},
    {78,180,255},{0,228,0},{174,255,243},{45,89,255},{134,134,103},{145,148,174},{255,208,186},
    {197,226,255},{171,134,1},{109,63,54},{207,138,255},{151,0,95},{9,80,61},{84,105,51},{74,65,105},
    {166,196,102},{208,195,210},{255,109,65},{0,143,149},{179,0,194},{209,99,106},{5,121,0},{227,255,205},
    {147,186,208},{153,69,1},{3,95,161},{163,255,0},{119,0,170},{0,182,199},{0,165,120},{183,130,88},
    {95,32,0},{130,114,135},{110,129,133},{166,74,118},{219,142,185},{79,210,114},{178,90,62},{65,70,15},
    {127,167,115},{59,105,106},{142,108,45},{196,172,0},{95,54,80},{128,76,255},{201,57,1},{246,0,122},
    {191,162,208}
};

/* ── 帧回调列表 ── */

typedef struct {
    stream_frame_cb_t cb;
    void *user_data;
} frame_callback_t;

/* ── 全局状态 ── */

static struct {
    volatile int running;
    volatile int stopping;
    volatile int stream_ready;     /* 流媒体线程 V4L2 就绪标志 */
    pthread_t    stream_thread;
    pthread_t    ai_thread;

    pthread_mutex_t  cb_mutex;
    frame_callback_t cbs[MAX_FRAME_CBS];
    int              cb_count;

    pthread_mutex_t  det_mutex;
    yolo_snapshot_t  det_snapshot;

    float fps;
    int   frame_count;
    time_t fps_window_start;
} s_stream = {
    .running = 0,
    .stopping = 0,
    .stream_thread = {},
    .ai_thread = {},
    .cb_mutex   = PTHREAD_MUTEX_INITIALIZER,
    .cbs = {},
    .cb_count   = 0,
    .det_mutex  = PTHREAD_MUTEX_INITIALIZER,
    .det_snapshot = {},
    .fps = 0,
    .frame_count = 0,
    .fps_window_start = 0,
};

/* ── 绘图工具 ── */

/* 在 RGB 图像上画矩形框（2px 粗） */
static void draw_rect(uint8_t *rgb, int img_w, int img_h,
                       int rx, int ry, int rw, int rh,
                       uint8_t r, uint8_t g, uint8_t b)
{
    for (int t = 0; t < 2; t++) {
        int y = ry + t;
        if (y >= 0 && y < img_h)
            for (int x = rx; x < rx + rw && x < img_w; x++)
                if (x >= 0) { int i = (y*img_w+x)*3; rgb[i]=r; rgb[i+1]=g; rgb[i+2]=b; }
        y = ry + rh - 1 - t;
        if (y >= 0 && y < img_h)
            for (int x = rx; x < rx + rw && x < img_w; x++)
                if (x >= 0) { int i = (y*img_w+x)*3; rgb[i]=r; rgb[i+1]=g; rgb[i+2]=b; }
        int x = rx + t;
        if (x >= 0 && x < img_w)
            for (int y2 = ry; y2 < ry + rh && y2 < img_h; y2++)
                if (y2 >= 0) { int i = (y2*img_w+x)*3; rgb[i]=r; rgb[i+1]=g; rgb[i+2]=b; }
        x = rx + rw - 1 - t;
        if (x >= 0 && x < img_w)
            for (int y2 = ry; y2 < ry + rh && y2 < img_h; y2++)
                if (y2 >= 0) { int i = (y2*img_w+x)*3; rgb[i]=r; rgb[i+1]=g; rgb[i+2]=b; }
    }
}

/* 5x7 位图字体 */
static const uint8_t s_font_alpha[][7] = {
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, /* A */
    {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}, /* B */
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, /* C */
    {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}, /* D */
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, /* E */
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}, /* F */
    {0x0E,0x11,0x10,0x17,0x11,0x11,0x0E}, /* G */
    {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}, /* H */
    {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, /* I */
    {0x07,0x02,0x02,0x02,0x02,0x12,0x0C}, /* J */
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, /* K */
    {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}, /* L */
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, /* M */
    {0x11,0x19,0x15,0x13,0x11,0x11,0x11}, /* N */
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, /* O */
    {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, /* P */
    {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, /* Q */
    {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}, /* R */
    {0x0E,0x11,0x10,0x0E,0x01,0x11,0x0E}, /* S */
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, /* T */
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, /* U */
    {0x11,0x11,0x11,0x11,0x0A,0x0A,0x04}, /* V */
    {0x11,0x11,0x11,0x15,0x15,0x1B,0x11}, /* W */
    {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}, /* X */
    {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}, /* Y */
    {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}, /* Z */
};

static const uint8_t s_font_digit[][7] = {
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, /* 0 */
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, /* 1 */
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, /* 2 */
    {0x0E,0x11,0x01,0x06,0x01,0x11,0x0E}, /* 3 */
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, /* 4 */
    {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, /* 5 */
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, /* 6 */
    {0x1F,0x01,0x02,0x04,0x04,0x04,0x04}, /* 7 */
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, /* 8 */
    {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}, /* 9 */
};

static void draw_char(uint8_t *rgb, int img_w, int img_h,
                       int cx, int cy, char ch, int factor,
                       uint8_t r, uint8_t g, uint8_t b)
{
    const uint8_t *glyph = NULL;
    if (ch >= 'A' && ch <= 'Z') glyph = s_font_alpha[ch - 'A'];
    else if (ch >= 'a' && ch <= 'z') glyph = s_font_alpha[ch - 'a'];
    else if (ch >= '0' && ch <= '9') glyph = s_font_digit[ch - '0'];
    else if (ch == ' ' || ch == '_') return;
    else if (ch == '.') {
        for (int fy = 0; fy < factor; fy++)
            for (int fx = 0; fx < factor; fx++) {
                int px = cx + 2*factor + fx, py = cy + 6*factor + fy;
                if (px >= 0 && px < img_w && py >= 0 && py < img_h) {
                    int i = (py*img_w+px)*3; rgb[i]=r; rgb[i+1]=g; rgb[i+2]=b;
                }
            }
        return;
    }
    else return;

    for (int row = 0; row < 7; row++)
        for (int col = 0; col < 5; col++)
            if (glyph[row] & (0x10 >> col))
                for (int fy = 0; fy < factor; fy++)
                    for (int fx = 0; fx < factor; fx++) {
                        int px = cx + col*factor + fx;
                        int py = cy + row*factor + fy;
                        if (px >= 0 && px < img_w && py >= 0 && py < img_h) {
                            int i = (py*img_w+px)*3;
                            rgb[i]=r; rgb[i+1]=g; rgb[i+2]=b;
                        }
                    }
}

static void draw_text(uint8_t *rgb, int img_w, int img_h,
                       int x, int y, const char *text, int factor,
                       uint8_t r, uint8_t g, uint8_t b)
{
    int cx = x;
    while (*text) {
        draw_char(rgb, img_w, img_h, cx, y, *text, factor, r, g, b);
        cx += 6 * factor;
        text++;
    }
}

/* ── 检测框叠加 ── */

static void draw_detection_overlays(uint8_t *rgb, int img_w, int img_h)
{
    yolo_snapshot_t snap;
    pthread_mutex_lock(&s_stream.det_mutex);
    snap = s_stream.det_snapshot;
    pthread_mutex_unlock(&s_stream.det_mutex);

    float sx = (float)img_w / STREAM_CAM_WIDTH;
    float sy = (float)img_h / STREAM_CAM_HEIGHT;

    for (int i = 0; i < snap.count; i++) {
        yolo_detection_t *d = &snap.detections[i];
        int rx = (int)(d->x * sx);
        int ry = (int)(d->y * sy);
        int rw = (int)(d->w * sx);
        int rh = (int)(d->h * sy);

        int ci = d->class_id % 80;
        uint8_t cr = s_colors[ci][0];
        uint8_t cg = s_colors[ci][1];
        uint8_t cb = s_colors[ci][2];

        draw_rect(rgb, img_w, img_h, rx, ry, rw, rh, cr, cg, cb);

        /* 标签：类名 + 置信度 */
        char label[48];
        int conf_int = (int)(d->confidence * 100);
        snprintf(label, sizeof(label), "%s %d", d->class_name, conf_int);

        int label_y = ry - 10;
        if (label_y < 0) label_y = ry + rh + 2;
        draw_text(rgb, img_w, img_h, rx, label_y, label, 1, cr, cg, cb);
    }
}

/* ── 帧回调分发 ── */

static void invoke_callbacks(const uint8_t *jpeg, size_t len)
{
    pthread_mutex_lock(&s_stream.cb_mutex);
    for (int i = 0; i < s_stream.cb_count; i++) {
        if (s_stream.cbs[i].cb)
            s_stream.cbs[i].cb(jpeg, len, s_stream.cbs[i].user_data);
    }
    pthread_mutex_unlock(&s_stream.cb_mutex);
}

/* ── AI 推理线程（/dev/video2 BG3P） ── */

static void *ai_thread_func(void *arg)
{
    (void)arg;

    /* 等待流媒体线程完成 V4L2 初始化（ISP 不能并发 setup） */
    KC_LOGI(TAG, "AI thread waiting for stream V4L2 ready...");
    for (int i = 0; i < 50 && !s_stream.stream_ready && !s_stream.stopping; i++)
        usleep(100000); /* 100ms × 50 = 最多等 5 秒 */

    if (s_stream.stopping) return NULL;
    KC_LOGI(TAG, "AI thread proceeding");

    struct v4l2_drm_context ctx;
    v4l2_drm_default_context(&ctx);
    ctx.device = AI_CAM_DEVICE;
    ctx.display = false;
    ctx.width = STREAM_CAM_WIDTH;
    ctx.height = STREAM_CAM_HEIGHT;
    ctx.video_format = v4l2_fourcc('B', 'G', '3', 'P');
    ctx.buffer_num = STREAM_CAM_BUF_NUM;

    if (v4l2_drm_setup(&ctx, 1, NULL) != 0) {
        KC_LOGE(TAG, "AI V4L2 setup failed");
        return NULL;
    }
    if (v4l2_drm_start(&ctx) != 0) {
        KC_LOGE(TAG, "AI V4L2 start failed");
        v4l2_drm_stop(&ctx);
        return NULL;
    }

    const char *kmodel = kc_config_get_str("yolo_kmodel", YOLO_DEFAULT_KMODEL);
    kc_err_t err = yolo_detect_init(kmodel, YOLO_DEFAULT_SCORE, YOLO_DEFAULT_NMS);
    if (err != KC_OK) {
        KC_LOGE(TAG, "YOLOv8 init failed, AI thread exiting");
        v4l2_drm_stop(&ctx);
        return NULL;
    }

    KC_LOGI(TAG, "AI thread started (YOLOv8n on /dev/video%d)", AI_CAM_DEVICE);

    while (!s_stream.stopping) {
        int ret = v4l2_drm_dump(&ctx, DUMP_TIMEOUT_MS);
        if (ret != 0) continue;

        unsigned buf_idx = ctx.vbuffer.index;
        const void *frame = ctx.buffers[buf_idx].mmap;

        yolo_snapshot_t snap;
        memset(&snap, 0, sizeof(snap));
        yolo_detect_run(frame, STREAM_CAM_WIDTH, STREAM_CAM_HEIGHT, &snap);

        pthread_mutex_lock(&s_stream.det_mutex);
        s_stream.det_snapshot = snap;
        pthread_mutex_unlock(&s_stream.det_mutex);

        v4l2_drm_dump_release(&ctx);
    }

    yolo_detect_deinit();
    v4l2_drm_stop(&ctx);
    KC_LOGI(TAG, "AI thread stopped");
    return NULL;
}

/* ── 流媒体捕获线程（/dev/video1 NV12） ── */

static void *stream_thread_func(void *arg)
{
    (void)arg;

    struct v4l2_drm_context ctx;
    v4l2_drm_default_context(&ctx);
    ctx.device = STREAM_CAM_DEVICE;
    ctx.display = false;
    ctx.width = STREAM_CAM_WIDTH;
    ctx.height = STREAM_CAM_HEIGHT;
    ctx.video_format = V4L2_PIX_FMT_NV12;
    ctx.buffer_num = STREAM_CAM_BUF_NUM;

    if (v4l2_drm_setup(&ctx, 1, NULL) != 0) {
        KC_LOGE(TAG, "Stream V4L2 setup failed");
        s_stream.running = 0;
        return NULL;
    }
    if (v4l2_drm_start(&ctx) != 0) {
        KC_LOGE(TAG, "Stream V4L2 start failed");
        v4l2_drm_stop(&ctx);
        s_stream.running = 0;
        return NULL;
    }

    /* 通知 AI 线程：流媒体 V4L2 已就绪 */
    s_stream.stream_ready = 1;

    for (int i = 0; i < 3; i++) {
        if (v4l2_drm_dump(&ctx, DUMP_TIMEOUT_MS) == 0)
            v4l2_drm_dump_release(&ctx);
    }

    KC_LOGI(TAG, "stream capture started (%dx%d @ ~5fps)", STREAM_OUT_W, STREAM_OUT_H);

    size_t rgb_size = STREAM_OUT_W * STREAM_OUT_H * 3;
    uint8_t *rgb = (uint8_t *)malloc(rgb_size);

    s_stream.fps = 0;
    s_stream.frame_count = 0;
    s_stream.fps_window_start = time(NULL);

    while (!s_stream.stopping) {
        if (v4l2_drm_dump(&ctx, DUMP_TIMEOUT_MS) != 0)
            continue;

        unsigned buf_idx = ctx.vbuffer.index;
        const uint8_t *nv12 = (const uint8_t *)ctx.buffers[buf_idx].mmap;

        nv12_to_rgb_downscale(nv12, STREAM_CAM_WIDTH, STREAM_CAM_HEIGHT,
                              rgb, STREAM_OUT_W, STREAM_OUT_H);
        v4l2_drm_dump_release(&ctx);

        draw_detection_overlays(rgb, STREAM_OUT_W, STREAM_OUT_H);

        uint8_t *jpeg = NULL;
        size_t jpeg_len = 0;
        if (rgb_to_jpeg_mem(rgb, STREAM_OUT_W, STREAM_OUT_H,
                            STREAM_JPEG_QUALITY, &jpeg, &jpeg_len) == 0) {
            invoke_callbacks(jpeg, jpeg_len);
            free(jpeg);
        }

        s_stream.frame_count++;
        time_t now = time(NULL);
        double elapsed = difftime(now, s_stream.fps_window_start);
        if (elapsed >= 2.0) {
            s_stream.fps = (float)(s_stream.frame_count / elapsed);
            s_stream.frame_count = 0;
            s_stream.fps_window_start = now;
        }

        usleep(STREAM_FPS_DELAY_US);
    }

    free(rgb);
    v4l2_drm_stop(&ctx);
    s_stream.running = 0;
    KC_LOGI(TAG, "stream capture stopped");
    return NULL;
}

/* ── 公共 API ── */

extern "C" {

kc_err_t hal_camera_stream_start(void)
{
    if (s_stream.running) return KC_OK;

    s_stream.stopping = 0;
    s_stream.running = 1;
    s_stream.stream_ready = 0;
    s_stream.cb_count = 0;
    memset(&s_stream.det_snapshot, 0, sizeof(s_stream.det_snapshot));

    /* 先启动流媒体线程（ISP 不能并发初始化） */
    if (pthread_create(&s_stream.stream_thread, NULL, stream_thread_func, NULL) != 0) {
        KC_LOGE(TAG, "failed to create stream thread");
        s_stream.running = 0;
        return KC_FAIL;
    }

    /* 再启动 AI 线程（内部会等待 stream_ready） */
    if (pthread_create(&s_stream.ai_thread, NULL, ai_thread_func, NULL) != 0) {
        KC_LOGE(TAG, "failed to create AI thread");
        s_stream.stopping = 1;
        pthread_join(s_stream.stream_thread, NULL);
        s_stream.running = 0;
        return KC_FAIL;
    }

    KC_LOGI(TAG, "stream HAL started (2 threads: AI + capture)");
    return KC_OK;
}

void hal_camera_stream_stop(void)
{
    if (!s_stream.running) return;

    s_stream.stopping = 1;
    pthread_join(s_stream.ai_thread, NULL);
    pthread_join(s_stream.stream_thread, NULL);

    pthread_mutex_lock(&s_stream.cb_mutex);
    s_stream.cb_count = 0;
    pthread_mutex_unlock(&s_stream.cb_mutex);

    KC_LOGI(TAG, "stream HAL stopped");
}

int hal_camera_stream_is_running(void)
{
    return s_stream.running;
}

void hal_camera_stream_on_frame(stream_frame_cb_t cb, void *user_data)
{
    pthread_mutex_lock(&s_stream.cb_mutex);
    if (s_stream.cb_count < MAX_FRAME_CBS) {
        s_stream.cbs[s_stream.cb_count].cb = cb;
        s_stream.cbs[s_stream.cb_count].user_data = user_data;
        s_stream.cb_count++;
    }
    pthread_mutex_unlock(&s_stream.cb_mutex);
}

void hal_camera_stream_remove_frame_cb(stream_frame_cb_t cb)
{
    pthread_mutex_lock(&s_stream.cb_mutex);
    for (int i = 0; i < s_stream.cb_count; i++) {
        if (s_stream.cbs[i].cb == cb) {
            s_stream.cbs[i] = s_stream.cbs[--s_stream.cb_count];
            break;
        }
    }
    pthread_mutex_unlock(&s_stream.cb_mutex);
}

float hal_camera_stream_get_fps(void)
{
    return s_stream.fps;
}

void hal_camera_stream_get_detections(yolo_snapshot_t *out)
{
    pthread_mutex_lock(&s_stream.det_mutex);
    *out = s_stream.det_snapshot;
    pthread_mutex_unlock(&s_stream.det_mutex);
}

} /* extern "C" */

#else /* !KC_HAS_K230_HW */

extern "C" {

kc_err_t hal_camera_stream_start(void)                              { return KC_ERR_NOT_FOUND; }
void     hal_camera_stream_stop(void)                               {}
int      hal_camera_stream_is_running(void)                         { return 0; }
void     hal_camera_stream_on_frame(stream_frame_cb_t cb, void *ud) { (void)cb; (void)ud; }
void     hal_camera_stream_remove_frame_cb(stream_frame_cb_t cb)    { (void)cb; }
float    hal_camera_stream_get_fps(void)                            { return 0; }
void     hal_camera_stream_get_detections(yolo_snapshot_t *o)       { memset(o, 0, sizeof(*o)); }

} /* extern "C" */

#endif /* KC_HAS_K230_HW */
