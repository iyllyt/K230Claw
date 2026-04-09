#pragma once

/*
 * role_mgr.h - 角色管理器
 *
 * 角色定义了一组传感器、工具和个性配置。
 * 用户通过 /role CLI 命令或 switch_role LLM 工具切换角色。
 *
 * 角色文件: {data_dir}/roles/*.md
 * frontmatter 格式:
 *   ---
 *   name: 桌面搭子
 *   description: 桌面智能伴侣
 *   sensors: face_detector, voice_wake
 *   disabled_tools: camera_capture
 *   ---
 *   (角色个性描述，注入系统提示词)
 */

#include "../kc_hal.h"

/* 初始化角色管理器（扫描 roles/ 目录 + 应用保存的角色） */
kc_err_t role_mgr_init(void);

/* 切换角色（停旧传感器 → 更新工具 → 启新传感器 → 保存配置） */
kc_err_t role_mgr_switch(const char *name);

/* 返回当前角色名 */
const char *role_mgr_get_active(void);

/* 返回可用角色列表（malloc，调用者 free） */
char *role_mgr_list(void);

/* 返回当前角色个性文本（NULL = 无个性定义） */
const char *role_mgr_get_personality(void);
