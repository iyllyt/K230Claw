/*
 * tool_files.c - 文件操作工具
 * 路径基础为 kc_get_data_dir()，realpath() 验证防止路径遍历
 */

#include "tool_files.h"
#include "../kc_config.h"
#include "../third_party/cJSON/cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <dirent.h>
#include <sys/stat.h>
#include <limits.h>
#include <libgen.h>
#include <errno.h>

#define TAG "tool_files"
#define MAX_FILE_SIZE (32 * 1024)

/*
 * 路径安全验证：用 realpath() 解析符号链接和 ".."，
 * 确保解析后的路径在 data_dir 下（含边界分隔符检查）。
 * 对于不存在的文件（write_file 场景），解析其父目录。
 */
static bool validate_path(const char *path) {
    if (!path || path[0] != '/') return false;

    const char *data_dir = kc_get_data_dir();
    size_t data_dir_len = strlen(data_dir);
    char resolved[PATH_MAX];

    /* 尝试解析完整路径 */
    if (realpath(path, resolved) == NULL) {
        /* 文件不存在时（write_file），解析父目录 */
        if (errno != ENOENT) return false;

        char path_copy[PATH_MAX];
        snprintf(path_copy, sizeof(path_copy), "%s", path);
        char *parent = dirname(path_copy);

        if (realpath(parent, resolved) == NULL) return false;

        /* 重新拼接文件名到解析后的父目录 */
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        size_t rlen = strlen(resolved);
        snprintf(resolved + rlen, sizeof(resolved) - rlen, "/%s", base);
    }

    /* 检查解析后路径是否以 data_dir 开头，且下一个字符是 '/' 或 '\0' */
    if (strncmp(resolved, data_dir, data_dir_len) != 0) return false;
    if (resolved[data_dir_len] != '/' && resolved[data_dir_len] != '\0') return false;

    return true;
}

kc_tool_result_t *tool_read_file_execute(const char *input_json)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) return tool_result_error("Invalid JSON input");

    const char *path = cJSON_GetStringValue(cJSON_GetObjectItem(root, "path"));
    if (!validate_path(path)) {
        char buf[256];
        snprintf(buf, sizeof(buf), "Error: path must start with %s", kc_get_data_dir());
        cJSON_Delete(root);
        return tool_result_error(buf);
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        char buf[256];
        snprintf(buf, sizeof(buf), "Error: file not found: %s", path);
        cJSON_Delete(root);
        return tool_result_error(buf);
    }

    /* 读取文件内容到动态缓冲区 */
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize < 0) fsize = 0;
    if (fsize > MAX_FILE_SIZE) fsize = MAX_FILE_SIZE;

    char *content = malloc(fsize + 1);
    if (!content) { fclose(f); cJSON_Delete(root); return tool_result_error("Out of memory"); }

    size_t n = fread(content, 1, fsize, f);
    content[n] = '\0';
    fclose(f);

    KC_LOGI(TAG, "read_file: %s (%d bytes)", path, (int)n);
    cJSON_Delete(root);

    kc_tool_result_t *r = tool_result_text(content);
    free(content);
    return r;
}

kc_tool_result_t *tool_write_file_execute(const char *input_json)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) return tool_result_error("Invalid JSON input");

    const char *path    = cJSON_GetStringValue(cJSON_GetObjectItem(root, "path"));
    const char *content = cJSON_GetStringValue(cJSON_GetObjectItem(root, "content"));

    if (!validate_path(path)) {
        char buf[256];
        snprintf(buf, sizeof(buf), "Error: path must start with %s", kc_get_data_dir());
        cJSON_Delete(root);
        return tool_result_error(buf);
    }
    if (!content) {
        cJSON_Delete(root);
        return tool_result_error("Missing 'content' parameter");
    }

    FILE *f = fopen(path, "w");
    if (!f) {
        char buf[256];
        snprintf(buf, sizeof(buf), "Error: cannot write: %s", path);
        cJSON_Delete(root);
        return tool_result_error(buf);
    }

    size_t len = strlen(content);
    size_t written = fwrite(content, 1, len, f);
    fclose(f);

    if (written != len) {
        char buf[128];
        snprintf(buf, sizeof(buf), "Error: wrote %d of %d bytes", (int)written, (int)len);
        cJSON_Delete(root);
        return tool_result_error(buf);
    }

    char buf[256];
    snprintf(buf, sizeof(buf), "OK: wrote %d bytes to %s", (int)written, path);
    KC_LOGI(TAG, "write_file: %s (%d bytes)", path, (int)written);
    cJSON_Delete(root);
    return tool_result_text(buf);
}

kc_tool_result_t *tool_list_dir_execute(const char *input_json)
{
    cJSON *root = cJSON_Parse(input_json);
    const char *dir_path = kc_get_data_dir();
    if (root) {
        cJSON *p = cJSON_GetObjectItem(root, "path");
        if (p && cJSON_IsString(p) && p->valuestring[0])
            dir_path = p->valuestring;
    }

    if (!validate_path(dir_path)) {
        char buf[256];
        snprintf(buf, sizeof(buf), "Error: path must start with %s", kc_get_data_dir());
        cJSON_Delete(root);
        return tool_result_error(buf);
    }

    DIR *dir = opendir(dir_path);
    if (!dir) {
        char buf[256];
        snprintf(buf, sizeof(buf), "Error: cannot open %s", dir_path);
        cJSON_Delete(root);
        return tool_result_error(buf);
    }

    /* 动态构建结果 */
    size_t buf_size = 4096;
    char *buf = malloc(buf_size);
    if (!buf) { closedir(dir); cJSON_Delete(root); return tool_result_error("Out of memory"); }

    size_t off = 0;
    struct dirent *ent;
    int count = 0;

    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;

        size_t needed = strlen(dir_path) + strlen(ent->d_name) + 3;
        if (off + needed >= buf_size) {
            buf_size *= 2;
            char *tmp = realloc(buf, buf_size);
            if (!tmp) break;
            buf = tmp;
        }
        off += snprintf(buf + off, buf_size - off, "%s/%s\n", dir_path, ent->d_name);
        count++;
    }
    closedir(dir);

    if (count == 0) {
        snprintf(buf, buf_size, "(no files found)");
    }

    KC_LOGI(TAG, "list_dir: %d entries in %s", count, dir_path);
    cJSON_Delete(root);

    kc_tool_result_t *r = tool_result_text(buf);
    free(buf);
    return r;
}
