#pragma once

/*
 * serial_cli.h - stdin/stdout 交互式 REPL
 *
 * Phase 3: 作为 kc_channel_t 通道实现注册到通道管理器。
 * 输入线程：读取 stdin，推入 inbound 队列。
 * 输出：通过 send_text 回调写入 stdout。
 */

#include "../kc_hal.h"
#include "../channels/channel.h"

/* 获取 CLI 通道实例（用于注册到 channel_mgr） */
kc_channel_t *cli_channel_get(void);

/* 初始化 CLI（配置，不启动线程） */
kc_err_t serial_cli_init(void);

/* 启动 outbound 分发线程（从 outbound 队列读取，路由到各通道） */
kc_err_t outbound_dispatch_start(void);

/* 停止 CLI + outbound 线程 */
void serial_cli_stop(void);
