/*
 * kws_features.cpp - KWS Fbank 特征提取 C++ 实现
 *
 * 使用从 SDK kws demo 复制的 Fbank 类（fbank.h + fft.cc）。
 * 实现流式特征提取（残余采样缓冲区）。
 *
 * 参数: 16kHz, 40 mel bins, 400 帧长(25ms), 160 步长(10ms)
 */

#include "kws_features.h"
#include "fbank.h"

#include <vector>
#include <cstring>

#define TAG "kws_feat"

static wenet::Fbank *s_fbank = NULL;
static std::vector<float> s_remained;
static int s_num_bins = 40;
static int s_frame_length = 400;
static int s_frame_shift = 160;

extern "C" {

kc_err_t kws_features_init(int sample_rate, int num_bins)
{
    if (s_fbank) return KC_OK;

    s_num_bins = num_bins;
    s_frame_length = sample_rate / 1000 * 25;  /* 400 @16kHz */
    s_frame_shift = sample_rate / 1000 * 10;   /* 160 @16kHz */

    s_fbank = new (std::nothrow) wenet::Fbank(
        num_bins, sample_rate, s_frame_length, s_frame_shift);
    if (!s_fbank) return KC_ERR_NO_MEM;

    /* 初始填充 320 个零（匹配 SDK FeaturePipeline） */
    s_remained.assign(320, 0.0f);

    KC_LOGI(TAG, "KWS features init: rate=%d bins=%d frame=%d shift=%d",
            sample_rate, num_bins, s_frame_length, s_frame_shift);
    return KC_OK;
}

int kws_features_compute(const float *wav, int wav_len,
                          float *output, int chunk_size)
{
    if (!s_fbank || !wav || !output) return 0;

    /* 拼接残余 + 新数据 */
    std::vector<float> combined;
    combined.reserve(s_remained.size() + wav_len);
    combined.insert(combined.end(), s_remained.begin(), s_remained.end());
    combined.insert(combined.end(), wav, wav + wav_len);

    /* 计算 Fbank 特征 */
    std::vector<std::vector<float>> feats;
    s_fbank->Compute(combined, &feats);

    /* 保存残余 */
    int frames_produced = (int)feats.size();
    int consumed = frames_produced * s_frame_shift;
    int remaining = (int)combined.size() - consumed;
    if (remaining > 0) {
        s_remained.assign(combined.end() - remaining, combined.end());
    } else {
        s_remained.clear();
    }

    /* 输出帧特征（最多 chunk_size 帧） */
    int out_frames = frames_produced < chunk_size ?
                     frames_produced : chunk_size;
    for (int i = 0; i < out_frames; i++) {
        for (int j = 0; j < s_num_bins; j++) {
            output[i * s_num_bins + j] = feats[i][j];
        }
    }

    return out_frames;
}

void kws_features_reset(void)
{
    s_remained.assign(320, 0.0f);
}

void kws_features_deinit(void)
{
    delete s_fbank;
    s_fbank = NULL;
    s_remained.clear();
    KC_LOGI(TAG, "KWS features deinitialized");
}

} /* extern "C" */
