/*
 * memory_store.c - 持久化长期记忆和每日笔记
 * 从 MimiClaw 移植: 路径 /spiffs/ → /var/lib/k230claw/，日志宏替换
 */

#include "memory_store.h"
#include "../kc_config.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#define TAG "memory"

static void get_date_str(char *buf, size_t size, int days_ago)
{
    time_t now;
    time(&now);
    now -= days_ago * 86400;
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(buf, size, "%Y-%m-%d", &tm);
}

kc_err_t memory_store_init(void)
{
    /* Linux 上目录由 init 脚本创建 */
    KC_LOGI(TAG, "memory store initialized at %s", kc_get_memory_dir());
    return KC_OK;
}

kc_err_t memory_read_long_term(char *buf, size_t size)
{
    FILE *f = fopen(kc_get_memory_file(), "r");
    if (!f) { buf[0] = '\0'; return KC_ERR_NOT_FOUND; }
    size_t n = fread(buf, 1, size - 1, f);
    buf[n] = '\0';
    fclose(f);
    return KC_OK;
}

kc_err_t memory_write_long_term(const char *content)
{
    FILE *f = fopen(kc_get_memory_file(), "w");
    if (!f) { KC_LOGE(TAG, "cannot write %s", kc_get_memory_file()); return KC_FAIL; }
    fputs(content, f);
    fclose(f);
    KC_LOGI(TAG, "long-term memory updated (%d bytes)", (int)strlen(content));
    return KC_OK;
}

kc_err_t memory_append_today(const char *note)
{
    char date_str[16];
    get_date_str(date_str, sizeof(date_str), 0);

    char path[128];
    snprintf(path, sizeof(path), "%s%s.md", kc_get_memory_dir(), date_str);

    FILE *f = fopen(path, "a");
    if (!f) {
        f = fopen(path, "w");
        if (!f) { KC_LOGE(TAG, "cannot open %s", path); return KC_FAIL; }
        fprintf(f, "# %s\n\n", date_str);
    }
    fprintf(f, "%s\n", note);
    fclose(f);
    return KC_OK;
}

kc_err_t memory_read_recent(char *buf, size_t size, int days)
{
    size_t offset = 0;
    buf[0] = '\0';

    for (int i = 0; i < days && offset < size - 1; i++) {
        char date_str[16];
        get_date_str(date_str, sizeof(date_str), i);

        char path[128];
        snprintf(path, sizeof(path), "%s%s.md", kc_get_memory_dir(), date_str);

        FILE *f = fopen(path, "r");
        if (!f) continue;

        if (offset > 0 && offset < size - 4)
            offset += snprintf(buf + offset, size - offset, "\n---\n");

        size_t n = fread(buf + offset, 1, size - offset - 1, f);
        offset += n;
        buf[offset] = '\0';
        fclose(f);
    }
    return KC_OK;
}
