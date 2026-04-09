/*
 * message_bus.c - pthread 消息总线
 * 替代 MimiClaw 的 FreeRTOS xQueueCreate/Send/Receive
 * 使用 pthread_mutex + pthread_cond + 环形缓冲区实现线程安全队列
 */

#include "message_bus.h"
#include "../kc_hal.h"
#include "../kc_config.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

#define TAG "bus"

/* ── 线程安全队列实现 ── */

typedef struct {
    kc_msg_t *buf;          /* 环形缓冲区 */
    int capacity;
    int head;               /* 下一个写入位置 */
    int tail;               /* 下一个读取位置 */
    int count;              /* 当前元素数 */
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} kc_queue_t;

static kc_queue_t *queue_create(int capacity) {
    kc_queue_t *q = (kc_queue_t *)calloc(1, sizeof(kc_queue_t));
    if (!q) return NULL;

    q->buf = (kc_msg_t *)calloc(capacity, sizeof(kc_msg_t));
    if (!q->buf) { free(q); return NULL; }

    q->capacity = capacity;
    q->head = q->tail = q->count = 0;
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
    return q;
}

/* 计算 timespec 绝对时间 */
static void ms_to_abstime(uint32_t timeout_ms, struct timespec *ts) {
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec  += timeout_ms / 1000;
    ts->tv_nsec += (timeout_ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec  += 1;
        ts->tv_nsec -= 1000000000L;
    }
}

static kc_err_t queue_push(kc_queue_t *q, const kc_msg_t *msg, uint32_t timeout_ms) {
    pthread_mutex_lock(&q->mutex);

    /* 等待队列有空间 */
    while (q->count >= q->capacity) {
        if (timeout_ms == 0) {
            pthread_mutex_unlock(&q->mutex);
            return KC_ERR_TIMEOUT;
        }
        struct timespec ts;
        ms_to_abstime(timeout_ms, &ts);
        if (pthread_cond_timedwait(&q->not_full, &q->mutex, &ts) != 0) {
            pthread_mutex_unlock(&q->mutex);
            return KC_ERR_TIMEOUT;
        }
    }

    /* 写入 */
    memcpy(&q->buf[q->head], msg, sizeof(kc_msg_t));
    q->head = (q->head + 1) % q->capacity;
    q->count++;

    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mutex);
    return KC_OK;
}

static kc_err_t queue_pop(kc_queue_t *q, kc_msg_t *msg, uint32_t timeout_ms) {
    pthread_mutex_lock(&q->mutex);

    /* 等待队列有数据 */
    while (q->count == 0) {
        if (kc_is_shutting_down()) {
            pthread_mutex_unlock(&q->mutex);
            return KC_ERR_TIMEOUT;
        }
        if (timeout_ms == UINT32_MAX) {
            /* 用 1 秒超时轮询 shutdown 标志，而非真正无限等待 */
            struct timespec ts;
            ms_to_abstime(1000, &ts);
            pthread_cond_timedwait(&q->not_empty, &q->mutex, &ts);
        } else {
            struct timespec ts;
            ms_to_abstime(timeout_ms, &ts);
            if (pthread_cond_timedwait(&q->not_empty, &q->mutex, &ts) != 0) {
                pthread_mutex_unlock(&q->mutex);
                return KC_ERR_TIMEOUT;
            }
        }
    }

    /* 读出 */
    memcpy(msg, &q->buf[q->tail], sizeof(kc_msg_t));
    q->tail = (q->tail + 1) % q->capacity;
    q->count--;

    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mutex);
    return KC_OK;
}

/* ── 全局总线实例 ── */

static kc_queue_t *s_inbound = NULL;
static kc_queue_t *s_outbound = NULL;

kc_err_t message_bus_init(void) {
    s_inbound  = queue_create(KC_BUS_QUEUE_LEN);
    s_outbound = queue_create(KC_BUS_QUEUE_LEN);

    if (!s_inbound || !s_outbound) {
        KC_LOGE(TAG, "failed to create message queues");
        return KC_ERR_NO_MEM;
    }

    KC_LOGI(TAG, "message bus initialized (depth %d)", KC_BUS_QUEUE_LEN);
    return KC_OK;
}

kc_err_t message_bus_push_inbound(const kc_msg_t *msg) {
    kc_err_t ret = queue_push(s_inbound, msg, 1000);
    if (ret != KC_OK) KC_LOGW(TAG, "inbound queue full, dropping");
    return ret;
}

kc_err_t message_bus_pop_inbound(kc_msg_t *msg, uint32_t timeout_ms) {
    return queue_pop(s_inbound, msg, timeout_ms);
}

kc_err_t message_bus_push_outbound(const kc_msg_t *msg) {
    kc_err_t ret = queue_push(s_outbound, msg, 1000);
    if (ret != KC_OK) KC_LOGW(TAG, "outbound queue full, dropping");
    return ret;
}

kc_err_t message_bus_pop_outbound(kc_msg_t *msg, uint32_t timeout_ms) {
    return queue_pop(s_outbound, msg, timeout_ms);
}
