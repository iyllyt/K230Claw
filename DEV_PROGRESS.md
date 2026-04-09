# K230Claw 开发进度

## 项目概述

K230Claw 是轻量级 AI 助手，专为 K230 双核 RISC-V SoC 定制。
大核 Linux 6.6（1.6GHz）运行 K230Claw 主程序，小核 NuttX 为辅助系统。

**目标板**：庐山派 K230（立创，1GB LPDDR4，RTL8189FTV WiFi，GC2093 摄像头）

---

## Phase 1：编译 → 启动 → 联网 → HTTPS → CLI 对话

**完成情况**：全部完成

## Phase 2：系统工具 + 代码加固

**完成情况**：全部完成

## Phase 3：框架完善

**完成情况**：全部完成

## Phase 4：硬件集成 — 桌面智能终端

**完成情况**：代码完成，K230 交叉编译完成，真机测试部分通过

## Phase 5：Navigator 角色 — YOLOv8 多目标检测 + MJPEG 视频流（2026-04-09）

新增"航行智能终端"角色。切换到该角色时自动启动 YOLOv8n 多目标检测 + 摄像头实时视频流。

### 待验证

- [ ] V4L2 串行化启动修复验证
- [ ] `/role navigator` → 浏览器看实时检测视频
- [ ] K230 交叉编译通过
- [ ] /shell login shell 提示符
- [ ] WiFi 持久化
