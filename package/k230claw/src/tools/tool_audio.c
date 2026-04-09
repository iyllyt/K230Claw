/*
 * tool_audio.c - 音频工具
 *
 * record_audio: 录音 → WAV → 云端 OpenAI Whisper STT → 文字
 * speak_text:   文字 → 本地 KPU TTS → ALSA 播放
 */

#include "tool_audio.h"
#include "../kc_config.h"
#include "../kc_hal.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define TAG "tool_audio"

#ifdef KC_HAS_K230_HW

#include "../hal/hal_audio.h"
#include "../hal/tts/tts_engine.h"
#include "../third_party/cJSON/cJSON.h"
#include <curl/curl.h>

/* ── WAV 编码 ── */

static uint8_t *encode_wav(const int16_t *pcm, int frames,
                            int sample_rate, int channels,
                            size_t *wav_len)
{
    int data_size = frames * channels * 2; /* 16-bit */
    int file_size = 44 + data_size;
    uint8_t *wav = (uint8_t *)malloc(file_size);
    if (!wav) return NULL;

    /* RIFF header */
    memcpy(wav, "RIFF", 4);
    *(uint32_t *)(wav + 4) = file_size - 8;
    memcpy(wav + 8, "WAVE", 4);

    /* fmt chunk */
    memcpy(wav + 12, "fmt ", 4);
    *(uint32_t *)(wav + 16) = 16;                   /* chunk size */
    *(uint16_t *)(wav + 20) = 1;                     /* PCM format */
    *(uint16_t *)(wav + 22) = channels;
    *(uint32_t *)(wav + 24) = sample_rate;
    *(uint32_t *)(wav + 28) = sample_rate * channels * 2;  /* byte rate */
    *(uint16_t *)(wav + 32) = channels * 2;          /* block align */
    *(uint16_t *)(wav + 34) = 16;                    /* bits per sample */

    /* data chunk */
    memcpy(wav + 36, "data", 4);
    *(uint32_t *)(wav + 40) = data_size;
    memcpy(wav + 44, pcm, data_size);

    *wav_len = file_size;
    return wav;
}

/* ── curl 响应缓冲区 ── */

typedef struct {
    char *data;
    size_t len;
} curl_buf_t;

static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb,
                             void *userdata)
{
    curl_buf_t *buf = (curl_buf_t *)userdata;
    size_t total = size * nmemb;
    char *new_data = (char *)realloc(buf->data, buf->len + total + 1);
    if (!new_data) return 0;
    buf->data = new_data;
    memcpy(buf->data + buf->len, ptr, total);
    buf->len += total;
    buf->data[buf->len] = '\0';
    return total;
}

/* ── 云端 Whisper STT ── */

static char *cloud_stt(const uint8_t *wav, size_t wav_len)
{
    const char *api_key = kc_config_get_str("stt_api_key", "");
    if (!api_key[0])
        api_key = kc_config_get_str("openai_api_key", "");
    if (!api_key[0]) {
        KC_LOGE(TAG, "STT: no API key configured");
        return NULL;
    }

    const char *model = kc_config_get_str("stt_model", "whisper-1");
    const char *language = kc_config_get_str("stt_language", "zh");
    const char *url = kc_config_get_str("stt_api_url",
        "https://api.openai.com/v1/audio/transcriptions");
    const char *proxy = kc_config_get_str("proxy", "");

    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    /* Authorization header */
    char auth[256];
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", api_key);
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, auth);

    /* multipart form */
    curl_mime *mime = curl_mime_init(curl);

    curl_mimepart *part = curl_mime_addpart(mime);
    curl_mime_name(part, "file");
    curl_mime_data(part, (const char *)wav, wav_len);
    curl_mime_filename(part, "recording.wav");
    curl_mime_type(part, "audio/wav");

    part = curl_mime_addpart(mime);
    curl_mime_name(part, "model");
    curl_mime_data(part, model, CURL_ZERO_TERMINATED);

    part = curl_mime_addpart(mime);
    curl_mime_name(part, "language");
    curl_mime_data(part, language, CURL_ZERO_TERMINATED);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    if (proxy[0])
        curl_easy_setopt(curl, CURLOPT_PROXY, proxy);

    curl_buf_t resp = {NULL, 0};
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_mime_free(mime);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        KC_LOGE(TAG, "STT curl error: %s", curl_easy_strerror(res));
        free(resp.data);
        return NULL;
    }

    /* 解析响应 JSON: {"text": "..."} */
    if (!resp.data) return NULL;

    cJSON *json = cJSON_Parse(resp.data);
    free(resp.data);
    if (!json) return NULL;

    cJSON *text_item = cJSON_GetObjectItem(json, "text");
    char *result = NULL;
    if (text_item && cJSON_IsString(text_item))
        result = strdup(text_item->valuestring);

    cJSON_Delete(json);
    return result;
}

/* forward declare for voice_wake pause/resume */
void voice_wake_pause(void);
void voice_wake_resume(void);

/* weak symbols: 如果 sensor_voice.c 未链接，使用空实现 */
__attribute__((weak)) void voice_wake_pause(void) {}
__attribute__((weak)) void voice_wake_resume(void) {}

/* ── record_audio 工具 ── */

kc_tool_result_t *tool_record_audio_execute(const char *input_json)
{
    /* 解析参数 */
    int duration = 5;
    cJSON *input = cJSON_Parse(input_json);
    if (input) {
        cJSON *dur = cJSON_GetObjectItem(input, "duration");
        if (dur && cJSON_IsNumber(dur)) {
            duration = (int)dur->valuedouble;
            if (duration < 1) duration = 1;
            if (duration > 30) duration = 30;
        }
        cJSON_Delete(input);
    }

    KC_LOGI(TAG, "recording %d seconds...", duration);

    /* 暂停 KWS 传感器 */
    voice_wake_pause();

    /* 录音 */
    kc_audio_capture_t *cap = NULL;
    kc_err_t err = hal_audio_capture_open(&cap, 16000, 1);
    if (err != KC_OK) {
        voice_wake_resume();
        return tool_result_error("Failed to open microphone.");
    }

    int total_frames = 16000 * duration;
    int16_t *pcm = (int16_t *)malloc(total_frames * sizeof(int16_t));
    if (!pcm) {
        hal_audio_capture_close(cap);
        voice_wake_resume();
        return tool_result_error("Out of memory for recording.");
    }

    int read = hal_audio_capture_read(cap, pcm, total_frames);
    hal_audio_capture_close(cap);

    /* 恢复 KWS */
    voice_wake_resume();

    if (read <= 0) {
        free(pcm);
        return tool_result_error("Recording failed.");
    }

    KC_LOGI(TAG, "recorded %d frames, sending to STT...", read);

    /* 编码 WAV */
    size_t wav_len = 0;
    uint8_t *wav = encode_wav(pcm, read, 16000, 1, &wav_len);
    free(pcm);

    if (!wav) return tool_result_error("WAV encoding failed.");

    /* 发送到 Whisper API */
    char *text = cloud_stt(wav, wav_len);
    free(wav);

    if (!text) {
        return tool_result_error(
            "Speech-to-text failed. Check STT API key and network.");
    }

    KC_LOGI(TAG, "STT result: %s", text);

    /* 返回结果 */
    char result[2048];
    snprintf(result, sizeof(result),
             "User said (via voice): \"%s\"", text);
    kc_tool_result_t *r = tool_result_text(result);
    free(text);
    return r;
}

/* ── speak_text 工具 ── */

kc_tool_result_t *tool_speak_text_execute(const char *input_json)
{
    /* 解析参数 */
    cJSON *input = cJSON_Parse(input_json);
    if (!input)
        return tool_result_error("Invalid input JSON.");

    cJSON *text_item = cJSON_GetObjectItem(input, "text");
    if (!text_item || !cJSON_IsString(text_item)) {
        cJSON_Delete(input);
        return tool_result_error("Missing 'text' parameter.");
    }

    const char *text = text_item->valuestring;

    if (!kc_tts_available()) {
        cJSON_Delete(input);
        return tool_result_error(
            "TTS not available. Check kmodel files in tts_kmodel_dir.");
    }

    KC_LOGI(TAG, "speaking: %s", text);

    kc_err_t err = kc_tts_speak(text);
    cJSON_Delete(input);

    if (err != KC_OK)
        return tool_result_error("TTS synthesis or playback failed.");

    return tool_result_text("Spoke the text aloud to the user.");
}

#else /* !KC_HAS_K230_HW — x86 stub */

kc_tool_result_t *tool_record_audio_execute(const char *input_json)
{
    (void)input_json;
    return tool_result_error("Audio recording not available on this platform.");
}

kc_tool_result_t *tool_speak_text_execute(const char *input_json)
{
    (void)input_json;
    return tool_result_error("TTS not available on this platform.");
}

#endif /* KC_HAS_K230_HW */
