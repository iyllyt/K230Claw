#pragma once

/*
 * channel_mgr.h - 通道管理器
 *
 * 管理所有已注册通道的生命周期和消息路由。
 * Agent Loop 通过 channel_mgr_send* 发送 outbound 消息，
 * 管理器根据通道名称路由到对应通道的 send_text/send_image。
 */

#include "channel.h"

/* 初始化通道管理器 */
kc_err_t channel_mgr_init(void);

/* 注册一个通道（不 start，仅注册） */
kc_err_t channel_mgr_register(kc_channel_t *ch);

/* 按名称查找通道 */
kc_channel_t *channel_mgr_get(const char *name);

/* 启动所有已注册通道 */
kc_err_t channel_mgr_start_all(void);

/* 停止所有已注册通道 */
void channel_mgr_stop_all(void);

/* 发送文本消息到指定通道（按名称路由） */
kc_err_t channel_mgr_send(const char *channel_name, const char *chat_id, const char *text);

/* 发送图片到指定通道（通道不支持时返回 KC_ERR_INVALID） */
kc_err_t channel_mgr_send_image(const char *channel_name, const char *chat_id,
                                const char *b64, const char *mime, const char *caption);
