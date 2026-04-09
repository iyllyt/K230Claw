#pragma once

/*
 * channel.h - 通道抽象接口
 *
 * 参考 PicoClaw Channel 接口，用 C 函数指针实现。
 * 每种通道（CLI, WebSocket, Telegram 等）实现此接口。
 *
 * 设计要点:
 *   - 通道负责 接收输入 和 发送输出
 *   - 接收端：通道 pthread 读取输入，推入 inbound 队列（已有）
 *   - 发送端：通道管理器根据 msg.channel 名称路由到对应通道的 send_text/send_image
 *   - 生命周期由通道管理器统一管理（start_all / stop_all）
 */

#include "../kc_hal.h"

typedef struct kc_channel {
    const char *name;   /* 通道名："cli", "websocket", "telegram" 等 */

    /* 生命周期 */
    kc_err_t (*start)(struct kc_channel *self);
    kc_err_t (*stop)(struct kc_channel *self);

    /* 发送消息到此通道 */
    kc_err_t (*send_text)(struct kc_channel *self, const char *chat_id, const char *text);

    /* 发送图片到此通道（NULL = 不支持，如 CLI 串口） */
    kc_err_t (*send_image)(struct kc_channel *self, const char *chat_id,
                           const char *b64, const char *mime, const char *caption);

    /* 通道私有数据（各通道自行定义） */
    void *ctx;
} kc_channel_t;
