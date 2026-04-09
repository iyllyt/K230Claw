# K230Claw 真机测试文档

**测试日期**：2026-04-09
**测试内容**：Phase 4 bug fix 回归 + Phase 5 Navigator YOLOv8 新功能

---

## 一、基础功能

| # | 测试项 | 操作 | 预期 | 结果 |
|---|--------|------|------|------|
| 1.1 | 系统启动 | 上电 | K230Claw 自动启动 | |
| 1.2 | WiFi 连接 | `/wifi <SSID> <密码>` | Status: Online | |
| 1.3 | Web UI | 浏览器 `http://<IP>:8080` | 聊天页面正常 | |
| 1.4 | CLI 对话 | 串口输入"你好" | LLM 回复 | |

## 二、Phase 4 Bug Fix 回归

| # | Bug | 操作 | 预期 | 结果 |
|---|-----|------|------|------|
| 2.1 | /shell 卡死 | `/shell` → `ls` → `exit` | 正常工作 | |
| 2.2 | TTS 初始化 | `/log` | 无 TTS init failed | |
| 2.3 | 语音唤醒误触发 | `/role desktop_buddy` 安静 30 秒 | 不频繁触发 | |
| 2.4 | 传感器事件丢失 | `/role watchdog`，出现在摄像头前 | AI 回复不丢失 | |
| 2.5 | SSH 登录 | `ssh root@<IP>` | 能连接 | |

## 三、Phase 5 Navigator

| # | 操作 | 预期 | 结果 |
|---|------|------|------|
| 3.1 | `/role` | 列出 navigator | |
| 3.2 | `/role navigator` | 切换成功，/log 有 YOLOv8n init | |
| 3.3 | 浏览器 `http://<IP>:8080/stream.html` | 实时视频 + 检测框 | |
| 3.4 | `/stream_status` | 返回 JSON（detections, top_class, fps） | |
| 3.5 | 两个浏览器同时打开 | 都正常显示 | |
| 3.6 | `/role default` | 流媒体停止 | |
| 3.7 | `/role watchdog` | face_detector 正常 | |

## 四、问题记录

| # | 现象 | 复现步骤 | 备注 |
|---|------|----------|------|
| | | | |
