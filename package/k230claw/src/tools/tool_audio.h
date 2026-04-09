#pragma once

#include "tool_registry.h"

/* record_audio: 录音 → 云端 Whisper STT → 文字 */
kc_tool_result_t *tool_record_audio_execute(const char *input_json);

/* speak_text: 文字 → 本地 KPU TTS → ALSA 播放 */
kc_tool_result_t *tool_speak_text_execute(const char *input_json);
