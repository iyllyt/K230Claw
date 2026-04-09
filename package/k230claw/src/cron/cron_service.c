/*
 * cron_service.c - 定时任务服务
 *
 * 后台 pthread 每 10 秒检查，到时推事件消息到 inbound 队列。
 * 持久化到 data_dir/cron_jobs.json。
 */

#include "cron_service.h"
#include "../kc_config.h"
#include "../bus/message_bus.h"
#include "../third_party/cJSON/cJSON.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>

#define TAG "cron"
#define CRON_CHECK_INTERVAL  10  /* 检查间隔（秒） */

static kc_cron_job_t s_jobs[CRON_MAX_JOBS];
static int s_job_count = 0;
static int s_next_id = 1;
static pthread_mutex_t s_cron_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t s_cron_tid;

/* ── 持久化 ── */

static void cron_file_path(char *buf, size_t size)
{
    snprintf(buf, size, "%s/cron_jobs.json", kc_get_data_dir());
}

kc_err_t cron_save(void)
{
    char path[256];
    cron_file_path(path, sizeof(path));

    pthread_mutex_lock(&s_cron_mutex);

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < s_job_count; i++) {
        kc_cron_job_t *j = &s_jobs[i];
        if (!j->enabled) continue;
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(obj, "id", j->id);
        cJSON_AddStringToObject(obj, "name", j->name);
        cJSON_AddNumberToObject(obj, "type", j->type);
        cJSON_AddNumberToObject(obj, "trigger_time", (double)j->trigger_time);
        cJSON_AddNumberToObject(obj, "interval_sec", j->interval_sec);
        cJSON_AddNumberToObject(obj, "next_fire", (double)j->next_fire);
        cJSON_AddStringToObject(obj, "message", j->message);
        cJSON_AddStringToObject(obj, "channel", j->channel);
        cJSON_AddItemToArray(arr, obj);
    }

    pthread_mutex_unlock(&s_cron_mutex);

    char *json = cJSON_Print(arr);
    cJSON_Delete(arr);
    if (!json) return KC_ERR_NO_MEM;

    FILE *f = fopen(path, "w");
    if (!f) { free(json); return KC_FAIL; }
    fputs(json, f);
    fclose(f);
    free(json);
    return KC_OK;
}

static void cron_load(void)
{
    char path[256];
    cron_file_path(path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) return;

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > 32768) { fclose(f); return; }

    char *buf = malloc(len + 1);
    if (!buf) { fclose(f); return; }
    size_t rd = fread(buf, 1, len, f);
    buf[rd] = '\0';
    fclose(f);

    cJSON *arr = cJSON_Parse(buf);
    free(buf);
    if (!arr || !cJSON_IsArray(arr)) { cJSON_Delete(arr); return; }

    cJSON *item;
    cJSON_ArrayForEach(item, arr) {
        if (s_job_count >= CRON_MAX_JOBS) break;
        kc_cron_job_t *j = &s_jobs[s_job_count];
        memset(j, 0, sizeof(*j));

        cJSON *id = cJSON_GetObjectItem(item, "id");
        cJSON *name = cJSON_GetObjectItem(item, "name");
        cJSON *type = cJSON_GetObjectItem(item, "type");
        cJSON *trigger = cJSON_GetObjectItem(item, "trigger_time");
        cJSON *interval = cJSON_GetObjectItem(item, "interval_sec");
        cJSON *next = cJSON_GetObjectItem(item, "next_fire");
        cJSON *msg = cJSON_GetObjectItem(item, "message");
        cJSON *ch = cJSON_GetObjectItem(item, "channel");

        j->id = id ? (int)id->valuedouble : s_next_id++;
        if (name) strncpy(j->name, name->valuestring, sizeof(j->name) - 1);
        j->type = type ? (int)type->valuedouble : 0;
        j->trigger_time = trigger ? (time_t)trigger->valuedouble : 0;
        j->interval_sec = interval ? (int)interval->valuedouble : 0;
        j->next_fire = next ? (time_t)next->valuedouble : 0;
        if (msg) strncpy(j->message, msg->valuestring, sizeof(j->message) - 1);
        if (ch && cJSON_IsString(ch))
            strncpy(j->channel, ch->valuestring, sizeof(j->channel) - 1);
        else
            strncpy(j->channel, KC_CHAN_CLI, sizeof(j->channel) - 1);
        j->enabled = true;

        if (j->id >= s_next_id) s_next_id = j->id + 1;
        s_job_count++;
    }
    cJSON_Delete(arr);
    KC_LOGI(TAG, "loaded %d cron jobs from %s", s_job_count, path);
}

/* ── 公共 API ── */

int cron_add_job(const char *name, int type, time_t trigger_or_interval, const char *message, const char *channel)
{
    pthread_mutex_lock(&s_cron_mutex);

    if (s_job_count >= CRON_MAX_JOBS) {
        pthread_mutex_unlock(&s_cron_mutex);
        return -1;
    }

    kc_cron_job_t *j = &s_jobs[s_job_count];
    memset(j, 0, sizeof(*j));
    j->id = s_next_id++;
    if (name) strncpy(j->name, name, sizeof(j->name) - 1);
    j->type = type;
    j->enabled = true;
    if (message) strncpy(j->message, message, sizeof(j->message) - 1);
    strncpy(j->channel, (channel && channel[0]) ? channel : KC_CHAN_CLI, sizeof(j->channel) - 1);

    if (type == 0) {
        /* once: trigger_or_interval 是绝对时间 */
        j->trigger_time = trigger_or_interval;
        j->next_fire = trigger_or_interval;
    } else {
        /* interval: trigger_or_interval 是间隔秒数 */
        j->interval_sec = (int)trigger_or_interval;
        j->next_fire = time(NULL) + j->interval_sec;
    }

    int id = j->id;
    s_job_count++;
    pthread_mutex_unlock(&s_cron_mutex);

    cron_save();
    KC_LOGI(TAG, "added cron job #%d '%s' (type=%d)", id, name ? name : "", type);
    return id;
}

kc_err_t cron_remove_job(int id)
{
    pthread_mutex_lock(&s_cron_mutex);

    for (int i = 0; i < s_job_count; i++) {
        if (s_jobs[i].id == id) {
            s_jobs[i].enabled = false;
            /* 压缩数组 */
            for (int k = i; k < s_job_count - 1; k++)
                s_jobs[k] = s_jobs[k + 1];
            s_job_count--;
            pthread_mutex_unlock(&s_cron_mutex);
            cron_save();
            KC_LOGI(TAG, "removed cron job #%d", id);
            return KC_OK;
        }
    }

    pthread_mutex_unlock(&s_cron_mutex);
    return KC_ERR_NOT_FOUND;
}

char *cron_list_jobs(void)
{
    pthread_mutex_lock(&s_cron_mutex);

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < s_job_count; i++) {
        kc_cron_job_t *j = &s_jobs[i];
        if (!j->enabled) continue;
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(obj, "id", j->id);
        cJSON_AddStringToObject(obj, "name", j->name);
        cJSON_AddStringToObject(obj, "type", j->type == 0 ? "once" : "interval");
        if (j->type == 1)
            cJSON_AddNumberToObject(obj, "interval_sec", j->interval_sec);

        /* 格式化 next_fire 为人类可读时间 */
        char time_buf[32];
        struct tm tm;
        localtime_r(&j->next_fire, &tm);
        strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm);
        cJSON_AddStringToObject(obj, "next_fire", time_buf);
        cJSON_AddStringToObject(obj, "message", j->message);
        cJSON_AddItemToArray(arr, obj);
    }

    pthread_mutex_unlock(&s_cron_mutex);

    char *json = cJSON_Print(arr);
    cJSON_Delete(arr);
    return json;
}

/* ── 后台线程 ── */

static void *cron_thread(void *arg)
{
    (void)arg;
    KC_LOGI(TAG, "cron service started");

    while (!kc_is_shutting_down()) {
        /* 分段 sleep，每 1 秒检查关停标志，避免关停延迟 */
        for (int s = 0; s < CRON_CHECK_INTERVAL && !kc_is_shutting_down(); s++)
            sleep(1);
        if (kc_is_shutting_down()) break;

        time_t now = time(NULL);
        bool need_save = false;

        pthread_mutex_lock(&s_cron_mutex);

        for (int i = 0; i < s_job_count; i++) {
            kc_cron_job_t *j = &s_jobs[i];
            if (!j->enabled || now < j->next_fire) continue;

            /* 触发！构造 inbound 消息，回复到创建时的通道 */
            kc_msg_t msg = {0};
            strncpy(msg.channel, j->channel[0] ? j->channel : KC_CHAN_CLI, sizeof(msg.channel) - 1);
            strncpy(msg.chat_id, "local", sizeof(msg.chat_id) - 1);
            msg.msg_type = KC_MSG_TYPE_EVENT;
            strncpy(msg.event_name, "cron", sizeof(msg.event_name) - 1);

            char content[384];
            snprintf(content, sizeof(content),
                     "[Scheduled Task: %s] %s",
                     j->name[0] ? j->name : "unnamed",
                     j->message);
            msg.content = strdup(content);

            KC_LOGI(TAG, "firing cron job #%d '%s'", j->id, j->name);

            if (msg.content) {
                /* 解锁后推消息（避免持锁调用 push） */
                pthread_mutex_unlock(&s_cron_mutex);
                if (message_bus_push_inbound(&msg) != KC_OK)
                    free(msg.content);
                pthread_mutex_lock(&s_cron_mutex);
            }

            if (j->type == 0) {
                /* once: 触发后禁用 */
                j->enabled = false;
                need_save = true;
            } else {
                /* interval: 设置下次触发 */
                j->next_fire = now + j->interval_sec;
                need_save = true;
            }
        }

        /* 清理已禁用的 once 任务 */
        int new_count = 0;
        for (int i = 0; i < s_job_count; i++) {
            if (s_jobs[i].enabled) {
                if (new_count != i)
                    s_jobs[new_count] = s_jobs[i];
                new_count++;
            }
        }
        s_job_count = new_count;

        pthread_mutex_unlock(&s_cron_mutex);

        if (need_save) cron_save();
    }

    KC_LOGI(TAG, "cron service stopped");
    return NULL;
}

kc_err_t cron_service_init(void)
{
    s_job_count = 0;
    s_next_id = 1;
    cron_load();
    KC_LOGI(TAG, "cron service initialized (%d jobs)", s_job_count);
    return KC_OK;
}

kc_err_t cron_service_start(void)
{
    if (pthread_create(&s_cron_tid, NULL, cron_thread, NULL) != 0) {
        KC_LOGE(TAG, "failed to create cron thread");
        return KC_FAIL;
    }
    return KC_OK;
}

void cron_service_stop(void)
{
    KC_LOGI(TAG, "stopping cron service...");
    pthread_join(s_cron_tid, NULL);
    cron_save();
}
