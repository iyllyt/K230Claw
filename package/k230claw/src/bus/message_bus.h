#pragma once

/*
 * message_bus.h - 线程安全消息总线
 *
 * Phase 3: 扩展消息类型，支持事件消息（传感器、cron 等）。
 * API 保持向后兼容，msg_type 默认 0 (CHAT)。
 */

#include <stdint.h>
#include "../kc_hal.h"

/* 消息类型 */
#define KC_MSG_TYPE_CHAT    0   /* 用户对话消息 */
#define KC_MSG_TYPE_EVENT   1   /* 系统事件（传感器、cron 等） */

/* 消息结构 */
typedef struct {
    char channel[16];       /* "cli", "telegram", "websocket", "system" */
    char chat_id[96];       /* 会话 ID */
    char *content;          /* 堆分配，所有权随消息转移，接收方负责 free */
    int  msg_type;          /* KC_MSG_TYPE_CHAT / KC_MSG_TYPE_EVENT */
    char event_name[32];    /* msg_type=EVENT 时的事件名，如 "person_detected" */
} kc_msg_t;

/* 初始化消息总线（inbound + outbound 两个队列） */
kc_err_t message_bus_init(void);

/* 推入 inbound 队列（发往 Agent Loop），超时 1 秒 */
kc_err_t message_bus_push_inbound(const kc_msg_t *msg);

/* 从 inbound 队列弹出（阻塞），timeout_ms=UINT32_MAX 表示无限等待 */
kc_err_t message_bus_pop_inbound(kc_msg_t *msg, uint32_t timeout_ms);

/* 推入 outbound 队列（发往通道），超时 1 秒 */
kc_err_t message_bus_push_outbound(const kc_msg_t *msg);

/* 从 outbound 队列弹出（阻塞） */
kc_err_t message_bus_pop_outbound(kc_msg_t *msg, uint32_t timeout_ms);
