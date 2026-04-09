#pragma once

/*
 * tts_engine.h - 本地 KPU TTS 引擎 C API
 *
 * 三阶段管道：FastSpeech1(编码) → FastSpeech2(解码) → HiFiGan(声码器)
 * 文本预处理：zh_frontend + pypinyin + cppjieba（纯 C++，无外部依赖）
 * 输出：24kHz 单声道 S16_LE PCM
 */

#ifdef __cplusplus
extern "C" {
#endif

#include "../../kc_hal.h"
#include <stdint.h>

/*
 * 初始化本地 TTS 引擎
 * dict_dir:   拼音字典目录（含 pinyin.txt, small_pinyin.txt, phone_id_map_en.txt）
 * kmodel_dir: kmodel 文件目录（含 zh_fastspeech_1/2.kmodel, hifigan.kmodel）
 */
kc_err_t kc_tts_init(const char *dict_dir, const char *kmodel_dir);

/*
 * 文本转语音：文本 → KPU 推理 → ALSA 播放
 * 自动分块处理长文本（每块 <=50 音素）
 */
kc_err_t kc_tts_speak(const char *text);

/*
 * 文本转 PCM（不播放，返回 PCM 缓冲区）
 * 调用者 free(*pcm_out)
 */
kc_err_t kc_tts_synthesize(const char *text,
                            int16_t **pcm_out, int *total_frames,
                            int *sample_rate);

/* TTS 是否已初始化 */
int kc_tts_available(void);

void kc_tts_deinit(void);

#ifdef __cplusplus
}
#endif
