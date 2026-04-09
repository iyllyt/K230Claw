/*
 * context_builder.c - 系统提示词构建
 *
 * 从 MimiClaw context_builder.c 移植:
 *   - 设备描述改为 K230 Linux
 *   - 工具列表更新为 K230 版本
 *   - 路径常量替换
 */

#include "context_builder.h"
#include "../kc_config.h"
#include "../memory/memory_store.h"
#include "../skills/skill_loader.h"
#include "../roles/role_mgr.h"

#include <stdio.h>
#include <string.h>

#define TAG "context"
#define SYSTEM_PROMPT_FILE "/etc/k230claw/system_prompt.md"

/* 从文件追加内容到 buf */
static size_t append_file(char *buf, size_t size, size_t offset, const char *path, const char *header)
{
    FILE *f = fopen(path, "r");
    if (!f) return offset;

    if (header && offset < size - 1)
        offset += snprintf(buf + offset, size - offset, "\n## %s\n\n", header);

    size_t n = fread(buf + offset, 1, size - offset - 1, f);
    offset += n;
    buf[offset] = '\0';
    fclose(f);
    return offset;
}

/* 内置的角色+环境提示词（当外置文件不存在时的 fallback） */
static const char *s_builtin_preamble =
    "# K230Claw\n\n"
    "You are K230Claw, a personal AI assistant running on a K230 RISC-V SoC (Linux 6.6).\n"
    "You run on the big core (1.6GHz) with full Linux capabilities.\n\n"
    "Be helpful, accurate, and concise.\n\n"
    "## System Environment\n"
    "This is an embedded Linux system (Buildroot minimal rootfs), NOT a standard distro.\n"
    "- Shell: /bin/sh (BusyBox ash), NOT bash. Avoid bash-specific syntax.\n"
    "- BusyBox applets: ls, ps, top, grep, etc. are BusyBox versions with limited flags (not GNU coreutils).\n"
    "- NO package manager: apt, yum, dnf, pacman do NOT exist. Software cannot be installed at runtime.\n"
    "- NO systemd: no systemctl, journalctl, timedatectl. Init is BusyBox init with /etc/init.d/S* scripts.\n"
    "- Network tools: ifconfig, udhcpc, wpa_supplicant, iwconfig, ping. No 'ip' command or NetworkManager.\n"
    "- WiFi: managed via wpa_supplicant + udhcpc. Config at /etc/wpa_supplicant.conf.\n"
    "- Board: LuShanPi K230 (1GB LPDDR4, RTL8189FTV WiFi, GC2093 camera).\n"
    "- Known hardware quirk: WiFi module (RTL8189FTV) can deadlock due to LPS power-save bug. "
    "If WiFi stops working, a cold reboot (power cycle) is needed.\n\n";

kc_err_t context_build_system_prompt(char *buf, size_t size)
{
    size_t off = 0;

    /* 尝试从外置文件读取角色+环境部分 */
    FILE *pf = fopen(SYSTEM_PROMPT_FILE, "r");
    if (pf) {
        size_t n = fread(buf, 1, size / 2, pf); /* 限制为缓冲区一半，留空间给动态段落 */
        off = n;
        buf[off] = '\0';
        fclose(pf);
        if (off > 0 && buf[off - 1] != '\n')
            off += snprintf(buf + off, size - off, "\n");
    } else {
        off += snprintf(buf + off, size - off, "%s", s_builtin_preamble);
    }

    /* 动态拼接的段落（含路径） */
    off += snprintf(buf + off, size - off,
        "## Tool Usage Guidelines\n"
        "You have access to tools passed via the API tools parameter. Use them when needed.\n"
        "- Always use get_current_time for date/time queries (do NOT guess).\n"
        "- File tools (read_file, write_file, list_dir) are restricted to paths under %s.\n"
        "- Before calling run_command, you MUST explain to the user what command you will run and why. Never run commands silently.\n"
        "- run_command runs on BusyBox ash shell. Use BusyBox-compatible commands only.\n"
        "- Provide your final answer as text after using tools.\n\n"
        "## Memory\n"
        "You have persistent memory stored on the Linux filesystem:\n"
        "- Long-term memory: %s\n"
        "- Daily notes: %s<YYYY-MM-DD>.md\n\n"
        "Actively use memory to remember things across conversations.\n"
        "When you learn something about the user, write it to MEMORY.md.\n"
        "Always read_file MEMORY.md before writing to avoid losing existing content.\n"
        "Use get_current_time to know today's date before writing daily notes.\n\n"
        "## Skills\n"
        "Skills are instruction files stored in %s.\n"
        "When a task matches a skill, read the full skill file for detailed instructions.\n",
        kc_get_data_dir(), kc_get_memory_file(),
        kc_get_memory_dir(), kc_get_skills_dir());

    /* 角色个性（如果有） */
    const char *role_personality = role_mgr_get_personality();
    if (role_personality && role_personality[0]) {
        off += snprintf(buf + off, size - off,
            "\n## Active Role: %s\n\n%s\n", role_mgr_get_active(), role_personality);
    }

    /* 人格和用户信息文件 */
    off = append_file(buf, size, off, kc_get_soul_file(), "Personality");
    off = append_file(buf, size, off, kc_get_user_file(), "User Info");

    /* 长期记忆 */
    char mem_buf[4096];
    if (memory_read_long_term(mem_buf, sizeof(mem_buf)) == KC_OK && mem_buf[0])
        off += snprintf(buf + off, size - off, "\n## Long-term Memory\n\n%s\n", mem_buf);

    /* 最近 3 天笔记 */
    char recent_buf[4096];
    if (memory_read_recent(recent_buf, sizeof(recent_buf), 3) == KC_OK && recent_buf[0])
        off += snprintf(buf + off, size - off, "\n## Recent Notes\n\n%s\n", recent_buf);

    /* 技能 */
    char skills_buf[2048];
    size_t skills_len = skill_loader_build_summary(skills_buf, sizeof(skills_buf));
    if (skills_len > 0)
        off += snprintf(buf + off, size - off,
            "\n## Available Skills\n\n%s\n", skills_buf);

    KC_LOGI(TAG, "system prompt built: %d bytes", (int)off);
    return KC_OK;
}
