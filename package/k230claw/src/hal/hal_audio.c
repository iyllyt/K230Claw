/*
 * hal_audio.c - ALSA 音频 HAL 封装
 *
 * K230 内置 INNO 编解码器（I2S 接口），ALSA 设备始终 "default"。
 * 格式固定 S16_LE interleaved。
 *
 * 条件编译：
 *   KC_HAS_K230_HW — 真机，使用 ALSA
 *   否则           — x86 stub，返回错误
 */

#include "hal_audio.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define TAG "hal_audio"

#ifdef KC_HAS_K230_HW

#include <alsa/asoundlib.h>

#define ALSA_DEVICE "default"

/* ── 录音结构体 ── */

struct kc_audio_capture {
    snd_pcm_t *pcm;
    snd_pcm_uframes_t period_frames;
    unsigned int sample_rate;
    int channels;
};

kc_err_t hal_audio_capture_open(kc_audio_capture_t **out,
                                unsigned int sample_rate, int channels)
{
    if (!out) return KC_ERR_INVALID;

    kc_audio_capture_t *cap = calloc(1, sizeof(*cap));
    if (!cap) return KC_ERR_NO_MEM;

    cap->sample_rate = sample_rate;
    cap->channels = channels;

    int rc = snd_pcm_open(&cap->pcm, ALSA_DEVICE,
                          SND_PCM_STREAM_CAPTURE, 0);
    if (rc < 0) {
        KC_LOGE(TAG, "capture open failed: %s", snd_strerror(rc));
        free(cap);
        return KC_FAIL;
    }

    snd_pcm_hw_params_t *params;
    snd_pcm_hw_params_alloca(&params);
    snd_pcm_hw_params_any(cap->pcm, params);

    snd_pcm_hw_params_set_access(cap->pcm, params,
                                  SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(cap->pcm, params,
                                  SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(cap->pcm, params, channels);
    snd_pcm_hw_params_set_rate_near(cap->pcm, params, &sample_rate, 0);

    /* period: 4800 帧 @16kHz = 300ms（匹配 KWS demo） */
    snd_pcm_uframes_t period = 4800;
    snd_pcm_hw_params_set_period_size_near(cap->pcm, params, &period, 0);

    rc = snd_pcm_hw_params(cap->pcm, params);
    if (rc < 0) {
        KC_LOGE(TAG, "capture hw_params failed: %s", snd_strerror(rc));
        snd_pcm_close(cap->pcm);
        free(cap);
        return KC_FAIL;
    }

    snd_pcm_hw_params_get_period_size(params, &cap->period_frames, 0);
    KC_LOGI(TAG, "capture opened: rate=%u ch=%d period=%lu",
            sample_rate, channels, (unsigned long)cap->period_frames);

    *out = cap;
    return KC_OK;
}

int hal_audio_capture_read(kc_audio_capture_t *cap,
                           int16_t *buf, int frames)
{
    if (!cap || !buf || frames <= 0) return -1;

    int total_read = 0;
    while (total_read < frames) {
        int to_read = frames - total_read;
        snd_pcm_sframes_t rc = snd_pcm_readi(
            cap->pcm,
            buf + total_read * cap->channels,
            to_read);

        if (rc == -EPIPE) {
            KC_LOGW(TAG, "capture overrun, recovering");
            snd_pcm_prepare(cap->pcm);
            continue;
        } else if (rc < 0) {
            KC_LOGE(TAG, "capture read error: %s", snd_strerror((int)rc));
            return total_read > 0 ? total_read : (int)rc;
        }
        total_read += (int)rc;
    }
    return total_read;
}

void hal_audio_capture_close(kc_audio_capture_t *cap)
{
    if (!cap) return;
    snd_pcm_drain(cap->pcm);
    snd_pcm_close(cap->pcm);
    free(cap);
    KC_LOGI(TAG, "capture closed");
}

/* ── 播放结构体 ── */

struct kc_audio_playback {
    snd_pcm_t *pcm;
    snd_pcm_uframes_t period_frames;
    unsigned int sample_rate;
    int channels;
};

kc_err_t hal_audio_playback_open(kc_audio_playback_t **out,
                                 unsigned int sample_rate, int channels)
{
    if (!out) return KC_ERR_INVALID;

    kc_audio_playback_t *pb = calloc(1, sizeof(*pb));
    if (!pb) return KC_ERR_NO_MEM;

    pb->sample_rate = sample_rate;
    pb->channels = channels;

    int rc = snd_pcm_open(&pb->pcm, ALSA_DEVICE,
                          SND_PCM_STREAM_PLAYBACK, 0);
    if (rc < 0) {
        KC_LOGE(TAG, "playback open failed: %s", snd_strerror(rc));
        free(pb);
        return KC_FAIL;
    }

    snd_pcm_hw_params_t *params;
    snd_pcm_hw_params_alloca(&params);
    snd_pcm_hw_params_any(pb->pcm, params);

    snd_pcm_hw_params_set_access(pb->pcm, params,
                                  SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pb->pcm, params,
                                  SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(pb->pcm, params, channels);
    snd_pcm_hw_params_set_rate_near(pb->pcm, params, &sample_rate, 0);

    /* period: sample_rate / 25 = 40ms（匹配 audio_demo） */
    snd_pcm_uframes_t period = sample_rate / 25;
    snd_pcm_hw_params_set_period_size_near(pb->pcm, params, &period, 0);

    rc = snd_pcm_hw_params(pb->pcm, params);
    if (rc < 0) {
        KC_LOGE(TAG, "playback hw_params failed: %s", snd_strerror(rc));
        snd_pcm_close(pb->pcm);
        free(pb);
        return KC_FAIL;
    }

    snd_pcm_hw_params_get_period_size(params, &pb->period_frames, 0);
    KC_LOGI(TAG, "playback opened: rate=%u ch=%d period=%lu",
            sample_rate, channels, (unsigned long)pb->period_frames);

    *out = pb;
    return KC_OK;
}

int hal_audio_playback_write(kc_audio_playback_t *pb,
                             const int16_t *buf, int frames)
{
    if (!pb || !buf || frames <= 0) return -1;

    int total_written = 0;
    while (total_written < frames) {
        int to_write = frames - total_written;
        snd_pcm_sframes_t rc = snd_pcm_writei(
            pb->pcm,
            buf + total_written * pb->channels,
            to_write);

        if (rc == -EPIPE) {
            KC_LOGW(TAG, "playback underrun, recovering");
            snd_pcm_prepare(pb->pcm);
            continue;
        } else if (rc < 0) {
            KC_LOGE(TAG, "playback write error: %s", snd_strerror((int)rc));
            return total_written > 0 ? total_written : (int)rc;
        }
        total_written += (int)rc;
    }
    return total_written;
}

void hal_audio_playback_close(kc_audio_playback_t *pb)
{
    if (!pb) return;
    snd_pcm_drain(pb->pcm);
    snd_pcm_close(pb->pcm);
    free(pb);
    KC_LOGI(TAG, "playback closed");
}

/* ── 便捷播放 ── */

kc_err_t hal_audio_play_buffer(const int16_t *pcm, int total_frames,
                               unsigned int sample_rate, int channels)
{
    kc_audio_playback_t *pb = NULL;
    kc_err_t err = hal_audio_playback_open(&pb, sample_rate, channels);
    if (err != KC_OK) return err;

    int written = hal_audio_playback_write(pb, pcm, total_frames);
    hal_audio_playback_close(pb);

    return (written == total_frames) ? KC_OK : KC_FAIL;
}

/* ── 麦克风共享 ── */

static pthread_mutex_t s_exclusive_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile int s_exclusive_request = 0;

void hal_audio_capture_request_exclusive(void)
{
    pthread_mutex_lock(&s_exclusive_mutex);
    s_exclusive_request = 1;
    pthread_mutex_unlock(&s_exclusive_mutex);
    KC_LOGI(TAG, "exclusive capture requested");
}

void hal_audio_capture_release_exclusive(void)
{
    pthread_mutex_lock(&s_exclusive_mutex);
    s_exclusive_request = 0;
    pthread_mutex_unlock(&s_exclusive_mutex);
    KC_LOGI(TAG, "exclusive capture released");
}

int hal_audio_capture_exclusive_requested(void)
{
    return s_exclusive_request;
}

#else /* !KC_HAS_K230_HW — x86 stub */

struct kc_audio_capture  { int dummy; };
struct kc_audio_playback { int dummy; };

kc_err_t hal_audio_capture_open(kc_audio_capture_t **out,
                                unsigned int sample_rate, int channels)
{
    (void)out; (void)sample_rate; (void)channels;
    KC_LOGW(TAG, "Audio HAL not available (x86 stub)");
    return KC_ERR_NOT_FOUND;
}

int hal_audio_capture_read(kc_audio_capture_t *cap,
                           int16_t *buf, int frames)
{
    (void)cap; (void)buf; (void)frames;
    return -1;
}

void hal_audio_capture_close(kc_audio_capture_t *cap) { (void)cap; }

kc_err_t hal_audio_playback_open(kc_audio_playback_t **out,
                                 unsigned int sample_rate, int channels)
{
    (void)out; (void)sample_rate; (void)channels;
    KC_LOGW(TAG, "Audio HAL not available (x86 stub)");
    return KC_ERR_NOT_FOUND;
}

int hal_audio_playback_write(kc_audio_playback_t *pb,
                             const int16_t *buf, int frames)
{
    (void)pb; (void)buf; (void)frames;
    return -1;
}

void hal_audio_playback_close(kc_audio_playback_t *pb) { (void)pb; }

kc_err_t hal_audio_play_buffer(const int16_t *pcm, int total_frames,
                               unsigned int sample_rate, int channels)
{
    (void)pcm; (void)total_frames; (void)sample_rate; (void)channels;
    return KC_ERR_NOT_FOUND;
}

void hal_audio_capture_request_exclusive(void) {}
void hal_audio_capture_release_exclusive(void) {}
int  hal_audio_capture_exclusive_requested(void) { return 0; }

#endif /* KC_HAS_K230_HW */
