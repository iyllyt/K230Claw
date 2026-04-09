#pragma once

/*
 * ws_server.h - Web 聊天界面（mongoose 嵌入式 HTTP/WebSocket）
 *
 * 功能:
 *   - HTTP GET / → 内嵌 HTML 聊天页面
 *   - WebSocket /ws → 双向消息
 *   - 注册为 kc_channel_t，通过 channel_mgr 路由消息
 *
 * 依赖: third_party/mongoose/mongoose.c + mongoose.h
 *   下载: https://github.com/cesanta/mongoose (MIT License)
 *   只需 mongoose.c 和 mongoose.h 两个文件
 */

#include "../channels/channel.h"

/* 获取 WebSocket 通道实例 */
kc_channel_t *ws_channel_get(void);
