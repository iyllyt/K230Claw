#pragma once

/*
 * kws_features.h - KWS Fbank 特征提取 C API
 *
 * 封装 FFT + Fbank 计算为简洁接口。
 * 参数匹配 SDK KWS demo: 16kHz, 40 mel bins, 25ms 帧, 10ms 步长。
 */

#ifdef __cplusplus
extern "C" {
#endif

#include "../kc_hal.h"

/*
 * 初始化 Fbank 特征提取器
 * sample_rate: 16000
 * num_bins: 40
 */
kc_err_t kws_features_init(int sample_rate, int num_bins);

/*
 * 从 PCM 采样计算 Fbank 特征
 * wav:         float 采样（int16 直接 cast，不归一化）
 * wav_len:     采样数（4800 = 300ms @16kHz）
 * output:      输出缓冲区，至少 chunk_size * num_bins floats
 * chunk_size:  需要的帧数（30）
 * 返回: 实际产生的帧数
 *
 * 内部维护残余采样缓冲区，支持流式调用。
 */
int kws_features_compute(const float *wav, int wav_len,
                          float *output, int chunk_size);

void kws_features_reset(void);
void kws_features_deinit(void);

#ifdef __cplusplus
}
#endif
