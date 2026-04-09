/*
 * tool_shell.c - 终端命令执行工具（Phase 2 安全加固版）
 *
 * 安全机制:
 *   1. 白名单：安全命令前缀直接执行
 *   2. 非白名单命令：返回确认提示，LLM 需带 confirmed:true 再次调用
 *   3. 输出重定向检查：> 和 >> 目标必须在 data_dir 内
 *   4. fork/exec/waitpid 三阶段终止（替代 popen+timeout）
 *   5. 输出截断 4KB
 */

#include "tool_shell.h"
#include "../kc_config.h"
#include "../third_party/cJSON/cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>

#define TAG "tool_shell"
#define MAX_OUTPUT_SIZE  4096
#define TIMEOUT_SECONDS  5
#define GRACE_SECONDS    2

/* ── 白名单：这些命令前缀允许直接执行 ── */

static const char *s_whitelist[] = {
    "ls",       "cat",      "head",     "tail",     "wc",
    "df",       "du",       "free",     "uptime",   "uname",
    "ps",       "top",      "date",     "cal",
    "ifconfig", "ping",     "iwconfig", "route",
    "grep",     "find",     "which",    "file",     "stat",
    "dmesg",    "mount",    "lsmod",    "id",       "whoami",
    "hostname", "env",      "printenv", "pwd",
    "echo",
    NULL
};

/* 提取命令的第一个 token（去除路径前缀） */
static const char *extract_cmd_name(const char *cmd, char *buf, size_t buf_size)
{
    while (*cmd == ' ' || *cmd == '\t') cmd++;

    size_t i = 0;
    while (cmd[i] && cmd[i] != ' ' && cmd[i] != '\t' &&
           cmd[i] != '|' && cmd[i] != ';' && cmd[i] != '&' &&
           cmd[i] != '\n' && i < buf_size - 1) {
        buf[i] = cmd[i];
        i++;
    }
    buf[i] = '\0';

    const char *base = strrchr(buf, '/');
    return base ? base + 1 : buf;
}

static int is_whitelisted(const char *cmd)
{
    char first_token[128];
    const char *name = extract_cmd_name(cmd, first_token, sizeof(first_token));

    for (int i = 0; s_whitelist[i]; i++) {
        if (strcmp(name, s_whitelist[i]) == 0)
            return 1;
    }
    return 0;
}

/* ── 输出重定向目标路径检查 ── */

static int has_unsafe_redirect(const char *cmd)
{
    const char *data_dir = kc_get_data_dir();
    size_t data_dir_len = strlen(data_dir);
    const char *p = cmd;

    while ((p = strchr(p, '>')) != NULL) {
        p++;
        if (*p == '>') p++;

        while (*p == ' ' || *p == '\t') p++;

        if (*p == '\0') return 1;

        if (*p == '/') {
            if (strncmp(p, data_dir, data_dir_len) != 0 ||
                (p[data_dir_len] != '/' && p[data_dir_len] != '\0' &&
                 p[data_dir_len] != ' ' && p[data_dir_len] != '\t')) {
                return 1;
            }
        }
    }
    return 0;
}

/* ── fork/exec/waitpid 三阶段终止 ── */

static int exec_with_timeout(const char *cmd, char *output, size_t output_size,
                             int timeout_sec)
{
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        snprintf(output, output_size, "Error: pipe() failed: %s", strerror(errno));
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        snprintf(output, output_size, "Error: fork() failed: %s", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        setsid();
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }

    close(pipefd[1]);

    int flags = fcntl(pipefd[0], F_GETFL, 0);
    fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

    size_t total = 0;
    size_t max_read = output_size - 1;
    if (max_read > MAX_OUTPUT_SIZE) max_read = MAX_OUTPUT_SIZE;

    int timed_out = 0;
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    while (1) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (now.tv_sec - start.tv_sec) +
                         (now.tv_nsec - start.tv_nsec) / 1e9;

        if (elapsed >= timeout_sec) {
            timed_out = 1;
            break;
        }

        if (total < max_read) {
            ssize_t n = read(pipefd[0], output + total, max_read - total);
            if (n > 0) {
                total += n;
                continue;
            }
            if (n == 0) break;
        }

        int status;
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) {
            while (total < max_read) {
                ssize_t n = read(pipefd[0], output + total, max_read - total);
                if (n <= 0) break;
                total += n;
            }
            close(pipefd[0]);
            output[total] = '\0';
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }

        usleep(50000);
    }

    output[total] = '\0';
    close(pipefd[0]);

    if (timed_out) {
        kill(-pid, SIGTERM);

        struct timespec grace_start;
        clock_gettime(CLOCK_MONOTONIC, &grace_start);
        while (1) {
            int status;
            pid_t w = waitpid(pid, &status, WNOHANG);
            if (w == pid) goto done_timeout;

            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double grace_elapsed = (now.tv_sec - grace_start.tv_sec) +
                                   (now.tv_nsec - grace_start.tv_nsec) / 1e9;
            if (grace_elapsed >= GRACE_SECONDS) break;
            usleep(100000);
        }

        kill(-pid, SIGKILL);
        waitpid(pid, NULL, 0);

    done_timeout:;
        size_t off = strlen(output);
        snprintf(output + off, output_size - off,
                 "\n[Command timed out after %d seconds]", timeout_sec);
    }

    return timed_out ? 124 : -1;
}

/* ── 公共 API ── */

kc_tool_result_t *tool_shell_execute(const char *input_json)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) return tool_result_error("Invalid JSON input");

    const char *cmd = cJSON_GetStringValue(cJSON_GetObjectItem(root, "command"));
    if (!cmd || !cmd[0]) {
        cJSON_Delete(root);
        return tool_result_error("Missing 'command' parameter");
    }

    /* 检查输出重定向安全性 */
    if (has_unsafe_redirect(cmd)) {
        KC_LOGW(TAG, "blocked unsafe redirect: %s", cmd);
        cJSON_Delete(root);
        return tool_result_error("Output redirection to paths outside data directory is not allowed.");
    }

    /* 白名单检查 */
    if (!is_whitelisted(cmd)) {
        cJSON *confirmed = cJSON_GetObjectItem(root, "confirmed");
        if (!confirmed || !cJSON_IsTrue(confirmed)) {
            KC_LOGI(TAG, "non-whitelisted command needs confirmation: %s", cmd);
            cJSON_Delete(root);
            return tool_result_text(
                "This command is not in the safety whitelist. "
                "Please tell the user what this command does and ask for their permission. "
                "If the user approves, call run_command again with the same command "
                "and add \"confirmed\": true to the parameters.");
        }
        KC_LOGI(TAG, "user confirmed non-whitelisted command: %s", cmd);
    }

    KC_LOGI(TAG, "run_command: %s", cmd);

    char output[MAX_OUTPUT_SIZE + 256];
    exec_with_timeout(cmd, output, sizeof(output), TIMEOUT_SECONDS);

    KC_LOGI(TAG, "run_command done: %d bytes output", (int)strlen(output));
    cJSON_Delete(root);
    return tool_result_text(output);
}
