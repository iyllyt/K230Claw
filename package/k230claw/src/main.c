/*
 * main.c - K230Claw 入口点
 *
 * 对应 MimiClaw 的 mimi.c (app_main)
 * 按正确顺序初始化所有子系统，启动 Agent Loop 和 CLI
 */

#include "kc_hal.h"
#include "kc_config.h"
#include "bus/message_bus.h"
#include "memory/memory_store.h"
#include "memory/session_mgr.h"
#include "skills/skill_loader.h"
#include "llm/llm_proxy.h"
#include "tools/tool_registry.h"
#include "agent/agent_loop.h"
#include "channels/channel_mgr.h"
#include "sensors/sensor_mgr.h"
#include "roles/role_mgr.h"
#include "sensors/sensor_state.h"
#include "cron/cron_service.h"
#include "gateway/ws_server.h"
#include "cli/serial_cli.h"

#ifdef KC_HAS_K230_HW
#include "hal/kpu_wrapper.h"
#include "hal/tts/tts_engine.h"
#include "sensors/sensor_face.h"
#include "sensors/sensor_voice.h"
#include "sensors/sensor_stream.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#include <curl/curl.h>

#define TAG "main"

/* ── 网络状态 ── */
static int s_online = 0;

int kc_is_online(void) { return s_online; }

/* libcurl 写回调：丢弃响应体 */
static size_t discard_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    (void)ptr; (void)userdata;
    return size * nmemb;
}

void kc_check_network(void) {
    /*
     * 用 libcurl 发一个轻量 HEAD 请求到配置的 API URL，
     * 验证完整链路（DNS → 代理 → TLS 握手）。
     * 这样代理模式、DNS 异常但代理可用等场景都能正确判断。
     */
    const char *url = kc_config_get_str("openai_api_url", KC_OPENAI_API_URL);
    const char *provider = kc_config_get_str("provider", "openai");
    if (strcmp(provider, "anthropic") == 0)
        url = kc_config_get_str("anthropic_api_url", KC_ANTHROPIC_API_URL);

    const char *proxy = kc_config_get_str("proxy", "");

    CURL *curl = curl_easy_init();
    if (!curl) {
        s_online = 0;
        KC_LOGW(TAG, "network: curl_easy_init failed");
        return;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);           /* HEAD 请求 */
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_cb);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    if (proxy[0])
        curl_easy_setopt(curl, CURLOPT_PROXY, proxy);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    /*
     * 只要 TCP+TLS 建立成功就算 online（即使 API 返回 4xx/5xx）。
     * curl 返回 CURLE_OK 说明完整链路通了；
     * CURLE_HTTP_RETURNED_ERROR 等也表示链路可用。
     */
    s_online = (res == CURLE_OK ||
                res == CURLE_HTTP_RETURNED_ERROR);

    KC_LOGI(TAG, "network: %s (%s, %s)",
            s_online ? "online" : "offline", url,
            s_online ? "ok" : curl_easy_strerror(res));
}

/* 递归创建目录（类似 mkdir -p） */
static void mkdirs(const char *path) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

/* 确保目录存在 */
static void ensure_dirs(void) {
    const char *dirs[] = {
        kc_get_data_dir(),
        kc_get_memory_dir(),
        kc_get_session_dir(),
        kc_get_config_dir(),
        kc_get_skills_dir(),
        NULL
    };
    for (int i = 0; dirs[i]; i++) {
        struct stat st;
        if (stat(dirs[i], &st) != 0) {
            mkdirs(dirs[i]);
            /* 验证创建结果 */
            if (stat(dirs[i], &st) != 0) {
                KC_LOGE(TAG, "FATAL: cannot create directory: %s (check permissions)", dirs[i]);
            }
        }
    }
}

/* 信号处理：优雅退出（只设标志，不调任何非异步信号安全函数） */
static volatile sig_atomic_t s_running = 1;

int kc_is_shutting_down(void) { return !s_running; }

static void signal_handler(int sig) {
    (void)sig;
    s_running = 0;
}

int main(int argc, char *argv[]) {
    /* 解析命令行参数 */
    const char *config_path = KC_CONFIG_FILE;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            config_path = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: k230claw [-c config_path]\n");
            return 0;
        }
    }

    /* 0. 日志重定向到文件（stderr → 日志文件）
     * 优先 /var/log/k230claw.log（K230 真机），fallback 到 data_dir 旁 */
    {
        const char *log_paths[] = {
            "/var/log/k230claw.log",
            "/tmp/k230claw.log",
            NULL
        };
        for (int i = 0; log_paths[i]; i++) {
            FILE *logf = fopen(log_paths[i], "a");
            if (logf) {
                dup2(fileno(logf), STDERR_FILENO);
                fclose(logf);
                break;
            }
        }
    }

    KC_LOGI(TAG, "K230Claw starting...");

    /* 1. 加载配置 */
    kc_config_init(config_path);

    /* 2. 初始化运行时路径（从 data_dir 配置项） */
    kc_paths_init();

    /* 2.5 设置时区（一次性，线程安全） */
    {
        const char *tz = kc_config_get_str("timezone", KC_TIMEZONE);
        setenv("TZ", tz, 1);
        tzset();
        KC_LOGI(TAG, "timezone set to: %s", tz);
    }

    /* 3. 创建数据目录 */
    ensure_dirs();

    /* 4. 初始化子系统（顺序重要） */
    KC_ERROR_CHECK(message_bus_init());
    KC_ERROR_CHECK(memory_store_init());
    KC_ERROR_CHECK(skill_loader_init());
    KC_ERROR_CHECK(session_mgr_init());
    KC_ERROR_CHECK(llm_proxy_init());
    KC_ERROR_CHECK(cron_service_init());
    KC_ERROR_CHECK(tool_registry_init());

    /* 5. 网络探测 */
    kc_check_network();

    /* 5.5. 初始化传感器系统 */
    KC_ERROR_CHECK(sensor_state_init());
    KC_ERROR_CHECK(sensor_mgr_init());

#ifdef KC_HAS_K230_HW
    /* 5.6. 初始化 KPU 子系统 */
    KC_WARN_CHECK(kpu_init());

    /* 5.7. 初始化本地 TTS 引擎 */
    {
        const char *tts_dict = kc_config_get_str("tts_dict_dir",
            "/root/app/tts_zh/file/file");
        const char *tts_kmodel = kc_config_get_str("tts_kmodel_dir",
            "/root/app/tts_zh");
        kc_err_t tts_err = kc_tts_init(tts_dict, tts_kmodel);
        if (tts_err != KC_OK)
            KC_LOGW(TAG, "TTS init failed, speak_text will be unavailable");
    }

    /* 5.8. 注册硬件传感器 */
    {
        kc_sensor_t *face = face_detector_get();
        if (face) sensor_mgr_register(face);
        kc_sensor_t *voice = voice_wake_get();
        if (voice) sensor_mgr_register(voice);
        kc_sensor_t *stream = video_stream_get();
        if (stream) sensor_mgr_register(stream);
    }
#endif

    /* 6. 初始化通道管理器并注册通道 */
    KC_ERROR_CHECK(channel_mgr_init());
    KC_ERROR_CHECK(serial_cli_init());
    KC_ERROR_CHECK(channel_mgr_register(cli_channel_get()));
    KC_ERROR_CHECK(channel_mgr_register(ws_channel_get()));

    /* 7. 启动 Agent Loop（后台线程） */
    KC_ERROR_CHECK(agent_loop_init());
    KC_ERROR_CHECK(agent_loop_start());

    /* 8. 启动所有通道 + outbound 分发 + 角色系统（按需启动传感器） */
    KC_ERROR_CHECK(channel_mgr_start_all());
    KC_ERROR_CHECK(outbound_dispatch_start());
    KC_ERROR_CHECK(role_mgr_init());  /* 加载角色定义 + 应用保存的角色 */
    KC_ERROR_CHECK(cron_service_start());

    KC_LOGI(TAG, "K230Claw ready.");

    /* 9. 主线程等待退出信号 */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    while (s_running) {
        sleep(1);
    }

    /* 有序关停 */
    KC_LOGI(TAG, "shutting down...");
    cron_service_stop();     /* 先停 cron */
    sensor_mgr_stop_all();   /* 停传感器（含人脸检测和语音唤醒） */
    serial_cli_stop();       /* 停止 outbound dispatch */
    channel_mgr_stop_all();  /* 停止所有通道（含 CLI 输入线程） */
    agent_loop_stop();
#ifdef KC_HAS_K230_HW
    kc_tts_deinit();
    kpu_deinit();
#endif
    kc_config_save();
    curl_global_cleanup();

    KC_LOGI(TAG, "K230Claw stopped.");
    return 0;
}
