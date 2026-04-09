#pragma once

/*
 * kpu_wrapper.h - nncase KPU C API 封装
 *
 * 将 nncase C++ API 封装为纯 C 接口。
 * 供人脸检测传感器、KWS 传感器和 TTS 引擎使用。
 *
 * nncase 是 C++ only 的，此头文件用 extern "C" 包装。
 */

#ifdef __cplusplus
extern "C" {
#endif

#include "../kc_hal.h"
#include <stddef.h>
#include <stdint.h>

typedef struct kpu_model kpu_model_t;
typedef struct kpu_ai2d kpu_ai2d_t;

/* ── KPU 子系统 ── */

kc_err_t kpu_init(void);
void     kpu_deinit(void);

/* ── 模型生命周期 ── */

kc_err_t kpu_model_load(const char *kmodel_path, kpu_model_t **out);
void     kpu_model_free(kpu_model_t *m);

/* ── 模型信息 ── */

int      kpu_model_input_count(kpu_model_t *m);
int      kpu_model_output_count(kpu_model_t *m);

/* 获取输入/输出 shape（最多 4 维）*/
kc_err_t kpu_model_input_shape(kpu_model_t *m, int idx,
                                int shape[4], int *ndim);
kc_err_t kpu_model_output_shape(kpu_model_t *m, int idx,
                                 int shape[4], int *ndim);

/* ── 设置输入 ── */

/* 从 CPU 内存拷贝数据到输入 tensor */
kc_err_t kpu_model_set_input(kpu_model_t *m, int idx,
                              const void *data, size_t len);

/* ── 推理 ── */

kc_err_t kpu_model_run(kpu_model_t *m);

/* ── 读取输出 ── */

/*
 * 获取输出数据指针（float*）。
 * 生命周期：到下次 run() 或 free() 为止。
 */
float   *kpu_model_output_data(kpu_model_t *m, int idx);

/* 获取输出元素数量 */
int      kpu_model_output_size(kpu_model_t *m, int idx);

/* ── ai2d 硬件加速预处理 ── */

/*
 * 创建 ai2d padding+resize 预处理器。
 * 输入: uint8 (src_ch, src_h, src_w) → 输出: uint8 (src_ch, dst_h, dst_w)
 * padding 用指定颜色填充（pad_r/g/b）。
 */
kc_err_t kpu_ai2d_create_resize(kpu_ai2d_t **out,
    int src_ch, int src_h, int src_w,
    int dst_h, int dst_w,
    int pad_r, int pad_g, int pad_b);

/*
 * 运行 ai2d 预处理，输出直接设为模型的 input[input_idx]。
 * data: 源图像数据（uint8, CHW 排列）
 * data_len: 数据字节数
 */
kc_err_t kpu_ai2d_run(kpu_ai2d_t *ai2d, kpu_model_t *model, int input_idx,
                       const void *data, size_t data_len);

void     kpu_ai2d_free(kpu_ai2d_t *ai2d);

/*
 * 仿射变换预处理 + 设为模型输入。
 * 每次调用重建 ai2d pipeline（仿射矩阵因人脸位置不同而变化）。
 * affine_matrix: 2x3 仿射矩阵 [a00,a01,a02, a10,a11,a12]
 * 输入: uint8 NCHW (src_ch, src_h, src_w)
 * 输出: uint8 NCHW (src_ch, dst_h, dst_w)
 */
kc_err_t kpu_ai2d_run_affine(kpu_model_t *model, int input_idx,
    const void *buf_data, size_t buf_size,
    int src_ch, int src_h, int src_w,
    int dst_h, int dst_w,
    const float affine_matrix[6]);

#ifdef __cplusplus
}
#endif
