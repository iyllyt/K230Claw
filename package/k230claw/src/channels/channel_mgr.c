/*
 * channel_mgr.c - 通道管理器
 *
 * 管理通道注册、生命周期和消息路由。
 * 当前通道：CLI。未来：WebSocket, Telegram, QQ 等。
 */

#include "channel_mgr.h"
#include "../kc_config.h"

#include <string.h>

#define TAG "chan_mgr"
#define MAX_CHANNELS 8

static kc_channel_t *s_channels[MAX_CHANNELS];
static int s_channel_count = 0;

kc_err_t channel_mgr_init(void)
{
    s_channel_count = 0;
    memset(s_channels, 0, sizeof(s_channels));
    KC_LOGI(TAG, "channel manager initialized");
    return KC_OK;
}

kc_err_t channel_mgr_register(kc_channel_t *ch)
{
    if (!ch || !ch->name) return KC_ERR_INVALID;
    if (s_channel_count >= MAX_CHANNELS) {
        KC_LOGE(TAG, "too many channels (max %d)", MAX_CHANNELS);
        return KC_FAIL;
    }
    s_channels[s_channel_count++] = ch;
    KC_LOGI(TAG, "registered channel: %s", ch->name);
    return KC_OK;
}

kc_channel_t *channel_mgr_get(const char *name)
{
    if (!name) return NULL;
    for (int i = 0; i < s_channel_count; i++) {
        if (strcmp(s_channels[i]->name, name) == 0)
            return s_channels[i];
    }
    return NULL;
}

kc_err_t channel_mgr_start_all(void)
{
    for (int i = 0; i < s_channel_count; i++) {
        if (s_channels[i]->start) {
            kc_err_t err = s_channels[i]->start(s_channels[i]);
            if (err != KC_OK) {
                KC_LOGE(TAG, "failed to start channel: %s", s_channels[i]->name);
                return err;
            }
        }
    }
    KC_LOGI(TAG, "all %d channels started", s_channel_count);
    return KC_OK;
}

void channel_mgr_stop_all(void)
{
    KC_LOGI(TAG, "stopping all channels...");
    for (int i = 0; i < s_channel_count; i++) {
        if (s_channels[i]->stop) {
            s_channels[i]->stop(s_channels[i]);
        }
    }
    KC_LOGI(TAG, "all channels stopped");
}

kc_err_t channel_mgr_send(const char *channel_name, const char *chat_id, const char *text)
{
    /* "system" 通道：广播到所有已注册通道（传感器事件/cron 回复） */
    if (channel_name && strcmp(channel_name, KC_CHAN_SYSTEM) == 0) {
        kc_err_t last_err = KC_ERR_NOT_FOUND;
        for (int i = 0; i < s_channel_count; i++) {
            if (s_channels[i] && s_channels[i]->send_text)
                last_err = s_channels[i]->send_text(s_channels[i], chat_id, text);
        }
        return last_err;
    }

    kc_channel_t *ch = channel_mgr_get(channel_name);
    if (!ch) {
        KC_LOGW(TAG, "no channel '%s' registered", channel_name);
        return KC_ERR_NOT_FOUND;
    }
    if (!ch->send_text) {
        KC_LOGW(TAG, "channel '%s' does not support send_text", channel_name);
        return KC_ERR_INVALID;
    }
    return ch->send_text(ch, chat_id, text);
}

kc_err_t channel_mgr_send_image(const char *channel_name, const char *chat_id,
                                const char *b64, const char *mime, const char *caption)
{
    kc_channel_t *ch = channel_mgr_get(channel_name);
    if (!ch) return KC_ERR_NOT_FOUND;
    if (!ch->send_image) {
        KC_LOGW(TAG, "channel '%s' does not support send_image", channel_name);
        return KC_ERR_INVALID;
    }
    return ch->send_image(ch, chat_id, b64, mime, caption);
}
