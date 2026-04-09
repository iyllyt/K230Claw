/*
 * sensor_voice.c - 语音唤醒传感器
 *
 * 后台线程：ALSA 录音 → Fbank 特征提取 → KPU KWS 推理
 * 检测到唤醒词后推事件给 Agent Loop。
 *
 * 模型: kws.kmodel (WeNet-based, 流式缓存推理)
 * 输入: features [1,30,40] + cache [1,256,105]
 * 输出: logits [30,num_keyword] + new_cache [256,105]
 *
 * 条件编译: KC_HAS_K230_HW
 */

#include "sensor_voice.h"
#include "sensor_state.h"
#include "../kc_config.h"

#ifdef KC_HAS_K230_HW

#include "../hal/hal_audio.h"
#include "../hal/kpu_wrapper.h"
#include "../hal/kws_features.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <math.h>

#define TAG "voice_wake"

/* ── 配置 ── */
#define KWS_SAMPLE_RATE   16000
#define KWS_NUM_BINS      40
#define KWS_CHUNK_SIZE    30        /* 推理帧数 */
#define KWS_HIDDEN_DIM    256
#define KWS_CACHE_DIM     105
#define KWS_PCM_FRAMES    4800      /* 每次录音 300ms */
#define KWS_THRESHOLD     0.5f      /* 默认唤醒阈值（匹配 SDK kws demo） */

/* ── 上下文 ── */

typedef struct {
    pthread_t thread;
    volatile int stopping;
    volatile int paused;
    kpu_model_t *model;
    int num_keywords;
    float cache[KWS_HIDDEN_DIM * KWS_CACHE_DIM]; /* 持久缓存 */
} voice_ctx_t;

static voice_ctx_t s_voice_ctx;

static void *voice_thread(void *arg)
{
    voice_ctx_t *ctx = (voice_ctx_t *)arg;

    /* 打开麦克风 */
    kc_audio_capture_t *cap = NULL;
    kc_err_t err = hal_audio_capture_open(&cap, KWS_SAMPLE_RATE, 1);
    if (err != KC_OK) {
        KC_LOGE(TAG, "failed to open microphone");
        return NULL;
    }

    KC_LOGI(TAG, "voice wake started (keywords=%d)", ctx->num_keywords);

    /* 分配 PCM + 特征缓冲区 */
    int16_t *pcm_buf = (int16_t *)malloc(KWS_PCM_FRAMES * sizeof(int16_t));
    float *wav_buf = (float *)malloc(KWS_PCM_FRAMES * sizeof(float));
    float *features = (float *)malloc(KWS_CHUNK_SIZE * KWS_NUM_BINS * sizeof(float));

    if (!pcm_buf || !wav_buf || !features) {
        KC_LOGE(TAG, "out of memory");
        hal_audio_capture_close(cap);
        free(pcm_buf); free(wav_buf); free(features);
        return NULL;
    }

    /* 获取阈值 */
    const char *thresh_str = kc_config_get_str("kws_threshold", "");
    float threshold = thresh_str[0] ? (float)atof(thresh_str) : KWS_THRESHOLD;

    while (!ctx->stopping && !kc_is_shutting_down()) {
        /* 暂停检查（record_audio 抢占麦克风） */
        if (ctx->paused) {
            hal_audio_capture_close(cap);
            cap = NULL;
            KC_LOGI(TAG, "paused (mic released)");
            while (ctx->paused && !ctx->stopping && !kc_is_shutting_down())
                usleep(100000);
            if (ctx->stopping || kc_is_shutting_down()) break;
            err = hal_audio_capture_open(&cap, KWS_SAMPLE_RATE, 1);
            if (err != KC_OK) {
                KC_LOGE(TAG, "failed to reopen mic after pause");
                break;
            }
            kws_features_reset();
            KC_LOGI(TAG, "resumed");
        }

        /* 1. 录音 300ms */
        int read = hal_audio_capture_read(cap, pcm_buf, KWS_PCM_FRAMES);
        if (read <= 0) {
            usleep(100000);
            continue;
        }

        /* 2. int16 → float（不归一化，匹配 SDK） */
        for (int i = 0; i < read; i++)
            wav_buf[i] = (float)pcm_buf[i];

        /* 3. Fbank 特征提取 */
        int nframes = kws_features_compute(wav_buf, read,
                                            features, KWS_CHUNK_SIZE);
        if (nframes < KWS_CHUNK_SIZE) {
            /* 不够 30 帧，继续录音 */
            continue;
        }

        /* 4. 设置模型输入 */
        kpu_model_set_input(ctx->model, 0, features,
                            KWS_CHUNK_SIZE * KWS_NUM_BINS * sizeof(float));
        kpu_model_set_input(ctx->model, 1, ctx->cache,
                            KWS_HIDDEN_DIM * KWS_CACHE_DIM * sizeof(float));

        /* 5. KPU 推理 */
        if (kpu_model_run(ctx->model) != KC_OK) {
            KC_LOGW(TAG, "KPU run failed");
            continue;
        }

        /* 6. 更新缓存 */
        float *new_cache = kpu_model_output_data(ctx->model, 1);
        if (new_cache) {
            memcpy(ctx->cache, new_cache,
                   KWS_HIDDEN_DIM * KWS_CACHE_DIM * sizeof(float));
        }

        /* 7. 检测唤醒词 */
        float *logits = kpu_model_output_data(ctx->model, 0);
        if (!logits) continue;

        /* 对每个关键词取 30 帧中的最大分数 */
        float max_score = -1e9f;
        int max_idx = 0;
        for (int j = 1; j < ctx->num_keywords; j++) { /* 跳过 0（静音类） */
            float score = -1e9f;
            for (int i = 0; i < KWS_CHUNK_SIZE; i++) {
                float s = logits[ctx->num_keywords * i + j];
                if (s > score) score = s;
            }
            if (score > max_score) {
                max_score = score;
                max_idx = j;
            }
        }

        if (max_score > threshold && max_idx > 0) {
            /* 只在首次检测或距上次事件超过 30 秒时触发 */
            static time_t last_wake = 0;
            time_t now = time(NULL);
            if (now - last_wake >= 30) {
                KC_LOGI(TAG, "wake word detected! keyword=%d score=%.3f",
                        max_idx, max_score);
                sensor_state_set("wake_word_detected", "true");
                sensor_push_event("wake_word",
                    "Wake word detected! The user wants to speak. "
                    "Use record_audio to listen to what they want to say.");
                last_wake = now;
            }
        }
    }

    if (cap) hal_audio_capture_close(cap);
    free(pcm_buf);
    free(wav_buf);
    free(features);

    KC_LOGI(TAG, "voice wake stopped");
    return NULL;
}

/* ── 传感器接口 ── */

static kc_err_t voice_start(kc_sensor_t *self)
{
    (void)self;
    voice_ctx_t *ctx = &s_voice_ctx;

    /* 初始化特征提取 */
    if (kws_features_init(KWS_SAMPLE_RATE, KWS_NUM_BINS) != KC_OK) {
        KC_LOGE(TAG, "features init failed");
        return KC_FAIL;
    }

    /* 加载模型 */
    const char *kmodel = kc_config_get_str("kws_kmodel",
        "/root/app/kws/kws.kmodel");

    if (kpu_model_load(kmodel, &ctx->model) != KC_OK) {
        KC_LOGE(TAG, "failed to load KWS model: %s", kmodel);
        kws_features_deinit();
        return KC_FAIL;
    }

    /* 确定关键词数量 */
    /* SDK kws demo 默认 2 个关键词（0=静音, 1=唤醒词），从配置或 shape 最后一维获取 */
    int shape[4] = {0};
    int ndim = 0;
    kpu_model_output_shape(ctx->model, 0, shape, &ndim);
    /* shape 可能是 [30,2] 或 [1,30,2]，取最后一维作为关键词数 */
    ctx->num_keywords = (ndim >= 1) ? shape[ndim - 1] : 2;
    if (ctx->num_keywords <= 0 || ctx->num_keywords > 100)
        ctx->num_keywords = 2;  /* fallback */
    KC_LOGI(TAG, "KWS model: %d keywords (output shape: ndim=%d [%d,%d,%d,%d])",
            ctx->num_keywords, ndim, shape[0], shape[1], shape[2], shape[3]);

    /* 初始化缓存为零 */
    memset(ctx->cache, 0, sizeof(ctx->cache));

    /* 启动线程 */
    ctx->stopping = 0;
    ctx->paused = 0;
    if (pthread_create(&ctx->thread, NULL, voice_thread, ctx) != 0) {
        KC_LOGE(TAG, "thread create failed");
        kpu_model_free(ctx->model);
        kws_features_deinit();
        return KC_FAIL;
    }

    return KC_OK;
}

static kc_err_t voice_stop(kc_sensor_t *self)
{
    (void)self;
    voice_ctx_t *ctx = &s_voice_ctx;
    ctx->stopping = 1;
    pthread_join(ctx->thread, NULL);

    if (ctx->model) { kpu_model_free(ctx->model); ctx->model = NULL; }
    kws_features_deinit();

    KC_LOGI(TAG, "voice wake stopped");
    return KC_OK;
}

static kc_sensor_t s_voice_sensor = {
    .name = "voice_wake",
    .start = voice_start,
    .stop = voice_stop,
    .ctx = &s_voice_ctx
};

kc_sensor_t *voice_wake_get(void) { return &s_voice_sensor; }

void voice_wake_pause(void)
{
    s_voice_ctx.paused = 1;
    KC_LOGI(TAG, "pause requested");
}

void voice_wake_resume(void)
{
    s_voice_ctx.paused = 0;
    KC_LOGI(TAG, "resume requested");
}

#else /* !KC_HAS_K230_HW */

kc_sensor_t *voice_wake_get(void) { return NULL; }
void voice_wake_pause(void) {}
void voice_wake_resume(void) {}

#endif /* KC_HAS_K230_HW */
