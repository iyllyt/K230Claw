#pragma once

/*
 * sensor_voice.h - 语音唤醒传感器
 *
 * 后台持续监听麦克风（ALSA），KPU KWS 检测唤醒词。
 * 检测到唤醒词后推 "wake_word" 事件。
 *
 * 与 record_audio 工具共享麦克风：
 *   record_audio 调用 voice_wake_pause() 暂停
 *   录完后调用 voice_wake_resume() 恢复
 */

#include "sensor_mgr.h"

/* 获取语音唤醒传感器实例 */
kc_sensor_t *voice_wake_get(void);

/* 暂停/恢复（供 record_audio 工具调用） */
void voice_wake_pause(void);
void voice_wake_resume(void);
