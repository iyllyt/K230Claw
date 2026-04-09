/*
 * skill_loader.c - 技能文件加载器
 *
 * Phase 3 PART 8: 支持 frontmatter triggers 关键词触发。
 *
 * frontmatter 格式:
 *   ---
 *   triggers: keyword1, keyword2, keyword3
 *   ---
 *
 * 匹配规则：用户消息（转小写后）包含任一触发词则匹配。
 * 匹配后返回完整技能内容（跳过 frontmatter）。
 */

#include "skill_loader.h"
#include "../kc_config.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <dirent.h>

#define TAG "skills"
#define MAX_SKILLS      32
#define MAX_TRIGGERS    16
#define MAX_TRIGGER_LEN 32

typedef struct {
    char path[296];
    char title[64];
    char desc[256];
    char triggers[MAX_TRIGGERS][MAX_TRIGGER_LEN];
    int  trigger_count;
} skill_entry_t;

static skill_entry_t s_skills[MAX_SKILLS];
static int s_skill_count = 0;

/* 转小写（原地修改） */
static void str_tolower(char *s) {
    for (; *s; s++) *s = tolower((unsigned char)*s);
}

/* 解析 frontmatter 中的 triggers 行 */
static void parse_triggers(const char *line, skill_entry_t *skill)
{
    /* "triggers: keyword1, keyword2, keyword3" */
    const char *p = line;
    while (*p && *p != ':') p++;
    if (*p == ':') p++;
    while (*p == ' ') p++;

    skill->trigger_count = 0;
    while (*p && skill->trigger_count < MAX_TRIGGERS) {
        /* 跳过空格 */
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '\n') break;

        /* 读取到逗号或行尾 */
        char trigger[MAX_TRIGGER_LEN];
        int i = 0;
        while (*p && *p != ',' && *p != '\n' && *p != '\r' && i < MAX_TRIGGER_LEN - 1) {
            trigger[i++] = *p++;
        }
        trigger[i] = '\0';

        /* 去尾空格 */
        while (i > 0 && trigger[i-1] == ' ') trigger[--i] = '\0';

        if (i > 0) {
            str_tolower(trigger);
            strncpy(skill->triggers[skill->trigger_count], trigger, MAX_TRIGGER_LEN - 1);
            skill->trigger_count++;
        }

        if (*p == ',') p++;
    }
}

/* 从第一行提取标题 */
static void extract_title(const char *line, size_t len, char *out, size_t out_size)
{
    const char *start = line;
    if (len >= 2 && line[0] == '#' && line[1] == ' ') { start = line + 2; len -= 2; }
    while (len > 0 && (start[len-1] == '\n' || start[len-1] == '\r' || start[len-1] == ' ')) len--;
    size_t copy = len < out_size - 1 ? len : out_size - 1;
    memcpy(out, start, copy);
    out[copy] = '\0';
}

/* 提取描述 */
static void extract_description(FILE *f, char *out, size_t out_size)
{
    size_t off = 0;
    char line[256];
    while (fgets(line, sizeof(line), f) && off < out_size - 1) {
        size_t len = strlen(line);
        if (len == 0 || (len == 1 && line[0] == '\n') ||
            (len >= 2 && line[0] == '#' && line[1] == '#'))
            break;
        if (off == 0 && line[0] == '\n') continue;
        if (line[len-1] == '\n') line[len-1] = ' ';
        size_t copy = len < out_size - off - 1 ? len : out_size - off - 1;
        memcpy(out + off, line, copy);
        off += copy;
    }
    while (off > 0 && out[off-1] == ' ') off--;
    out[off] = '\0';
}

/* 加载单个技能文件 */
static void load_skill(const char *dir_path, const char *filename)
{
    if (s_skill_count >= MAX_SKILLS) return;

    skill_entry_t *skill = &s_skills[s_skill_count];
    memset(skill, 0, sizeof(*skill));
    snprintf(skill->path, sizeof(skill->path), "%s%s", dir_path, filename);

    FILE *f = fopen(skill->path, "r");
    if (!f) return;

    char line[512];
    int in_frontmatter = 0;
    int frontmatter_done = 0;

    /* 读取 frontmatter */
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        /* 去尾换行 */
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';

        if (strcmp(line, "---") == 0) {
            if (!in_frontmatter) {
                in_frontmatter = 1;
                continue;
            } else {
                frontmatter_done = 1;
                break;
            }
        }

        if (in_frontmatter) {
            if (strncmp(line, "triggers:", 9) == 0) {
                parse_triggers(line, skill);
            }
        } else {
            /* 没有 frontmatter，第一行就是标题 */
            extract_title(line, len, skill->title, sizeof(skill->title));
            extract_description(f, skill->desc, sizeof(skill->desc));
            fclose(f);
            s_skill_count++;
            return;
        }
    }

    /* frontmatter 后读取标题和描述 */
    if (fgets(line, sizeof(line), f)) {
        extract_title(line, strlen(line), skill->title, sizeof(skill->title));
        extract_description(f, skill->desc, sizeof(skill->desc));
    }

    fclose(f);
    s_skill_count++;
}

kc_err_t skill_loader_init(void)
{
    s_skill_count = 0;
    const char *skills_dir = kc_get_skills_dir();

    DIR *dir = opendir(skills_dir);
    if (!dir) {
        KC_LOGI(TAG, "no skills directory at %s", skills_dir);
        return KC_OK;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        size_t len = strlen(ent->d_name);
        if (len > 3 && strcmp(ent->d_name + len - 3, ".md") == 0)
            load_skill(skills_dir, ent->d_name);
    }
    closedir(dir);

    int total_triggers = 0;
    for (int i = 0; i < s_skill_count; i++)
        total_triggers += s_skills[i].trigger_count;

    KC_LOGI(TAG, "skills ready: %d skills, %d triggers", s_skill_count, total_triggers);
    return KC_OK;
}

size_t skill_loader_build_summary(char *buf, size_t size)
{
    size_t off = 0;
    for (int i = 0; i < s_skill_count && off < size - 1; i++) {
        off += snprintf(buf + off, size - off,
            "- **%s**: %s (read_file %s)\n",
            s_skills[i].title, s_skills[i].desc, s_skills[i].path);
    }
    buf[off] = '\0';
    return off;
}

char *skill_loader_match_triggers(const char *user_message)
{
    if (!user_message || s_skill_count == 0) return NULL;

    /* 转小写副本用于匹配 */
    size_t msg_len = strlen(user_message);
    char *msg_lower = malloc(msg_len + 1);
    if (!msg_lower) return NULL;
    memcpy(msg_lower, user_message, msg_len + 1);
    str_tolower(msg_lower);

    /* 检查每个技能的 triggers */
    for (int i = 0; i < s_skill_count; i++) {
        for (int t = 0; t < s_skills[i].trigger_count; t++) {
            if (strstr(msg_lower, s_skills[i].triggers[t]) != NULL) {
                /* 匹配！读取完整技能内容 */
                free(msg_lower);

                FILE *f = fopen(s_skills[i].path, "r");
                if (!f) return NULL;

                fseek(f, 0, SEEK_END);
                long fsize = ftell(f);
                fseek(f, 0, SEEK_SET);
                if (fsize <= 0 || fsize > 32768) { fclose(f); return NULL; }

                char *content = malloc(fsize + 1);
                if (!content) { fclose(f); return NULL; }
                size_t rd = fread(content, 1, fsize, f);
                content[rd] = '\0';
                fclose(f);

                /* 跳过 frontmatter */
                char *body = content;
                if (strncmp(body, "---", 3) == 0) {
                    char *end = strstr(body + 3, "\n---");
                    if (end) {
                        body = end + 4;
                        while (*body == '\n' || *body == '\r') body++;
                    }
                }

                /* 返回 body 部分（需要新分配，因为 content 包含 frontmatter） */
                char *result = strdup(body);
                free(content);

                KC_LOGI(TAG, "skill triggered: '%s' matched keyword '%s'",
                        s_skills[i].title, s_skills[i].triggers[t]);
                return result;
            }
        }
    }

    free(msg_lower);
    return NULL;
}
