/*
 * tts_engine.cpp - 本地 KPU TTS 引擎
 *
 * 整合 tts_zh 的完整管道：
 *   1. 中文文本 → 音素序列（zh_frontend + pypinyin）
 *   2. 音素 → 整数 ID，分块（每块 <=50，padding 值 357）
 *   3. FastSpeech1: speaker_id + phone_ids → encoder_output + durations
 *   4. Length Regulator: 按 duration 重复 encoder 向量，pad 到 600
 *   5. FastSpeech2: regulated → mel spectrogram (1,80,600)
 *   6. HiFiGan: mel 分块 (1,80,100) → 音频波形 (1,1,25600)
 *   7. float → int16 PCM (24kHz mono)
 *
 * 通过 kpu_wrapper C API 调用模型推理（不直接使用 nncase C++）。
 */

#include "tts_engine.h"
#include "../kpu_wrapper.h"
#include "../hal_audio.h"

#include "include/zh_frontend.h"
#include "include/jieba_utils.h"
#include "include/text_normalization.h"
#include "include/length_regulator.h"

#include <map>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <regex>

#define TAG "tts"

/* ── 常量（匹配 tts_zh/tts_src/main.cc） ── */
#define TTS_N           50      /* 每块最大音素数 */
#define TTS_M           100     /* HiFiGan mel 块大小 */
#define TTS_C           80      /* mel 频率 bin 数 */
#define TTS_DIM         256     /* encoder embedding 维度 */
#define TTS_L           600     /* 最大 duration 扩展长度 */
#define TTS_PAD_ID      357     /* padding 音素 ID ("sp") */
#define TTS_SAMPLE_RATE 24000   /* 输出采样率 */

/* ── 全局状态 ── */
static kpu_model_t *s_fs1 = NULL;   /* FastSpeech1 */
static kpu_model_t *s_fs2 = NULL;   /* FastSpeech2 */
static kpu_model_t *s_hfg = NULL;   /* HiFiGan */
static std::map<std::string, int> s_symbol_to_id;
static zh_frontend s_frontend;
static bool s_initialized = false;

extern "C" {

kc_err_t kc_tts_init(const char *dict_dir, const char *kmodel_dir)
{
    if (s_initialized) return KC_OK;
    if (!dict_dir || !kmodel_dir) return KC_ERR_INVALID;

    /* 1. 初始化拼音字典 */
    std::string pinyin_path = std::string(dict_dir) + "/pinyin_txt/pinyin.txt";
    std::string phrase_path = std::string(dict_dir) + "/pinyin_txt/small_pinyin.txt";
    pypinyin.Init(pinyin_path, phrase_path);
    KC_LOGI(TAG, "pinyin dict loaded");

    /* 2. 加载 phone_id_map */
    std::string map_path = std::string(dict_dir) + "/phone_id_map_en.txt";
    std::ifstream file(map_path);
    if (!file.is_open()) {
        KC_LOGE(TAG, "cannot open phone_id_map: %s", map_path.c_str());
        return KC_ERR_NOT_FOUND;
    }
    std::string line;
    while (std::getline(file, line)) {
        std::istringstream iss(line);
        std::string sym;
        int id;
        if (iss >> sym >> id)
            s_symbol_to_id[sym] = id;
    }
    file.close();
    KC_LOGI(TAG, "phone_id_map loaded: %d entries", (int)s_symbol_to_id.size());

    /* 3. 加载 3 个 kmodel */
    char path[512];

    snprintf(path, sizeof(path), "%s/zh_fastspeech_1.kmodel", kmodel_dir);
    if (kpu_model_load(path, &s_fs1) != KC_OK) {
        KC_LOGE(TAG, "failed to load FastSpeech1");
        return KC_FAIL;
    }

    snprintf(path, sizeof(path), "%s/zh_fastspeech_2.kmodel", kmodel_dir);
    if (kpu_model_load(path, &s_fs2) != KC_OK) {
        KC_LOGE(TAG, "failed to load FastSpeech2");
        kpu_model_free(s_fs1); s_fs1 = NULL;
        return KC_FAIL;
    }

    snprintf(path, sizeof(path), "%s/hifigan.kmodel", kmodel_dir);
    if (kpu_model_load(path, &s_hfg) != KC_OK) {
        KC_LOGE(TAG, "failed to load HiFiGan");
        kpu_model_free(s_fs1); s_fs1 = NULL;
        kpu_model_free(s_fs2); s_fs2 = NULL;
        return KC_FAIL;
    }

    s_initialized = true;
    KC_LOGI(TAG, "TTS engine initialized");
    return KC_OK;
}

int kc_tts_available(void) { return s_initialized ? 1 : 0; }

/* ── 文本 → 音素 ID 序列 ── */

static std::vector<int> text_to_phoneme_ids(const std::string &text)
{
    /* 逗号处理（原版 main.cc 的正则） */
    std::string cleaned = std::regex_replace(
        text, std::regex("(\\d),(\\d)"), "$1$2");

    /* 文本 → 音素 */
    std::vector<std::vector<std::string>> pinyin =
        s_frontend.get_phonemes(cleaned, false, true, false, false);

    /* 展平 */
    std::vector<std::string> phonemes;
    for (auto &group : pinyin)
        for (auto &p : group)
            phonemes.push_back(p);

    /* 音素 → ID */
    std::vector<int> ids;
    for (auto &p : phonemes) {
        if (p == "_" || p == "~") continue;
        auto it = s_symbol_to_id.find(p);
        if (it != s_symbol_to_id.end()) {
            ids.push_back(it->second);
        } else {
            ids.push_back(TTS_PAD_ID); /* 未知 → sp */
        }
    }
    return ids;
}

/* ── 分块（每块 <=50，padding 357） ── */

static void chunk_phoneme_ids(const std::vector<int> &ids,
                               std::vector<std::vector<int>> &chunks,
                               std::vector<int> &valid_lengths)
{
    chunks.clear();
    valid_lengths.clear();

    std::vector<int> current;
    for (size_t i = 0; i < ids.size(); i++) {
        current.push_back(ids[i]);
        if ((int)current.size() >= TTS_N) {
            valid_lengths.push_back((int)current.size());
            while ((int)current.size() < TTS_N)
                current.push_back(TTS_PAD_ID);
            chunks.push_back(current);
            current.clear();
        }
    }
    if (!current.empty()) {
        valid_lengths.push_back((int)current.size());
        while ((int)current.size() < TTS_N)
            current.push_back(TTS_PAD_ID);
        chunks.push_back(current);
    }

    if (chunks.empty()) {
        /* 空输入：推一个静音块 */
        std::vector<int> silence(TTS_N, TTS_PAD_ID);
        chunks.push_back(silence);
        valid_lengths.push_back(1);
    }
}

kc_err_t kc_tts_synthesize(const char *text,
                            int16_t **pcm_out, int *total_frames,
                            int *sample_rate)
{
    if (!s_initialized) return KC_FAIL;
    if (!text || !pcm_out || !total_frames || !sample_rate)
        return KC_ERR_INVALID;

    std::string input_text(text);
    if (input_text.empty()) return KC_ERR_INVALID;

    /* 1. 文本 → 音素 ID */
    std::vector<int> ids = text_to_phoneme_ids(input_text);
    KC_LOGI(TAG, "phoneme IDs: %d", (int)ids.size());

    /* 2. 分块 */
    std::vector<std::vector<int>> chunks;
    std::vector<int> valid_lengths;
    chunk_phoneme_ids(ids, chunks, valid_lengths);
    KC_LOGI(TAG, "chunks: %d", (int)chunks.size());

    /* 3. 逐块推理 */
    std::vector<float> all_audio;

    for (size_t k = 0; k < chunks.size(); k++) {
        int valid_n = valid_lengths[k];

        /* 3a. FastSpeech1 */
        int speaker_id = 0;
        kpu_model_set_input(s_fs1, 0, &speaker_id, sizeof(int));
        kpu_model_set_input(s_fs1, 1, chunks[k].data(),
                            TTS_N * sizeof(int));

        if (kpu_model_run(s_fs1) != KC_OK) {
            KC_LOGE(TAG, "FastSpeech1 run failed");
            return KC_FAIL;
        }

        /* 输出: encoder_output (1,50,256), durations (1,50) */
        float *enc_out = kpu_model_output_data(s_fs1, 0);
        float *dur_out = kpu_model_output_data(s_fs1, 1);
        if (!enc_out || !dur_out) {
            KC_LOGE(TAG, "FastSpeech1 output null");
            return KC_FAIL;
        }

        /* 截断到有效长度 */
        std::vector<float> encoder_buf(enc_out, enc_out + valid_n * TTS_DIM);
        std::vector<float> durations_buf(dur_out, dur_out + valid_n);

        /* 3b. Length Regulator (CPU) */
        int m_pad = 0;
        length_outputs lr_out = length_regulator(
            encoder_buf, durations_buf, valid_n, m_pad, TTS_L, TTS_DIM);

        /* 3c. FastSpeech2 */
        kpu_model_set_input(s_fs2, 0,
                            lr_out.repeat_encoder_hidden_states.data(),
                            TTS_L * TTS_DIM * sizeof(float));

        if (kpu_model_run(s_fs2) != KC_OK) {
            KC_LOGE(TAG, "FastSpeech2 run failed");
            return KC_FAIL;
        }

        /* 输出: mel spectrogram (1,80,600) */
        float *mel_raw = kpu_model_output_data(s_fs2, 0);
        if (!mel_raw) {
            KC_LOGE(TAG, "FastSpeech2 output null");
            return KC_FAIL;
        }

        /* 去除 padding 部分 */
        int actual_time = TTS_L - lr_out.M_pad;
        if (actual_time <= 0) actual_time = 1;

        /* 重排列: 从 (80, 600) 取 (80, actual_time) */
        std::vector<float> mel_valid;
        mel_valid.reserve(TTS_C * actual_time);
        for (int c = 0; c < TTS_C; c++) {
            for (int t = 0; t < actual_time; t++) {
                mel_valid.push_back(mel_raw[c * TTS_L + t]);
            }
        }

        /* 3d. 分块给 HiFiGan (每块 80x100) */
        int n_chunks = actual_time / TTS_M;
        int remaining = actual_time % TTS_M;
        int total_chunks = n_chunks + (remaining > 0 ? 1 : 0);

        /* 如果有余数，pad mel_valid 到 total_chunks * TTS_M */
        if (remaining > 0) {
            int padded_time = total_chunks * TTS_M;
            std::vector<float> mel_padded(TTS_C * padded_time, 0.0f);
            for (int c = 0; c < TTS_C; c++) {
                for (int t = 0; t < actual_time; t++) {
                    mel_padded[c * padded_time + t] = mel_valid[c * actual_time + t];
                }
            }
            mel_valid = mel_padded;
            actual_time = padded_time;
        }

        /* 生成 HiFiGan 输入块 */
        std::vector<float> chunk_audio;
        for (int ci = 0; ci < total_chunks; ci++) {
            /* 提取 (80, 100) 块 */
            std::vector<float> mel_chunk(TTS_C * TTS_M, 0.0f);
            for (int c = 0; c < TTS_C; c++) {
                for (int t = 0; t < TTS_M; t++) {
                    mel_chunk[c * TTS_M + t] =
                        mel_valid[c * actual_time + ci * TTS_M + t];
                }
            }

            kpu_model_set_input(s_hfg, 0, mel_chunk.data(),
                                TTS_C * TTS_M * sizeof(float));

            if (kpu_model_run(s_hfg) != KC_OK) {
                KC_LOGE(TAG, "HiFiGan run failed");
                return KC_FAIL;
            }

            /* 输出: (1,1,25600) 音频样本 */
            float *audio = kpu_model_output_data(s_hfg, 0);
            int audio_len = kpu_model_output_size(s_hfg, 0);
            if (!audio) {
                KC_LOGE(TAG, "HiFiGan output null");
                return KC_FAIL;
            }
            chunk_audio.insert(chunk_audio.end(), audio, audio + audio_len);
        }

        /* 截断 padding 产生的多余音频 */
        if (remaining > 0) {
            int valid_mel_frames = n_chunks * TTS_M + remaining;
            int valid_audio_samples = valid_mel_frames * 256;
            if ((int)chunk_audio.size() > valid_audio_samples)
                chunk_audio.resize(valid_audio_samples);
        }

        all_audio.insert(all_audio.end(),
                         chunk_audio.begin(), chunk_audio.end());
    }

    /* 4. float → int16 PCM */
    int num_samples = (int)all_audio.size();
    int16_t *pcm = (int16_t *)malloc(num_samples * sizeof(int16_t));
    if (!pcm) return KC_ERR_NO_MEM;

    for (int i = 0; i < num_samples; i++) {
        float s = all_audio[i];
        if (s > 1.0f) s = 1.0f;
        if (s < -1.0f) s = -1.0f;
        pcm[i] = (int16_t)(s * 32767.0f);
    }

    *pcm_out = pcm;
    *total_frames = num_samples;
    *sample_rate = TTS_SAMPLE_RATE;

    KC_LOGI(TAG, "synthesized %d samples (%.1fs @ %dHz)",
            num_samples, (float)num_samples / TTS_SAMPLE_RATE,
            TTS_SAMPLE_RATE);
    return KC_OK;
}

kc_err_t kc_tts_speak(const char *text)
{
    int16_t *pcm = NULL;
    int frames = 0, rate = 0;

    kc_err_t err = kc_tts_synthesize(text, &pcm, &frames, &rate);
    if (err != KC_OK) return err;

    err = hal_audio_play_buffer(pcm, frames, (unsigned int)rate, 1);
    free(pcm);
    return err;
}

void kc_tts_deinit(void)
{
    if (!s_initialized) return;
    kpu_model_free(s_fs1); s_fs1 = NULL;
    kpu_model_free(s_fs2); s_fs2 = NULL;
    kpu_model_free(s_hfg); s_hfg = NULL;
    s_symbol_to_id.clear();
    s_initialized = false;
    KC_LOGI(TAG, "TTS engine deinitialized");
}

} /* extern "C" */
