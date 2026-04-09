#pragma once

/*
 * hal_audio.h - ALSA 音频 HAL 封装
 *
 * 封装 ALSA 录音/播放为简洁 C API，供音频工具和 KWS 传感器共用。
 * K230 内置 INNO 编解码器：
 *   - DAC → HPOutL/R（3.5mm 耳机/喇叭输出）
 *   - ADC → MicL（板载麦克风输入）
 *   - 格式固定 S16_LE + interleaved
 *   - ALSA 设备固定 "default"
 */

#include "../kc_hal.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct kc_audio_capture kc_audio_capture_t;
typedef struct kc_audio_playback kc_audio_playback_t;

/* ── 录音 ── */

kc_err_t hal_audio_capture_open(kc_audio_capture_t **out,
                                unsigned int sample_rate, int channels);
int      hal_audio_capture_read(kc_audio_capture_t *cap,
                                int16_t *buf, int frames);
void     hal_audio_capture_close(kc_audio_capture_t *cap);

/* ── 播放 ── */

kc_err_t hal_audio_playback_open(kc_audio_playback_t **out,
                                 unsigned int sample_rate, int channels);
int      hal_audio_playback_write(kc_audio_playback_t *pb,
                                  const int16_t *buf, int frames);
void     hal_audio_playback_close(kc_audio_playback_t *pb);

/* 便捷：一次性播放整个 PCM 缓冲区 */
kc_err_t hal_audio_play_buffer(const int16_t *pcm, int total_frames,
                               unsigned int sample_rate, int channels);

/* ── 麦克风共享（KWS 传感器 vs record_audio 工具） ── */

/* record_audio 调用：请求独占麦克风 */
void hal_audio_capture_request_exclusive(void);
/* record_audio 完成后：释放独占 */
void hal_audio_capture_release_exclusive(void);
/* KWS 传感器检查：是否有独占请求 */
int  hal_audio_capture_exclusive_requested(void);

#ifdef __cplusplus
}
#endif
