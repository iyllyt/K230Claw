/*
 * kc_config.c - JSON 配置文件读写
 * 替代 ESP-IDF 的 NVS (非易失性存储)
 *
 * 配置文件格式 (/etc/k230claw/k230claw.conf):
 * {
 *   "provider": "openai",
 *   "model": "gpt-4o",
 *   "openai_api_key": "sk-xxx",
 *   "openai_api_url": "https://api.openai.com/v1/chat/completions",
 *   "anthropic_api_key": "",
 *   "anthropic_api_url": "https://api.anthropic.com/v1/messages",
 *   "proxy": "http://127.0.0.1:7890",
 *   "timezone": "Asia/Shanghai"
 * }
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "../kc_hal.h"
#include "../kc_config.h"
#include "../third_party/cJSON/cJSON.h"

#define TAG "config"
#define MAX_CONFIG_SIZE (8 * 1024)  /* 配置文件最大 8KB */

/* 全局配置对象 */
static cJSON *s_config = NULL;
static char s_config_path[256] = "";
static pthread_mutex_t s_config_mutex = PTHREAD_MUTEX_INITIALIZER;

/* 从文件读取全部内容 */
static char *read_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (len <= 0 || len > MAX_CONFIG_SIZE) {
        fclose(f);
        return NULL;
    }

    char *buf = (char *)malloc(len + 1);
    if (!buf) { fclose(f); return NULL; }

    size_t rd = fread(buf, 1, len, f);
    buf[rd] = '\0';
    fclose(f);
    return buf;
}

kc_err_t kc_config_init(const char *config_path) {
    if (!config_path || strlen(config_path) == 0) {
        config_path = KC_CONFIG_FILE;
    }
    strncpy(s_config_path, config_path, sizeof(s_config_path) - 1);

    /* 尝试读取已有配置 */
    char *json_str = read_file(s_config_path);
    if (json_str) {
        s_config = cJSON_Parse(json_str);
        free(json_str);
        if (s_config) {
            KC_LOGI(TAG, "loaded config from %s", s_config_path);
            return KC_OK;
        }
        KC_LOGW(TAG, "parse failed, using empty config");
    } else {
        KC_LOGI(TAG, "no config file at %s, using defaults", s_config_path);
    }

    /* 创建空配置 */
    s_config = cJSON_CreateObject();
    return KC_OK;
}

const char *kc_config_get_str(const char *key, const char *default_val) {
    if (!s_config || !key) return default_val;

    /* 4 个 TLS 缓冲区轮转，允许同一线程连续调用多次而不互相覆盖 */
    static __thread char s_tls_buf[4][512];
    static __thread int s_tls_idx = 0;
    char *buf = s_tls_buf[s_tls_idx++ & 3];

    pthread_mutex_lock(&s_config_mutex);
    cJSON *item = cJSON_GetObjectItem(s_config, key);
    if (cJSON_IsString(item) && item->valuestring[0] != '\0') {
        strncpy(buf, item->valuestring, 511);
        buf[511] = '\0';
        pthread_mutex_unlock(&s_config_mutex);
        return buf;
    }
    pthread_mutex_unlock(&s_config_mutex);
    return default_val;
}

kc_err_t kc_config_set_str(const char *key, const char *value) {
    if (!s_config || !key) return KC_ERR_INVALID;

    pthread_mutex_lock(&s_config_mutex);
    cJSON *item = cJSON_GetObjectItem(s_config, key);
    if (item) {
        cJSON_SetValuestring(item, value);
    } else {
        cJSON_AddStringToObject(s_config, key, value);
    }
    pthread_mutex_unlock(&s_config_mutex);
    return KC_OK;
}

/* ── 运行时路径 ── */

static char s_data_dir[256]    = "";
static char s_memory_dir[256]  = "";
static char s_session_dir[256] = "";
static char s_config_dir[256]  = "";
static char s_skills_dir[256]  = "";
static char s_memory_file[256] = "";
static char s_soul_file[256]   = "";
static char s_user_file[256]   = "";

const char *kc_get_data_dir(void)    { return s_data_dir; }
const char *kc_get_memory_dir(void)  { return s_memory_dir; }
const char *kc_get_session_dir(void) { return s_session_dir; }
const char *kc_get_config_dir(void)  { return s_config_dir; }
const char *kc_get_skills_dir(void)  { return s_skills_dir; }
const char *kc_get_memory_file(void) { return s_memory_file; }
const char *kc_get_soul_file(void)   { return s_soul_file; }
const char *kc_get_user_file(void)   { return s_user_file; }

void kc_paths_init(void) {
    const char *base = kc_config_get_str("data_dir", KC_DATA_DIR_DEFAULT);

    /* 去掉末尾斜杠 */
    snprintf(s_data_dir, sizeof(s_data_dir), "%s", base);
    size_t len = strlen(s_data_dir);
    if (len > 0 && s_data_dir[len - 1] == '/') s_data_dir[len - 1] = '\0';

    snprintf(s_memory_dir,  sizeof(s_memory_dir),  "%s/memory/",   s_data_dir);
    snprintf(s_session_dir, sizeof(s_session_dir), "%s/sessions/", s_data_dir);
    snprintf(s_config_dir,  sizeof(s_config_dir),  "%s/config/",   s_data_dir);
    snprintf(s_skills_dir,  sizeof(s_skills_dir),  "%s/skills/",   s_data_dir);
    snprintf(s_memory_file, sizeof(s_memory_file), "%s/memory/MEMORY.md", s_data_dir);
    snprintf(s_soul_file,   sizeof(s_soul_file),   "%s/config/SOUL.md",   s_data_dir);
    snprintf(s_user_file,   sizeof(s_user_file),   "%s/config/USER.md",   s_data_dir);
}

cJSON *kc_config_get_json(const char *key) {
    if (!s_config || !key) return NULL;
    pthread_mutex_lock(&s_config_mutex);
    cJSON *item = cJSON_GetObjectItem(s_config, key);
    cJSON *copy = item ? cJSON_Duplicate(item, 1) : NULL;
    pthread_mutex_unlock(&s_config_mutex);
    return copy;  /* 调用者负责 cJSON_Delete */
}

void kc_config_set_json(const char *key, cJSON *val) {
    if (!s_config || !key || !val) return;
    pthread_mutex_lock(&s_config_mutex);
    cJSON *existing = cJSON_GetObjectItem(s_config, key);
    if (existing) {
        cJSON_ReplaceItemInObject(s_config, key, val);
    } else {
        cJSON_AddItemToObject(s_config, key, val);
    }
    pthread_mutex_unlock(&s_config_mutex);
}

kc_err_t kc_config_save(void) {
    if (!s_config || s_config_path[0] == '\0') return KC_ERR_INVALID;

    pthread_mutex_lock(&s_config_mutex);
    char *json_str = cJSON_Print(s_config);
    pthread_mutex_unlock(&s_config_mutex);
    if (!json_str) return KC_ERR_NO_MEM;

    FILE *f = fopen(s_config_path, "w");
    if (!f) {
        KC_LOGE(TAG, "cannot write %s", s_config_path);
        free(json_str);
        return KC_FAIL;
    }

    fputs(json_str, f);
    fclose(f);
    free(json_str);
    KC_LOGI(TAG, "config saved to %s", s_config_path);
    return KC_OK;
}
