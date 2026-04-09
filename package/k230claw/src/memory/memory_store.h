#pragma once

/*
 * memory_store.h - 持久化长期记忆和每日笔记
 * 从 MimiClaw 移植，仅改路径常量
 */

#include "../kc_hal.h"
#include <stddef.h>

kc_err_t memory_store_init(void);
kc_err_t memory_read_long_term(char *buf, size_t size);
kc_err_t memory_write_long_term(const char *content);
kc_err_t memory_append_today(const char *note);
kc_err_t memory_read_recent(char *buf, size_t size, int days);
