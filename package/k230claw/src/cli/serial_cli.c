/*
 * serial_cli.c - stdin/stdout REPL + outbound 分发
 *
 * Phase 3: CLI 重构为 kc_channel_t 通道实现。
 *   - cli_channel_get() 返回通道实例，注册到 channel_mgr
 *   - outbound_dispatch 从 outbound 队列读取，通过 channel_mgr 路由到各通道
 *   - CLI 通道的 send_text 直接 printf 到 stdout
 *
 * 用户输入的文本作为消息推入 inbound 队列，由 Agent Loop 处理。
 * 特殊命令以 / 开头，直接在 CLI 线程处理。
 */

#include "serial_cli.h"
#include "../kc_config.h"
#include "../bus/message_bus.h"
#include "../memory/memory_store.h"
#include "../memory/session_mgr.h"
#include "../llm/llm_proxy.h"
#include "../channels/channel_mgr.h"
#include "../roles/role_mgr.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/wait.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <fcntl.h>
#include <errno.h>

#define TAG "cli"
#define CLI_BUF_SIZE 1024
#define CLI_CHAT_ID  "local"

/* /shell 期间暂停 CLI 输入线程 */
static volatile int s_shell_active = 0;

/* ── CLI 通道实现 ── */

static kc_err_t cli_send_text(kc_channel_t *self, const char *chat_id, const char *text)
{
    (void)self; (void)chat_id;
    printf("\n%s\n", text ? text : "");
    printf("k230claw> ");
    fflush(stdout);
    return KC_OK;
}

static kc_err_t cli_start(kc_channel_t *self);
static kc_err_t cli_stop_channel(kc_channel_t *self);

static kc_channel_t s_cli_channel = {
    .name       = KC_CHAN_CLI,
    .start      = cli_start,
    .stop       = cli_stop_channel,
    .send_text  = cli_send_text,
    .send_image = NULL,   /* 串口不支持图片 */
    .ctx        = NULL,
};

kc_channel_t *cli_channel_get(void)
{
    return &s_cli_channel;
}

/* ── outbound 消息分发线程 ── */

/*
 * 从 outbound 队列取消息，通过 channel_mgr 路由到对应通道。
 * 这是所有通道共用的分发机制，不专属于 CLI。
 */
static void *outbound_dispatch(void *arg)
{
    (void)arg;
    while (!kc_is_shutting_down()) {
        kc_msg_t msg;
        if (message_bus_pop_outbound(&msg, UINT32_MAX) != KC_OK) continue;
        if (kc_is_shutting_down()) { free(msg.content); break; }

        /* 通过通道管理器路由 */
        if (channel_mgr_send(msg.channel, msg.chat_id, msg.content) != KC_OK) {
            KC_LOGW(TAG, "failed to send to channel '%s'", msg.channel);
        }

        free(msg.content);
    }
    return NULL;
}

/* ── / 命令处理 ── */

static bool handle_command(const char *input) {
    if (strcmp(input, "/quit") == 0 || strcmp(input, "/exit") == 0) {
        printf("Bye!\n");
        raise(SIGINT);
        return true;
    }

    if (strcmp(input, "/shell") == 0) {
        KC_LOGI(TAG, "/shell: entering shell mode");
        printf("Entering Linux shell (type 'exit' to return)...\n");
        fflush(stdout);
        s_shell_active = 1;  /* 暂停 CLI 输入线程的 poll */
        usleep(100000);      /* 等待输入线程退出 poll */

        /* ── 调试：检查当前进程的终端信息 ── */
        {
            char linkbuf[256] = {0};
            ssize_t n = readlink("/proc/self/fd/0", linkbuf, sizeof(linkbuf)-1);
            KC_LOGI(TAG, "/shell: stdin fd0 -> %s (ret=%zd)", linkbuf, n);

            n = readlink("/proc/self/fd/1", linkbuf, sizeof(linkbuf)-1);
            KC_LOGI(TAG, "/shell: stdout fd1 -> %s (ret=%zd)", linkbuf, n);

            KC_LOGI(TAG, "/shell: pid=%d, ppid=%d, pgid=%d, sid=%d",
                    getpid(), getppid(), getpgid(0), getsid(0));

            /* 检查 /dev/ttyS0 是否可访问 */
            int testfd = open("/dev/ttyS0", O_RDWR | O_NONBLOCK);
            KC_LOGI(TAG, "/shell: open(/dev/ttyS0) = %d%s",
                    testfd, testfd < 0 ? strerror(errno) : " OK");
            if (testfd >= 0) {
                struct termios tio;
                int tr = tcgetattr(testfd, &tio);
                KC_LOGI(TAG, "/shell: tcgetattr(ttyS0) = %d (errno=%d %s)",
                        tr, errno, tr < 0 ? strerror(errno) : "OK");
                close(testfd);
            }

            /* 检查 /dev/tty 是否可访问 */
            testfd = open("/dev/tty", O_RDWR | O_NONBLOCK);
            KC_LOGI(TAG, "/shell: open(/dev/tty) = %d%s",
                    testfd, testfd < 0 ? strerror(errno) : " OK");
            if (testfd >= 0) close(testfd);

            /* 检查 /dev/console 是否可访问 */
            testfd = open("/dev/console", O_RDWR | O_NONBLOCK);
            KC_LOGI(TAG, "/shell: open(/dev/console) = %d%s",
                    testfd, testfd < 0 ? strerror(errno) : " OK");
            if (testfd >= 0) close(testfd);
        }

        pid_t pid = fork();
        KC_LOGI(TAG, "/shell: fork() = %d (errno=%d %s)",
                pid, pid < 0 ? errno : 0, pid < 0 ? strerror(errno) : "OK");

        if (pid == 0) {
            /* 子进程：调试日志用 _exit 前写 stderr */
            KC_LOGI(TAG, "/shell child: pid=%d, about to open tty", getpid());

            /* 尝试打开终端设备 */
            int fd = -1;
            const char *tty_path = NULL;

            /* 先尝试 /dev/tty（进程自己的控制终端） */
            fd = open("/dev/tty", O_RDWR);
            tty_path = "/dev/tty";
            KC_LOGI(TAG, "/shell child: open(/dev/tty) = %d", fd);

            if (fd < 0) {
                fd = open("/dev/ttyS0", O_RDWR);
                tty_path = "/dev/ttyS0";
                KC_LOGI(TAG, "/shell child: open(/dev/ttyS0) = %d%s",
                        fd, fd < 0 ? strerror(errno) : " OK");
            }
            if (fd < 0) {
                fd = open("/dev/console", O_RDWR);
                tty_path = "/dev/console";
                KC_LOGI(TAG, "/shell child: open(/dev/console) = %d%s",
                        fd, fd < 0 ? strerror(errno) : " OK");
            }

            if (fd >= 0) {
                KC_LOGI(TAG, "/shell child: using %s (fd=%d), dup2 to 0/1/2", tty_path, fd);

                /* 保存原始 stderr 用于最后一条日志 */
                int saved_stderr = dup(STDERR_FILENO);

                dup2(fd, STDIN_FILENO);
                dup2(fd, STDOUT_FILENO);
                dup2(fd, STDERR_FILENO);
                if (fd > 2) close(fd);

                /* 写最后一条日志到原始 stderr */
                dprintf(saved_stderr, "[cli] /shell child: dup2 done, calling execl(/bin/sh)\n");
                close(saved_stderr);
            } else {
                /* 所有终端设备都打不开 */
                fprintf(stderr, "[cli] /shell child: FATAL - no terminal device available\n");
            }

            setenv("HOME", "/root", 1);
            setenv("TERM", "vt100", 1);

            KC_LOGI(TAG, "/shell child: execl(/bin/sh -l) starting...");
            execl("/bin/sh", "-sh", NULL);  /* "-sh" = login shell, 加载 /etc/profile */
            /* execl 失败才会到这里 */
            fprintf(stderr, "[cli] /shell child: execl FAILED errno=%d (%s)\n",
                    errno, strerror(errno));
            _exit(127);
        } else if (pid > 0) {
            KC_LOGI(TAG, "/shell: parent waiting for child %d...", pid);

            /* 用 WNOHANG 循环等待，每次打印状态，便于定位卡死位置 */
            int status = 0;
            int waited = 0;
            while (1) {
                int ret = waitpid(pid, &status, WNOHANG);
                if (ret == pid) {
                    KC_LOGI(TAG, "/shell: child %d exited, status=%d (WIFEXITED=%d, WEXITSTATUS=%d)",
                            pid, status, WIFEXITED(status),
                            WIFEXITED(status) ? WEXITSTATUS(status) : -1);
                    break;
                } else if (ret < 0) {
                    KC_LOGE(TAG, "/shell: waitpid error errno=%d (%s)", errno, strerror(errno));
                    break;
                }
                /* ret == 0: child still running */
                usleep(500000); /* 500ms */
                waited++;
                if (waited % 10 == 0) {
                    KC_LOGW(TAG, "/shell: child %d still running after %ds (killed=%d, stopped=%d, continued=%d)",
                            pid, waited / 2,
                            WIFSIGNALED(status), WIFSTOPPED(status), WIFCONTINUED(status));

                    /* 检查子进程状态 */
                    char proc_path[64];
                    snprintf(proc_path, sizeof(proc_path), "/proc/%d/status", pid);
                    FILE *pf = fopen(proc_path, "r");
                    if (pf) {
                        char line[128];
                        while (fgets(line, sizeof(line), pf)) {
                            if (strncmp(line, "State:", 6) == 0 ||
                                strncmp(line, "Tgid:", 5) == 0 ||
                                strncmp(line, "PPid:", 5) == 0) {
                                /* 去掉换行 */
                                size_t ln = strlen(line);
                                while (ln > 0 && (line[ln-1] == '\n' || line[ln-1] == '\r'))
                                    line[--ln] = '\0';
                                KC_LOGW(TAG, "/shell child proc: %s", line);
                            }
                        }
                        fclose(pf);
                    }
                }
                if (waited >= 600) { /* 5 分钟超时 */
                    KC_LOGE(TAG, "/shell: child %d timeout 5min, sending SIGTERM", pid);
                    kill(pid, SIGTERM);
                    usleep(100000);
                    waitpid(pid, &status, WNOHANG);
                    break;
                }
            }
        } else {
            KC_LOGE(TAG, "/shell: fork() failed errno=%d (%s)", errno, strerror(errno));
            printf("Failed to start shell.\n");
        }

        s_shell_active = 0;  /* 恢复 CLI 输入线程 */
        KC_LOGI(TAG, "/shell: done, returning to CLI");
        printf("\n=== Back to K230Claw ===\n");
        return true;
    }

    if (strcmp(input, "/reconnect") == 0) {
        printf("Checking network...\n");
        kc_check_network();
        printf("Status: %s\n", kc_is_online() ? "Online" : "Offline");
        return true;
    }

    if (strncmp(input, "/wifi ", 6) == 0) {
        char ssid[64] = {0}, pass[64] = {0};
        if (sscanf(input + 6, "%63s %63s", ssid, pass) == 2) {
            printf("Connecting to WiFi '%s'...\n", ssid);
            pid_t pid = fork();
            if (pid == 0) {
                char *args[] = {"sta.sh", "wlan0", ssid, pass, NULL};
                execvp("sta.sh", args);
                _exit(127);
            } else if (pid > 0) {
                int status;
                waitpid(pid, &status, 0);
            }
            sleep(3);
            kc_check_network();
            if (kc_is_online()) {
                printf("Status: Online\n");
                /* 持久化 WiFi 凭据到 U-Boot 环境变量，重启后自动连接 */
                pid_t p1 = fork();
                if (p1 == 0) {
                    char *a[] = {"fw_setenv", "wlanssid", ssid, NULL};
                    execvp("fw_setenv", a);
                    _exit(127);
                } else if (p1 > 0) { waitpid(p1, NULL, 0); }

                pid_t p2 = fork();
                if (p2 == 0) {
                    char *a[] = {"fw_setenv", "wlanpass", pass, NULL};
                    execvp("fw_setenv", a);
                    _exit(127);
                } else if (p2 > 0) { waitpid(p2, NULL, 0); }

                printf("WiFi credentials saved (will reconnect on reboot).\n");
            } else {
                printf("Status: Offline (credentials not saved)\n");
            }
        } else {
            printf("Usage: /wifi <SSID> <PASSWORD>\n");
        }
        return true;
    }

    if (strcmp(input, "/clearhis") == 0) {
        session_clear(CLI_CHAT_ID);
        printf("Context cleared.\n");
        return true;
    }

    if (strcmp(input, "/memory") == 0) {
        char buf[4096];
        if (memory_read_long_term(buf, sizeof(buf)) == KC_OK && buf[0])
            printf("=== MEMORY.md ===\n%s\n=================\n", buf);
        else
            printf("MEMORY.md is empty.\n");
        return true;
    }

    if (strcmp(input, "/sessions") == 0) {
        session_list();
        return true;
    }

    if (strcmp(input, "/log") == 0) {
        FILE *logf = fopen("/var/log/k230claw.log", "r");
        if (!logf) logf = fopen("/tmp/k230claw.log", "r");
        if (!logf) { printf("No log file found.\n"); return true; }
        char *lines[50];
        int line_count = 0;
        char lbuf[512];
        memset(lines, 0, sizeof(lines));
        while (fgets(lbuf, sizeof(lbuf), logf)) {
            free(lines[line_count % 50]);
            lines[line_count % 50] = strdup(lbuf);
            line_count++;
        }
        fclose(logf);
        printf("=== Last %d log lines ===\n", line_count < 50 ? line_count : 50);
        int start = line_count < 50 ? 0 : line_count % 50;
        int show = line_count < 50 ? line_count : 50;
        for (int i = 0; i < show; i++) {
            int idx = (start + i) % 50;
            if (lines[idx]) printf("%s", lines[idx]);
        }
        for (int i = 0; i < 50; i++) free(lines[i]);
        printf("=========================\n");
        return true;
    }

    if (strcmp(input, "/web") == 0) {
        printf("Web UI: http://localhost:8080\n");
        return true;
    }

    if (strcmp(input, "/role") == 0) {
        char *list = role_mgr_list();
        if (list) { printf("%s", list); free(list); }
        return true;
    }
    if (strncmp(input, "/role ", 6) == 0) {
        const char *name = input + 6;
        while (*name == ' ') name++;
        if (*name) {
            kc_err_t err = role_mgr_switch(name);
            if (err == KC_OK)
                printf("Switched to role: %s\n", name);
            else
                printf("Role not found: %s (use /role to list)\n", name);
        }
        return true;
    }

    if (strcmp(input, "/help") == 0) {
        printf("Commands:\n"
               "  /wifi SSID PASS  - Connect to WiFi\n"
               "  /reconnect       - Retry network connection\n"
               "  /web             - Show Web UI address\n"
               "  /role            - List available roles\n"
               "  /role NAME       - Switch to a role\n"
               "  /shell           - Enter Linux shell\n"
               "  /clearhis        - Clear conversation context\n"
               "  /memory          - Show long-term memory\n"
               "  /sessions        - List all sessions\n"
               "  /log             - Show recent logs\n"
               "  /quit            - Exit K230Claw\n"
               "  /help            - Show this help\n"
               "  (other)          - Send message to AI\n");
        return true;
    }

    return false;
}

/* ── CLI 输入线程 ── */

static void *cli_input_thread(void *arg)
{
    (void)arg;
    char buf[CLI_BUF_SIZE];

    /* 等待其他服务启动完成，避免日志和提示符混在一起 */
    usleep(200000);  /* 200ms */

    printf("\n=== K230Claw AI Assistant ===\n");
    printf("Web UI: http://localhost:8080\n");
    if (kc_is_online())
        printf("Status: Online. Type your message or /help for commands.\n\n");
    else
        printf("Status: Offline. Use /wifi <SSID> <PASSWORD> to connect.\n\n");

    struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };

    while (!kc_is_shutting_down()) {
        /* /shell 期间暂停，让子 shell 独占 stdin */
        if (s_shell_active) {
            usleep(100000);  /* 100ms */
            continue;
        }

        printf("k230claw> ");
        fflush(stdout);

        int ready;
        do {
            if (s_shell_active) { ready = -1; break; }
            ready = poll(&pfd, 1, 1000);
        } while (ready == 0 && !kc_is_shutting_down() && !s_shell_active);

        if (ready <= 0) continue;

        if (fgets(buf, sizeof(buf), stdin) == NULL) break;

        size_t len = strlen(buf);
        while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r'))
            buf[--len] = '\0';

        if (len == 0) continue;

        if (buf[0] == '/' && handle_command(buf)) continue;

        if (kc_is_shutting_down()) break;

        if (!kc_is_online()) {
            printf("[Hint] Network may be unavailable. Trying anyway...\n"
                   "  Tip: /wifi <SSID> <PASSWORD> to connect, /reconnect to recheck.\n");
        }

        kc_msg_t msg = {0};
        strncpy(msg.channel, KC_CHAN_CLI, sizeof(msg.channel) - 1);
        strncpy(msg.chat_id, CLI_CHAT_ID, sizeof(msg.chat_id) - 1);
        msg.content = strdup(buf);
        if (!msg.content) { KC_LOGE(TAG, "out of memory"); continue; }

        if (message_bus_push_inbound(&msg) != KC_OK) {
            KC_LOGW(TAG, "inbound full");
            free(msg.content);
        }
    }

    return NULL;
}

/* ── 生命周期 ── */

static pthread_t s_cli_tid;
static pthread_t s_out_tid;

static kc_err_t cli_start(kc_channel_t *self)
{
    (void)self;
    pthread_create(&s_cli_tid, NULL, cli_input_thread, NULL);
    KC_LOGI(TAG, "CLI channel started");
    return KC_OK;
}

static kc_err_t cli_stop_channel(kc_channel_t *self)
{
    (void)self;
    KC_LOGI(TAG, "stopping CLI channel...");
    pthread_join(s_cli_tid, NULL);
    KC_LOGI(TAG, "CLI channel stopped");
    return KC_OK;
}

kc_err_t serial_cli_init(void)
{
    KC_LOGI(TAG, "CLI initialized");
    return KC_OK;
}

kc_err_t outbound_dispatch_start(void)
{
    pthread_create(&s_out_tid, NULL, outbound_dispatch, NULL);
    KC_LOGI(TAG, "outbound dispatch started");
    return KC_OK;
}

void serial_cli_stop(void)
{
    KC_LOGI(TAG, "stopping serial_cli...");
    /* CLI 通道线程由 channel_mgr_stop_all 停止 */
    /* 这里只等待 outbound dispatch */
    pthread_join(s_out_tid, NULL);
    KC_LOGI(TAG, "serial_cli stopped");
}
