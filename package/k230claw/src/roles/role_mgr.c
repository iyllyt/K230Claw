/*
 * role_mgr.c - 角色管理器
 *
 * 角色系统：开机默认模式 + 按需切换。
 * 每个角色定义启用哪些传感器、禁用哪些工具、角色个性。
 */

#include "role_mgr.h"
#include "../kc_config.h"
#include "../sensors/sensor_mgr.h"
#include "../tools/tool_registry.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <ctype.h>

#define TAG "role_mgr"
#define MAX_ROLES          16
#define MAX_SENSORS_PER    4
#define MAX_DIS_TOOLS_PER  8
#define NAME_LEN           32
#define DESC_LEN          128

typedef struct {
    char name[NAME_LEN];
    char description[DESC_LEN];
    char sensors[MAX_SENSORS_PER][NAME_LEN];
    int  sensor_count;
    char disabled_tools[MAX_DIS_TOOLS_PER][NAME_LEN];
    int  disabled_tool_count;
    char *personality;          /* malloc'd, body after frontmatter */
} kc_role_t;

static kc_role_t s_roles[MAX_ROLES];
static int s_role_count = 0;
static int s_active = -1;       /* index into s_roles, -1 = none */

/* ── 内部工具函数 ── */

/* 解析逗号分隔的值到数组，返回个数 */
static int parse_csv(const char *line, char out[][NAME_LEN], int max_items)
{
    const char *p = line;
    /* 跳过 "key: " */
    while (*p && *p != ':') p++;
    if (*p == ':') p++;
    while (*p == ' ') p++;

    int count = 0;
    while (*p && count < max_items) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '\n') break;

        int i = 0;
        while (*p && *p != ',' && *p != '\n' && *p != '\r' && i < NAME_LEN - 1) {
            out[count][i++] = *p++;
        }
        out[count][i] = '\0';

        /* 去尾空格 */
        while (i > 0 && out[count][i-1] == ' ') out[count][--i] = '\0';

        if (i > 0) count++;
        if (*p == ',') p++;
    }
    return count;
}

/* 提取 "key: value" 中的 value 到 out */
static void parse_value(const char *line, char *out, size_t out_size)
{
    const char *p = line;
    while (*p && *p != ':') p++;
    if (*p == ':') p++;
    while (*p == ' ') p++;

    size_t len = strlen(p);
    while (len > 0 && (p[len-1] == '\n' || p[len-1] == '\r' || p[len-1] == ' '))
        len--;

    size_t copy = len < out_size - 1 ? len : out_size - 1;
    memcpy(out, p, copy);
    out[copy] = '\0';
}

/* 注册内置 default 角色 */
static void register_default_role(void)
{
    kc_role_t *r = &s_roles[0];
    memset(r, 0, sizeof(*r));
    strncpy(r->name, "default", NAME_LEN - 1);
    strncpy(r->description, "Default mode: all tools, no sensors", DESC_LEN - 1);
    r->sensor_count = 0;
    r->disabled_tool_count = 0;
    r->personality = NULL;
    s_role_count = 1;
}

/* 从 .md 文件加载角色定义 */
static void load_role_file(const char *dir_path, const char *filename)
{
    if (s_role_count >= MAX_ROLES) return;

    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir_path, filename);

    FILE *f = fopen(path, "r");
    if (!f) return;

    kc_role_t *role = &s_roles[s_role_count];
    memset(role, 0, sizeof(*role));

    char line[512];
    int in_frontmatter = 0;
    int frontmatter_done = 0;

    /* 解析 frontmatter */
    while (fgets(line, sizeof(line), f)) {
        /* 检测 --- 分隔符 */
        if (strncmp(line, "---", 3) == 0) {
            if (!in_frontmatter) {
                in_frontmatter = 1;
                continue;
            } else {
                frontmatter_done = 1;
                break;
            }
        }

        if (!in_frontmatter) continue;

        if (strncmp(line, "name:", 5) == 0) {
            parse_value(line, role->name, NAME_LEN);
        } else if (strncmp(line, "description:", 12) == 0) {
            parse_value(line, role->description, DESC_LEN);
        } else if (strncmp(line, "sensors:", 8) == 0) {
            role->sensor_count = parse_csv(line, role->sensors, MAX_SENSORS_PER);
        } else if (strncmp(line, "disabled_tools:", 15) == 0) {
            role->disabled_tool_count = parse_csv(line, role->disabled_tools, MAX_DIS_TOOLS_PER);
        }
    }

    if (!frontmatter_done || role->name[0] == '\0') {
        fclose(f);
        return;
    }

    /* 检查是否与已有角色重名 */
    for (int i = 0; i < s_role_count; i++) {
        if (strcmp(s_roles[i].name, role->name) == 0) {
            KC_LOGW(TAG, "duplicate role name '%s', skipping %s", role->name, filename);
            fclose(f);
            return;
        }
    }

    /* 读取 body（frontmatter 之后的全部内容）作为 personality */
    long body_start = ftell(f);
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    long body_len = file_size - body_start;

    if (body_len > 0) {
        role->personality = malloc(body_len + 1);
        if (role->personality) {
            fseek(f, body_start, SEEK_SET);
            size_t nread = fread(role->personality, 1, body_len, f);
            role->personality[nread] = '\0';
            /* 去掉开头的空行 */
            char *p = role->personality;
            while (*p == '\n' || *p == '\r') p++;
            if (p != role->personality)
                memmove(role->personality, p, strlen(p) + 1);
        }
    }

    fclose(f);
    s_role_count++;
    KC_LOGI(TAG, "loaded role: %s (%s)", role->name, role->description);
}

/* ── 公共 API ── */

kc_err_t role_mgr_init(void)
{
    register_default_role();

    /* 扫描 roles/ 目录 */
    char roles_dir[296];
    snprintf(roles_dir, sizeof(roles_dir), "%s/roles", kc_get_data_dir());

    DIR *dir = opendir(roles_dir);
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            size_t len = strlen(ent->d_name);
            if (len > 3 && strcmp(ent->d_name + len - 3, ".md") == 0) {
                load_role_file(roles_dir, ent->d_name);
            }
        }
        closedir(dir);
    }

    KC_LOGI(TAG, "loaded %d roles", s_role_count);

    /* 应用上次保存的角色 */
    const char *saved = kc_config_get_str("active_role", "default");
    kc_err_t err = role_mgr_switch(saved);
    if (err != KC_OK) {
        KC_LOGW(TAG, "saved role '%s' not found, using default", saved);
        role_mgr_switch("default");
    }

    return KC_OK;
}

kc_err_t role_mgr_switch(const char *name)
{
    if (!name) return KC_ERR_INVALID;

    /* 查找目标角色 */
    int target = -1;
    for (int i = 0; i < s_role_count; i++) {
        if (strcmp(s_roles[i].name, name) == 0) {
            target = i;
            break;
        }
    }
    if (target < 0) return KC_ERR_NOT_FOUND;
    if (target == s_active) return KC_OK;  /* 已是当前角色 */

    /* 1. 停止旧角色的传感器 */
    if (s_active >= 0) {
        kc_role_t *old = &s_roles[s_active];
        for (int i = 0; i < old->sensor_count; i++) {
            sensor_mgr_stop_by_name(old->sensors[i]);
        }
    }

    /* 2. 重置工具状态 → 全部启用 */
    tool_registry_reset_all_enabled();

    /* 3. 禁用新角色指定的工具 */
    kc_role_t *role = &s_roles[target];
    for (int i = 0; i < role->disabled_tool_count; i++) {
        tool_registry_set_disabled(role->disabled_tools[i], true);
    }

    /* 4. 重建 tools JSON */
    tool_registry_rebuild();

    /* 5. 更新活跃角色 */
    s_active = target;

    /* 6. 启动新角色的传感器 */
    for (int i = 0; i < role->sensor_count; i++) {
        kc_err_t err = sensor_mgr_start_by_name(role->sensors[i]);
        if (err != KC_OK) {
            KC_LOGW(TAG, "failed to start sensor '%s' for role '%s'",
                    role->sensors[i], role->name);
        }
    }

    /* 7. 保存到配置 */
    kc_config_set_str("active_role", name);

    KC_LOGI(TAG, "switched to role: %s", name);
    return KC_OK;
}

const char *role_mgr_get_active(void)
{
    if (s_active >= 0 && s_active < s_role_count)
        return s_roles[s_active].name;
    return "default";
}

char *role_mgr_list(void)
{
    /* 格式：
     * Available roles (* = active):
     *   * default — Default mode: all tools, no sensors
     *     桌面搭子 — 桌面智能伴侣
     */
    size_t buf_size = 1024;
    char *buf = malloc(buf_size);
    if (!buf) return NULL;

    size_t off = 0;
    off += snprintf(buf + off, buf_size - off, "Available roles (* = active):\n");

    for (int i = 0; i < s_role_count && off < buf_size - 1; i++) {
        const char *marker = (i == s_active) ? "*" : " ";
        off += snprintf(buf + off, buf_size - off, "  %s %s", marker, s_roles[i].name);

        if (s_roles[i].description[0])
            off += snprintf(buf + off, buf_size - off, " — %s", s_roles[i].description);

        /* 显示传感器列表 */
        if (s_roles[i].sensor_count > 0) {
            off += snprintf(buf + off, buf_size - off, " [sensors:");
            for (int j = 0; j < s_roles[i].sensor_count && off < buf_size - 1; j++)
                off += snprintf(buf + off, buf_size - off, " %s", s_roles[i].sensors[j]);
            off += snprintf(buf + off, buf_size - off, "]");
        }

        off += snprintf(buf + off, buf_size - off, "\n");
    }

    return buf;
}

const char *role_mgr_get_personality(void)
{
    if (s_active >= 0 && s_active < s_role_count)
        return s_roles[s_active].personality;
    return NULL;
}
