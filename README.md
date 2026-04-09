# K230Claw

轻量级 AI 助手，专为 K230 双核 RISC-V SoC 定制。

K230Claw 基于系统底层（Linux 6.6 大核）直接运行，使用纯 C 开发，资源占用极低。不需要 Python、不需要容器、不需要应用层框架，一个二进制文件直接跑在 K230 上，充分利用硬件能力（摄像头、麦克风、KPU NPU）。

参考 [OpenClaw](https://github.com/openclaw)、[PicoClaw](https://github.com/sipeed/picoclaw)、[MimiClaw](https://github.com/jason-12138/mimiclaw) 等项目的设计理念，针对 K230 硬件做了深度定制。

## 功能特性

- **LLM 对话**：支持 OpenAI / Anthropic / DeepSeek 等兼容 API，工具调用（ReAct Agent Loop）
- **Web 聊天界面**：浏览器访问 `http://<K230-IP>:8080`，支持设置面板和模型预设切换
- **CLI 串口终端**：串口直接交互，支持 `/wifi`、`/shell`、`/role` 等命令
- **硬件集成**：摄像头拍照（V4L2）、本地语音合成（KPU TTS）、语音唤醒（KPU KWS）、人脸识别（KPU）
- **角色系统**：通过 `.md` 角色文件定义个性、启用/禁用传感器和工具
- **定时任务**：Cron 引擎，支持一次性和周期性任务
- **技能系统**：Markdown frontmatter 技能文件，关键词触发

## 硬件要求

| 项目 | 要求 |
|------|------|
| 开发板 | 庐山派 K230（立创，1GB LPDDR4）|
| WiFi | RTL8189FTV（板载）|
| 摄像头 | GC2093（MIPI CSI）|
| SDK | [k230_linux_sdk](https://github.com/kendryte/k230_linux_sdk) dev 分支 |

## 快速开始

### 1. 克隆 SDK

```bash
git clone -b dev https://github.com/kendryte/k230_linux_sdk.git
cd k230_linux_sdk
```

### 2. 将 K230Claw 集成到 SDK

```bash
git clone https://github.com/iyllyt/K230Claw.git
cd K230Claw

# 复制源码（含 Buildroot 包定义：Config.in + k230claw.mk）
cp -r package/k230claw ../buildroot-overlay/package/

# 复制 board overlay（角色文件、启动脚本等）
cp -r board/* ../buildroot-overlay/board/

# 复制 defconfig
cp configs/* ../buildroot-overlay/configs/
```

### 3. 处理 CRLF（Windows→Linux 必做）

```bash
cd ~/k230_linux_sdk
find buildroot-overlay/package/k230claw/src -type f \
  \( -name '*.c' -o -name '*.cpp' -o -name '*.h' -o -name '*.cc' \) \
  -exec dos2unix {} \;
dos2unix buildroot-overlay/configs/k230_canmv_lckfb_defconfig
```

### 4. 编译

```bash
cd ~/k230_linux_sdk
sudo make toolchain_and_depend          # 下载工具链和依赖（首次）
make CONF=k230_canmv_lckfb_defconfig    # 完整编译（首次 30-60 分钟）
# 输出: output/k230_canmv_lckfb_defconfig/images/sysimage-sdcard.img.gz
```

单独重编译 k230claw 包（修改代码后快速编译）：
```bash
make CONF=k230_canmv_lckfb_defconfig k230claw-rebuild
make CONF=k230_canmv_lckfb_defconfig    # 重新生成镜像
```

### 5. 烧录

```bash
gunzip output/k230_canmv_lckfb_defconfig/images/sysimage-sdcard.img.gz
sudo dd if=output/k230_canmv_lckfb_defconfig/images/sysimage-sdcard.img of=/dev/sdX bs=4M status=progress
sync
```

### 6. 启动使用

1. 插入 SD 卡，上电启动 K230
2. 串口连接（115200 baud），K230Claw 会自动启动
3. 输入 `/wifi <SSID> <密码>` 连接 WiFi
4. 直接输入文字和 AI 对话
5. 浏览器访问 `http://<K230-IP>:8080` 使用 Web 聊天界面

### 7. 主机 x86 编译（可选，快速验证逻辑）

```bash
cd package/k230claw/src
make CC=gcc EXTRA_CFLAGS="-I/usr/include" \
     EXTRA_LDFLAGS="-lcurl -lssl -lcrypto -lpthread"
./k230claw -c /path/to/k230claw.conf
```

## 角色系统

K230Claw 通过角色系统实现物联网场景的资源管理。每个角色定义三件事：**启用哪些传感器**、**禁用哪些工具**、**AI 的行为个性**。切换角色后，系统自动启停对应的硬件资源，AI 的行为模式也随之改变。

### 预置角色

| 角色 | 传感器 | 禁用工具 | 场景 |
|------|--------|----------|------|
| **default** | 无 | 无 | 开机默认模式。所有工具可用，无后台传感器，纯粹的 AI 对话助手 |
| **desktop_buddy** | face_detector, voice_wake | camera_capture | 桌面智能伴侣。检测到主人靠近时主动打招呼，语音唤醒后响应，主人久未说话时偶尔关心一下 |
| **watchdog** | face_detector | 无 | 安全看门狗。后台持续识别人脸，检测到陌生人时立即告警 |
| **navigator** | video_stream | record_audio, speak_text | 航行智能终端。YOLOv8n 多目标检测 + 实时视频流，浏览器访问 `/stream.html` 查看实时画面 |

### 使用方式

```
k230claw> /role
Available roles (* = active):
  * default — Default mode: all tools, no sensors
    desktop_buddy — Desktop companion, proactively interacts with owner
    watchdog — Security monitor, alerts on strangers
    navigator — Real-time video stream with YOLOv8n at /stream.html

k230claw> /role watchdog
Switched to role: watchdog
```

## 目录结构

```
K230Claw/
├── package/k230claw/     # Buildroot 包（源码 + 包定义）
│   ├── src/              # C/C++ 源码
│   ├── Config.in         # Buildroot 包配置
│   └── k230claw.mk       # Buildroot 构建脚本
├── board/                # K230 板级 overlay（角色文件、启动脚本等）
├── configs/              # defconfig
├── DEV_PROGRESS.md       # 开发进度记录
├── TEST_PLAN.md          # 真机测试文档
└── README.md
```

## 工具列表

| 工具 | 描述 | 平台 |
|------|------|------|
| get_current_time | 获取当前时间 | 全平台 |
| read_file | 读取文件 | 全平台 |
| write_file | 写入文件 | 全平台 |
| list_dir | 列出目录 | 全平台 |
| run_command | 执行 shell 命令（白名单+确认） | 全平台 |
| schedule_task | 创建定时任务 | 全平台 |
| list_tasks | 列出定时任务 | 全平台 |
| remove_task | 删除定时任务 | 全平台 |
| web_search | DuckDuckGo 网页搜索 | 全平台 |
| web_fetch | 获取网页内容 | 全平台 |
| camera_capture | 拍照 | K230 |
| record_audio | 录音转文字（Whisper API）| K230 |
| speak_text | 本地 TTS 语音合成 | K230 |
| register_face | 注册人脸 | K230 |
| switch_role | 切换角色 | 全平台 |

## CLI 命令

```
/wifi SSID PASS  - 连接 WiFi
/reconnect       - 重新检查网络
/web             - 显示 Web UI 地址
/role            - 列出角色
/role NAME       - 切换角色
/shell           - 进入 Linux shell
/clearhis        - 清除对话上下文
/memory          - 显示长期记忆
/sessions        - 列出所有会话
/log             - 查看最近日志
/quit            - 退出
/help            - 帮助
```

## 技术特点

- **纯 C 开发**：无 Python/Node 依赖，单二进制部署，内存占用低
- **系统级运行**：直接跑在 Linux 大核上，不依赖应用层框架
- **硬件直通**：V4L2 摄像头、ALSA 音频、KPU NPU 全部原生调用
- **本地 TTS**：中文语音合成完全在 KPU 上运行，无需联网
- **安全设计**：Shell 白名单、路径遍历防护、API key 脱敏、输出重定向检查
- **多通道**：CLI 串口 + WebSocket Web 界面，后续可扩展 Telegram/QQ 等

## 致谢

- [OpenClaw](https://github.com/openclaw) — AI 助手框架参考
- [PicoClaw](https://github.com/sipeed/picoclaw) — 架构参考
- [MimiClaw](https://github.com/jason-12138/mimiclaw) — 硬件集成参考
- [mongoose](https://github.com/cesanta/mongoose) — 嵌入式 HTTP/WebSocket 库
- [cJSON](https://github.com/DaveGamble/cJSON) — JSON 解析库
