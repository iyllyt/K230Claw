#pragma once

/*
 * cron_service.h - 定时任务服务
 *
 * 功能:
 *   - 支持一次性（once）和循环（interval）两种定时任务
 *   - 到时间推事件消息到 inbound 队列，触发 agent 响应
 *   - 持久化到 JSON 文件，重启后恢复
 *   - 后台 pthread 每 10 秒检查一次
 */

#include "../kc_hal.h"
#include <time.h>
#include <stdbool.h>

#define CRON_MAX_JOBS  32

typedef struct {
    int    id;
    char   name[64];
    int    type;            /* 0=once（到时执行一次）, 1=interval（循环执行） */
    time_t trigger_time;    /* type=0: 绝对触发时间 */
    int    interval_sec;    /* type=1: 间隔秒数 */
    time_t next_fire;       /* 下次触发时间 */
    char   message[256];    /* 触发时注入 inbound 的消息内容 */
    char   channel[32];     /* 创建时的来源通道，触发时回复到此通道 */
    bool   enabled;
} kc_cron_job_t;

/* 初始化 cron 服务（加载持久化任务） */
kc_err_t cron_service_init(void);

/* 启动 cron 后台线程 */
kc_err_t cron_service_start(void);

/* 停止 cron 后台线程 */
void cron_service_stop(void);

/* 添加任务，返回任务 ID。channel 为回复目标通道（如 "cli"/"websocket"） */
int cron_add_job(const char *name, int type, time_t trigger_or_interval, const char *message, const char *channel);

/* 删除任务 */
kc_err_t cron_remove_job(int id);

/* 列出所有任务（返回 JSON 字符串，调用者 free） */
char *cron_list_jobs(void);

/* 保存任务到文件 */
kc_err_t cron_save(void);
