/*
 * session_mgr.c - JSONL 会话历史管理
 * 从 MimiClaw 移植: 路径替换，环形缓冲区保留最近 N 条消息
 */

#include "session_mgr.h"
#include "../kc_config.h"
#include "../third_party/cJSON/cJSON.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>

#define TAG "session"
#define INITIAL_LINE_SIZE 4096

/* 动态读取一整行（不受固定缓冲区限制） */
static char *read_line_dynamic(FILE *f)
{
    size_t cap = INITIAL_LINE_SIZE;
    size_t len = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;

    while (1) {
        if (fgets(buf + len, (int)(cap - len), f) == NULL) {
            if (len == 0) { free(buf); return NULL; } /* EOF 且无数据 */
            break; /* EOF 但已有数据 */
        }
        len += strlen(buf + len);
        if (len > 0 && buf[len - 1] == '\n') break; /* 读到完整行 */
        /* 缓冲区满但未遇到换行，扩容 */
        cap *= 2;
        char *tmp = realloc(buf, cap);
        if (!tmp) { free(buf); return NULL; }
        buf = tmp;
    }

    /* 去掉尾部换行 */
    if (len > 0 && buf[len - 1] == '\n') buf[--len] = '\0';

    return buf;
}

static void session_path(const char *chat_id, char *buf, size_t size)
{
    snprintf(buf, size, "%s%s.jsonl", kc_get_session_dir(), chat_id);
}

kc_err_t session_mgr_init(void)
{
    KC_LOGI(TAG, "session manager initialized at %s", kc_get_session_dir());
    return KC_OK;
}

static int s_append_count = 0;

/* 保留最后 KC_SESSION_MAX_MSGS 条记录，崩溃安全（临时文件 → fsync → rename） */
static void session_compact(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return;

    /* 收集所有行 */
    char **lines = NULL;
    int total = 0, cap = 0;
    char *line;
    while ((line = read_line_dynamic(f)) != NULL) {
        if (line[0] == '\0') { free(line); continue; }
        if (total >= cap) {
            cap = cap ? cap * 2 : 64;
            char **tmp = realloc(lines, cap * sizeof(char *));
            if (!tmp) { free(line); break; }
            lines = tmp;
        }
        lines[total++] = line;
    }
    fclose(f);

    if (total <= KC_SESSION_MAX_MSGS) {
        /* 无需 compact */
        for (int i = 0; i < total; i++) free(lines[i]);
        free(lines);
        return;
    }

    /* 写入临时文件 */
    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    FILE *out = fopen(tmp_path, "w");
    if (!out) {
        for (int i = 0; i < total; i++) free(lines[i]);
        free(lines);
        return;
    }

    int start = total - KC_SESSION_MAX_MSGS;
    for (int i = start; i < total; i++)
        fprintf(out, "%s\n", lines[i]);

    fflush(out);
    fsync(fileno(out));
    fclose(out);

    rename(tmp_path, path);
    KC_LOGI(TAG, "session compacted: %d → %d lines", total, total - start);

    for (int i = 0; i < total; i++) free(lines[i]);
    free(lines);
}

kc_err_t session_append(const char *chat_id, const char *role, const char *content)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "role", role);
    cJSON_AddStringToObject(obj, "content", content);
    cJSON_AddNumberToObject(obj, "ts", (double)time(NULL));
    kc_err_t err = session_append_message(chat_id, obj);
    cJSON_Delete(obj);
    return err;
}

kc_err_t session_append_message(const char *chat_id, cJSON *message)
{
    if (!message) return KC_ERR_INVALID;

    char path[128];
    session_path(chat_id, path, sizeof(path));

    FILE *f = fopen(path, "a");
    if (!f) {
        /* 尝试创建目录后重试 */
        const char *dir = kc_get_session_dir();
        mkdir(dir, 0755);
        f = fopen(path, "a");
    }
    if (!f) { KC_LOGE(TAG, "cannot open %s: %s", path, strerror(errno)); return KC_FAIL; }

    /* 添加时间戳（如果消息中没有） */
    if (!cJSON_GetObjectItem(message, "ts"))
        cJSON_AddNumberToObject(message, "ts", (double)time(NULL));

    char *line = cJSON_PrintUnformatted(message);
    if (line) { fprintf(f, "%s\n", line); free(line); }

    fclose(f);

    /* 定期 compact */
    if (++s_append_count >= KC_SESSION_COMPACT_INTERVAL) {
        s_append_count = 0;
        session_compact(path);
    }

    return KC_OK;
}

kc_err_t session_get_history_json(const char *chat_id, char *buf, size_t size, int max_msgs)
{
    char path[128];
    session_path(chat_id, path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) { snprintf(buf, size, "[]"); return KC_OK; }

    /* 环形缓冲区保留最后 max_msgs 条 */
    cJSON **ring = calloc(max_msgs, sizeof(cJSON *));
    if (!ring) { fclose(f); snprintf(buf, size, "[]"); return KC_ERR_NO_MEM; }

    int count = 0, write_idx = 0;
    char *line;

    while ((line = read_line_dynamic(f)) != NULL) {
        if (line[0] == '\0') { free(line); continue; }

        cJSON *obj = cJSON_Parse(line);
        free(line);
        if (!obj) continue;

        if (count >= max_msgs)
            cJSON_Delete(ring[write_idx]);
        ring[write_idx] = obj;
        write_idx = (write_idx + 1) % max_msgs;
        if (count < max_msgs) count++;
    }
    fclose(f);

    /* 构建 JSON 数组 — 保留完整消息结构 */
    cJSON *arr = cJSON_CreateArray();
    int start = (count < max_msgs) ? 0 : write_idx;
    for (int i = 0; i < count; i++) {
        int idx = (start + i) % max_msgs;
        cJSON *src = ring[idx];
        cJSON *role_item = cJSON_GetObjectItem(src, "role");
        cJSON *content_item = cJSON_GetObjectItem(src, "content");
        if (!role_item) continue;

        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "role", role_item->valuestring);

        /* content 可能是字符串（旧格式）或数组（新格式，含 tool_use/tool_result） */
        if (content_item) {
            cJSON_AddItemToObject(entry, "content", cJSON_Duplicate(content_item, 1));
        }

        cJSON_AddItemToArray(arr, entry);
    }

    /* 清理 */
    int cleanup_start = (count < max_msgs) ? 0 : write_idx;
    for (int i = 0; i < count; i++)
        cJSON_Delete(ring[(cleanup_start + i) % max_msgs]);
    free(ring);

    char *json_str = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (json_str) { strncpy(buf, json_str, size - 1); buf[size - 1] = '\0'; free(json_str); }
    else snprintf(buf, size, "[]");

    return KC_OK;
}

cJSON *session_get_history(const char *chat_id, int max_msgs)
{
    char path[128];
    session_path(chat_id, path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) return cJSON_CreateArray();

    cJSON **ring = calloc(max_msgs, sizeof(cJSON *));
    if (!ring) { fclose(f); return cJSON_CreateArray(); }

    int count = 0, write_idx = 0;
    char *line;

    while ((line = read_line_dynamic(f)) != NULL) {
        if (line[0] == '\0') { free(line); continue; }
        cJSON *obj = cJSON_Parse(line);
        free(line);
        if (!obj) continue;
        if (count >= max_msgs)
            cJSON_Delete(ring[write_idx]);
        ring[write_idx] = obj;
        write_idx = (write_idx + 1) % max_msgs;
        if (count < max_msgs) count++;
    }
    fclose(f);

    /*
     * 构建返回数组，同时修复消息序列：
     * - 跳过开头的孤立 tool_result（缺少配对的 tool_use assistant 消息）
     * - 跳过开头的孤立 assistant tool_use（缺少后续 tool_result）
     */
    cJSON *arr = cJSON_CreateArray();
    int start = (count < max_msgs) ? 0 : write_idx;

    /* 找到第一条安全的起始消息 */
    int skip = 0;
    for (int i = 0; i < count; i++) {
        int idx = (start + i) % max_msgs;
        cJSON *src = ring[idx];
        cJSON *role_item = cJSON_GetObjectItem(src, "role");
        if (!role_item) { skip = i + 1; continue; }

        const char *role = role_item->valuestring;
        cJSON *content = cJSON_GetObjectItem(src, "content");

        /* 跳过孤立的 tool_result（user 消息中 content 数组首元素 type=tool_result） */
        if (strcmp(role, "user") == 0 && content && cJSON_IsArray(content)) {
            cJSON *first = cJSON_GetArrayItem(content, 0);
            cJSON *ftype = first ? cJSON_GetObjectItem(first, "type") : NULL;
            if (ftype && cJSON_IsString(ftype) &&
                strcmp(ftype->valuestring, "tool_result") == 0) {
                skip = i + 1;
                continue;
            }
        }

        /* 跳过孤立的 assistant tool_use（没有后续 tool_result 配对） */
        if (strcmp(role, "assistant") == 0 && content && cJSON_IsArray(content)) {
            int has_tool_use = 0;
            cJSON *block;
            cJSON_ArrayForEach(block, content) {
                cJSON *btype = cJSON_GetObjectItem(block, "type");
                if (btype && cJSON_IsString(btype) &&
                    strcmp(btype->valuestring, "tool_use") == 0) {
                    has_tool_use = 1;
                    break;
                }
            }
            if (has_tool_use) {
                /* 检查下一条是否是 tool_result */
                if (i + 1 < count) {
                    int next_idx = (start + i + 1) % max_msgs;
                    cJSON *next = ring[next_idx];
                    cJSON *nr = cJSON_GetObjectItem(next, "role");
                    cJSON *nc = cJSON_GetObjectItem(next, "content");
                    int next_is_result = 0;
                    if (nr && strcmp(nr->valuestring, "user") == 0 &&
                        nc && cJSON_IsArray(nc)) {
                        cJSON *nf = cJSON_GetArrayItem(nc, 0);
                        cJSON *nft = nf ? cJSON_GetObjectItem(nf, "type") : NULL;
                        if (nft && cJSON_IsString(nft) &&
                            strcmp(nft->valuestring, "tool_result") == 0)
                            next_is_result = 1;
                    }
                    if (!next_is_result) { skip = i + 1; continue; }
                } else {
                    skip = i + 1; continue;
                }
            }
        }
        break;  /* 找到安全起始点 */
    }

    if (skip > 0)
        KC_LOGW(TAG, "skipped %d orphaned tool messages at history start", skip);

    for (int i = skip; i < count; i++) {
        int idx = (start + i) % max_msgs;
        cJSON *src = ring[idx];
        cJSON *role_item = cJSON_GetObjectItem(src, "role");
        cJSON *content_item = cJSON_GetObjectItem(src, "content");
        if (!role_item) continue;

        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "role", role_item->valuestring);
        if (content_item)
            cJSON_AddItemToObject(entry, "content", cJSON_Duplicate(content_item, 1));
        cJSON_AddItemToArray(arr, entry);
    }

    int cleanup_start = (count < max_msgs) ? 0 : write_idx;
    for (int i = 0; i < count; i++)
        cJSON_Delete(ring[(cleanup_start + i) % max_msgs]);
    free(ring);

    return arr;
}

kc_err_t session_clear(const char *chat_id)
{
    char path[128];
    session_path(chat_id, path, sizeof(path));
    if (remove(path) == 0) { KC_LOGI(TAG, "session %s cleared", chat_id); return KC_OK; }
    return KC_ERR_NOT_FOUND;
}

void session_list(void)
{
    DIR *dir = opendir(kc_get_session_dir());
    if (!dir) { printf("No sessions directory.\n"); return; }

    struct dirent *entry;
    int count = 0;
    printf("Sessions:\n");
    while ((entry = readdir(dir)) != NULL) {
        if (strstr(entry->d_name, ".jsonl")) {
            /* 去掉 .jsonl 后缀显示 */
            char name[128];
            strncpy(name, entry->d_name, sizeof(name) - 1);
            name[sizeof(name) - 1] = '\0';
            char *dot = strstr(name, ".jsonl");
            if (dot) *dot = '\0';
            printf("  %s\n", name);
            count++;
        }
    }
    closedir(dir);
    if (count == 0) printf("  (no sessions)\n");
}
