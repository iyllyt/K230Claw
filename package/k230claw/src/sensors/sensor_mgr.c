/*
 * sensor_mgr.c - 后台传感器管理器
 *
 * 管理传感器注册、生命周期和事件推送。
 * 内置事件节流机制，防止高频事件淹没 agent loop。
 */

#include "sensor_mgr.h"
#include "../bus/message_bus.h"
#include "../kc_config.h"

#include <string.h>
#include <stdbool.h>
#include <time.h>
#include <pthread.h>

#define TAG "sensor_mgr"
#define MAX_SENSORS        8
#define MAX_EVENT_SLOTS   16
#define EVENT_MIN_INTERVAL 30  /* 同名事件最小间隔（秒） */

static kc_sensor_t *s_sensors[MAX_SENSORS];
static int s_sensor_count = 0;
static bool s_running[MAX_SENSORS];

/* ── 事件节流 ── */

typedef struct {
    char name[32];
    time_t last_push;
} event_throttle_t;

static event_throttle_t s_throttle[MAX_EVENT_SLOTS];
static int s_throttle_count = 0;
static pthread_mutex_t s_throttle_mutex = PTHREAD_MUTEX_INITIALIZER;

/* 返回 1 = 允许推送（已过节流间隔），0 = 节流中 */
static int throttle_check(const char *event_name)
{
    time_t now = time(NULL);
    pthread_mutex_lock(&s_throttle_mutex);

    /* 查找已有 slot */
    for (int i = 0; i < s_throttle_count; i++) {
        if (strcmp(s_throttle[i].name, event_name) == 0) {
            if (now - s_throttle[i].last_push < EVENT_MIN_INTERVAL) {
                pthread_mutex_unlock(&s_throttle_mutex);
                return 0; /* 节流中 */
            }
            s_throttle[i].last_push = now;
            pthread_mutex_unlock(&s_throttle_mutex);
            return 1;
        }
    }

    /* 新事件名，分配 slot */
    if (s_throttle_count < MAX_EVENT_SLOTS) {
        strncpy(s_throttle[s_throttle_count].name, event_name, 31);
        s_throttle[s_throttle_count].name[31] = '\0';
        s_throttle[s_throttle_count].last_push = now;
        s_throttle_count++;
    }

    pthread_mutex_unlock(&s_throttle_mutex);
    return 1;
}

/* ── 公共 API ── */

kc_err_t sensor_mgr_init(void)
{
    s_sensor_count = 0;
    s_throttle_count = 0;
    memset(s_sensors, 0, sizeof(s_sensors));
    memset(s_running, 0, sizeof(s_running));
    memset(s_throttle, 0, sizeof(s_throttle));
    KC_LOGI(TAG, "sensor manager initialized");
    return KC_OK;
}

kc_err_t sensor_mgr_register(kc_sensor_t *sensor)
{
    if (!sensor || !sensor->name) return KC_ERR_INVALID;
    if (s_sensor_count >= MAX_SENSORS) {
        KC_LOGE(TAG, "too many sensors (max %d)", MAX_SENSORS);
        return KC_FAIL;
    }
    s_sensors[s_sensor_count++] = sensor;
    KC_LOGI(TAG, "registered sensor: %s", sensor->name);
    return KC_OK;
}

kc_err_t sensor_mgr_start_all(void)
{
    for (int i = 0; i < s_sensor_count; i++) {
        if (s_running[i]) continue;
        if (s_sensors[i]->start) {
            kc_err_t err = s_sensors[i]->start(s_sensors[i]);
            if (err != KC_OK) {
                KC_LOGE(TAG, "failed to start sensor: %s", s_sensors[i]->name);
                return err;
            }
            s_running[i] = true;
        }
    }
    if (s_sensor_count > 0)
        KC_LOGI(TAG, "all %d sensors started", s_sensor_count);
    return KC_OK;
}

void sensor_mgr_stop_all(void)
{
    if (s_sensor_count == 0) return;
    KC_LOGI(TAG, "stopping all sensors...");
    for (int i = 0; i < s_sensor_count; i++) {
        if (!s_running[i]) continue;
        if (s_sensors[i]->stop) {
            s_sensors[i]->stop(s_sensors[i]);
            s_running[i] = false;
        }
    }
    KC_LOGI(TAG, "all sensors stopped");
}

kc_err_t sensor_mgr_start_by_name(const char *name)
{
    if (!name) return KC_ERR_INVALID;
    for (int i = 0; i < s_sensor_count; i++) {
        if (strcmp(s_sensors[i]->name, name) == 0) {
            if (s_running[i]) {
                KC_LOGI(TAG, "sensor '%s' already running", name);
                return KC_OK;
            }
            if (!s_sensors[i]->start) return KC_FAIL;
            kc_err_t err = s_sensors[i]->start(s_sensors[i]);
            if (err == KC_OK) {
                s_running[i] = true;
                KC_LOGI(TAG, "sensor '%s' started", name);
            } else {
                KC_LOGE(TAG, "failed to start sensor '%s'", name);
            }
            return err;
        }
    }
    KC_LOGW(TAG, "sensor not found: %s", name);
    return KC_ERR_NOT_FOUND;
}

kc_err_t sensor_mgr_stop_by_name(const char *name)
{
    if (!name) return KC_ERR_INVALID;
    for (int i = 0; i < s_sensor_count; i++) {
        if (strcmp(s_sensors[i]->name, name) == 0) {
            if (!s_running[i]) {
                KC_LOGI(TAG, "sensor '%s' already stopped", name);
                return KC_OK;
            }
            if (s_sensors[i]->stop) {
                s_sensors[i]->stop(s_sensors[i]);
                s_running[i] = false;
                KC_LOGI(TAG, "sensor '%s' stopped", name);
            }
            return KC_OK;
        }
    }
    KC_LOGW(TAG, "sensor not found: %s", name);
    return KC_ERR_NOT_FOUND;
}

int sensor_mgr_is_running(const char *name)
{
    if (!name) return -1;
    for (int i = 0; i < s_sensor_count; i++) {
        if (strcmp(s_sensors[i]->name, name) == 0)
            return s_running[i] ? 1 : 0;
    }
    return -1;
}

kc_err_t sensor_push_event(const char *event_name, const char *payload)
{
    if (!event_name || !payload) return KC_ERR_INVALID;

    /* 节流检查 */
    if (!throttle_check(event_name)) {
        KC_LOGD(TAG, "event '%s' throttled", event_name);
        return KC_OK;
    }

    /* 构造事件消息 */
    kc_msg_t msg = {0};
    strncpy(msg.channel, KC_CHAN_SYSTEM, sizeof(msg.channel) - 1);
    strncpy(msg.chat_id, "local", sizeof(msg.chat_id) - 1);
    msg.msg_type = KC_MSG_TYPE_EVENT;
    strncpy(msg.event_name, event_name, sizeof(msg.event_name) - 1);
    msg.content = strdup(payload);
    if (!msg.content) return KC_ERR_NO_MEM;

    kc_err_t err = message_bus_push_inbound(&msg);
    if (err != KC_OK) {
        KC_LOGW(TAG, "event '%s' push failed (queue full?)", event_name);
        free(msg.content);
    } else {
        KC_LOGI(TAG, "event '%s': %s", event_name, payload);
    }
    return err;
}
