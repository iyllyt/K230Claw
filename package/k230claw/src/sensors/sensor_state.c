/*
 * sensor_state.c - 传感器持续状态存储
 *
 * pthread_rwlock 保护的 key-value 表。
 * 读操作用读锁（并发读），写操作用写锁。
 */

#include "sensor_state.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>

#define TAG "sensor_state"
#define MAX_STATE_ENTRIES 32
#define MAX_KEY_LEN       32
#define MAX_VALUE_LEN    128

typedef struct {
    char key[MAX_KEY_LEN];
    char value[MAX_VALUE_LEN];
} state_entry_t;

static state_entry_t s_entries[MAX_STATE_ENTRIES];
static int s_entry_count = 0;
static pthread_rwlock_t s_lock = PTHREAD_RWLOCK_INITIALIZER;

kc_err_t sensor_state_init(void)
{
    s_entry_count = 0;
    memset(s_entries, 0, sizeof(s_entries));
    KC_LOGI(TAG, "sensor state initialized");
    return KC_OK;
}

kc_err_t sensor_state_set(const char *key, const char *value)
{
    if (!key || !value) return KC_ERR_INVALID;

    pthread_rwlock_wrlock(&s_lock);

    /* 查找已有 key */
    for (int i = 0; i < s_entry_count; i++) {
        if (strcmp(s_entries[i].key, key) == 0) {
            strncpy(s_entries[i].value, value, MAX_VALUE_LEN - 1);
            s_entries[i].value[MAX_VALUE_LEN - 1] = '\0';
            pthread_rwlock_unlock(&s_lock);
            return KC_OK;
        }
    }

    /* 新 key */
    if (s_entry_count >= MAX_STATE_ENTRIES) {
        pthread_rwlock_unlock(&s_lock);
        KC_LOGW(TAG, "state table full (max %d)", MAX_STATE_ENTRIES);
        return KC_FAIL;
    }

    strncpy(s_entries[s_entry_count].key, key, MAX_KEY_LEN - 1);
    s_entries[s_entry_count].key[MAX_KEY_LEN - 1] = '\0';
    strncpy(s_entries[s_entry_count].value, value, MAX_VALUE_LEN - 1);
    s_entries[s_entry_count].value[MAX_VALUE_LEN - 1] = '\0';
    s_entry_count++;

    pthread_rwlock_unlock(&s_lock);
    return KC_OK;
}

const char *sensor_state_get(const char *key, const char *default_val)
{
    if (!key) return default_val;

    pthread_rwlock_rdlock(&s_lock);

    for (int i = 0; i < s_entry_count; i++) {
        if (strcmp(s_entries[i].key, key) == 0) {
            /* 返回内部缓冲区指针（读锁保护期间有效） */
            const char *val = s_entries[i].value;
            pthread_rwlock_unlock(&s_lock);
            return val;
        }
    }

    pthread_rwlock_unlock(&s_lock);
    return default_val;
}

char *sensor_state_summary(void)
{
    pthread_rwlock_rdlock(&s_lock);

    if (s_entry_count == 0) {
        pthread_rwlock_unlock(&s_lock);
        return NULL;
    }

    /* 估算所需空间 */
    size_t needed = 64; /* "## Sensor State\n" + 余量 */
    for (int i = 0; i < s_entry_count; i++)
        needed += strlen(s_entries[i].key) + strlen(s_entries[i].value) + 8;

    char *buf = malloc(needed);
    if (!buf) { pthread_rwlock_unlock(&s_lock); return NULL; }

    size_t off = 0;
    off += snprintf(buf + off, needed - off, "## Sensor State\n");
    for (int i = 0; i < s_entry_count; i++) {
        off += snprintf(buf + off, needed - off, "- %s: %s\n",
                        s_entries[i].key, s_entries[i].value);
    }

    pthread_rwlock_unlock(&s_lock);
    return buf;
}
