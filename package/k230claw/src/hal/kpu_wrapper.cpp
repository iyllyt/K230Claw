/*
 * kpu_wrapper.cpp - nncase KPU C++ → C 封装
 *
 * 仅在 KC_HAS_K230_HW 下编译（Makefile 条件控制）。
 * 使用 nncase runtime C++ API：
 *   - interpreter: 模型加载/推理
 *   - host_runtime_tensor: 输入/输出 tensor
 *   - ai2d_builder: 硬件加速预处理
 */

#include "kpu_wrapper.h"

#include <nncase/runtime/interpreter.h>
#include <nncase/runtime/runtime_op_utility.h>
#include <nncase/runtime/util.h>
#include <nncase/functional/ai2d/ai2d_builder.h>

#include <fstream>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <cmath>

using namespace nncase;
using namespace nncase::runtime;
using namespace nncase::runtime::detail;
using namespace nncase::runtime::k230;
using namespace nncase::F::k230;

#define TAG "kpu"

/* ── 内部结构体 ── */

struct kpu_model {
    interpreter interp;
    int num_inputs;
    int num_outputs;
    std::vector<std::vector<int>> input_shapes;
    std::vector<std::vector<int>> output_shapes;
    std::vector<int> output_sizes;  /* 每个输出的元素数 */
};

struct kpu_ai2d {
    std::unique_ptr<ai2d_builder> builder;
    runtime_tensor in_tensor;
    runtime_tensor out_tensor;
    dims_t in_shape;
    dims_t out_shape;
    size_t in_size;
    bool built;
};

/* ── KPU 子系统 ── */

extern "C" {

kc_err_t kpu_init(void)
{
    KC_LOGI(TAG, "KPU subsystem initialized");
    return KC_OK;
}

void kpu_deinit(void)
{
    KC_LOGI(TAG, "KPU subsystem deinitialized");
}

/* ── 模型加载 ── */

kc_err_t kpu_model_load(const char *kmodel_path, kpu_model_t **out)
{
    if (!kmodel_path || !out) return KC_ERR_INVALID;

    std::ifstream ifs(kmodel_path, std::ios::binary);
    if (!ifs.is_open()) {
        KC_LOGE(TAG, "cannot open kmodel: %s", kmodel_path);
        return KC_ERR_NOT_FOUND;
    }

    kpu_model_t *m = new (std::nothrow) kpu_model_t();
    if (!m) return KC_ERR_NO_MEM;

    try {
        m->interp.load_model(ifs).expect("Invalid kmodel");
    } catch (...) {
        KC_LOGE(TAG, "failed to load kmodel: %s", kmodel_path);
        delete m;
        return KC_FAIL;
    }

    m->num_inputs = m->interp.inputs_size();
    m->num_outputs = m->interp.outputs_size();

    /* 初始化输入 tensor */
    for (int i = 0; i < m->num_inputs; i++) {
        auto desc = m->interp.input_desc(i);
        auto shape = m->interp.input_shape(i);

        auto tensor = host_runtime_tensor::create(
            desc.datatype, shape, hrt::pool_shared)
            .expect("cannot create input tensor");
        m->interp.input_tensor(i, tensor)
            .expect("cannot set input tensor");

        std::vector<int> s;
        for (auto d : shape) s.push_back((int)d);
        m->input_shapes.push_back(s);
    }

    /* 初始化输出 tensor */
    for (int i = 0; i < m->num_outputs; i++) {
        auto desc = m->interp.output_desc(i);
        auto shape = m->interp.output_shape(i);

        auto tensor = host_runtime_tensor::create(
            desc.datatype, shape, hrt::pool_shared)
            .expect("cannot create output tensor");
        m->interp.output_tensor(i, tensor)
            .expect("cannot set output tensor");

        std::vector<int> s;
        int total = 1;
        for (auto d : shape) {
            s.push_back((int)d);
            total *= (int)d;
        }
        m->output_shapes.push_back(s);
        m->output_sizes.push_back(total);
    }

    KC_LOGI(TAG, "loaded kmodel: %s (%d inputs, %d outputs)",
            kmodel_path, m->num_inputs, m->num_outputs);

    *out = m;
    return KC_OK;
}

void kpu_model_free(kpu_model_t *m)
{
    if (m) {
        delete m;
        KC_LOGI(TAG, "model freed");
    }
}

/* ── 模型信息 ── */

int kpu_model_input_count(kpu_model_t *m)
{
    return m ? m->num_inputs : 0;
}

int kpu_model_output_count(kpu_model_t *m)
{
    return m ? m->num_outputs : 0;
}

kc_err_t kpu_model_input_shape(kpu_model_t *m, int idx,
                                int shape[4], int *ndim)
{
    if (!m || idx < 0 || idx >= m->num_inputs) return KC_ERR_INVALID;
    const auto &s = m->input_shapes[idx];
    if (ndim) *ndim = (int)s.size();
    for (int i = 0; i < (int)s.size() && i < 4; i++)
        shape[i] = s[i];
    return KC_OK;
}

kc_err_t kpu_model_output_shape(kpu_model_t *m, int idx,
                                 int shape[4], int *ndim)
{
    if (!m || idx < 0 || idx >= m->num_outputs) return KC_ERR_INVALID;
    const auto &s = m->output_shapes[idx];
    if (ndim) *ndim = (int)s.size();
    for (int i = 0; i < (int)s.size() && i < 4; i++)
        shape[i] = s[i];
    return KC_OK;
}

/* ── 设置输入 ── */

kc_err_t kpu_model_set_input(kpu_model_t *m, int idx,
                              const void *data, size_t len)
{
    if (!m || idx < 0 || idx >= m->num_inputs || !data)
        return KC_ERR_INVALID;

    try {
        auto tensor = m->interp.input_tensor(idx)
            .expect("cannot get input tensor");

        auto buf = tensor.impl()->to_host().unwrap()
            ->buffer().as_host().unwrap()
            .map(map_access_::map_write).unwrap().buffer();

        size_t buf_size = buf.size_bytes();
        size_t copy_len = len < buf_size ? len : buf_size;
        memcpy(reinterpret_cast<char *>(buf.data()), data, copy_len);

        hrt::sync(tensor, sync_op_t::sync_write_back, true)
            .expect("sync write_back failed");
    } catch (...) {
        KC_LOGE(TAG, "set_input[%d] failed", idx);
        return KC_FAIL;
    }

    return KC_OK;
}

/* ── 推理 ── */

kc_err_t kpu_model_run(kpu_model_t *m)
{
    if (!m) return KC_ERR_INVALID;

    try {
        m->interp.run().expect("kpu run failed");
    } catch (...) {
        KC_LOGE(TAG, "inference failed");
        return KC_FAIL;
    }

    return KC_OK;
}

/* ── 读取输出 ── */

float *kpu_model_output_data(kpu_model_t *m, int idx)
{
    if (!m || idx < 0 || idx >= m->num_outputs) return NULL;

    try {
        auto out = m->interp.output_tensor(idx)
            .expect("cannot get output tensor");
        auto buf = out.impl()->to_host().unwrap()
            ->buffer().as_host().unwrap()
            .map(map_access_::map_read).unwrap().buffer();
        return reinterpret_cast<float *>(buf.data());
    } catch (...) {
        KC_LOGE(TAG, "output_data[%d] failed", idx);
        return NULL;
    }
}

int kpu_model_output_size(kpu_model_t *m, int idx)
{
    if (!m || idx < 0 || idx >= m->num_outputs) return 0;
    return m->output_sizes[idx];
}

/* ── ai2d 预处理 ── */

kc_err_t kpu_ai2d_create_resize(kpu_ai2d_t **out,
    int src_ch, int src_h, int src_w,
    int dst_h, int dst_w,
    int pad_r, int pad_g, int pad_b)
{
    if (!out) return KC_ERR_INVALID;

    kpu_ai2d_t *a = new (std::nothrow) kpu_ai2d_t();
    if (!a) return KC_ERR_NO_MEM;

    a->in_shape = {1, (size_t)src_ch, (size_t)src_h, (size_t)src_w};
    a->out_shape = {1, (size_t)src_ch, (size_t)dst_h, (size_t)dst_w};
    a->in_size = src_ch * src_h * src_w;
    a->built = false;

    try {
        /* 计算 letterbox padding（one_side: 右下填充） */
        float ratio_h = (float)dst_h / src_h;
        float ratio_w = (float)dst_w / src_w;
        float ratio = ratio_h < ratio_w ? ratio_h : ratio_w;
        int new_h = (int)(ratio * src_h);
        int new_w = (int)(ratio * src_w);
        int pad_bottom = dst_h - new_h;
        int pad_right = dst_w - new_w;

        /* ai2d 参数 */
        ai2d_datatype_t ai2d_dtype{
            ai2d_format::NCHW_FMT, ai2d_format::NCHW_FMT,
            typecode_t::dt_uint8, typecode_t::dt_uint8};

        ai2d_crop_param_t crop_param{false, 0, 0, 0, 0};
        ai2d_shift_param_t shift_param{false, 0};
        ai2d_pad_param_t pad_param{
            true,
            {{0, 0}, {0, 0},
             {0, (size_t)pad_bottom}, {0, (size_t)pad_right}},
            ai2d_pad_mode::constant,
            {pad_r, pad_g, pad_b}};
        ai2d_resize_param_t resize_param{
            true,
            ai2d_interp_method::tf_bilinear,
            ai2d_interp_mode::half_pixel};
        ai2d_affine_param_t affine_param{false};

        /* 创建输入/输出 tensor */
        a->in_tensor = host_runtime_tensor::create(
            typecode_t::dt_uint8, a->in_shape, hrt::pool_shared)
            .expect("create ai2d in tensor failed");

        a->out_tensor = host_runtime_tensor::create(
            typecode_t::dt_uint8, a->out_shape, hrt::pool_shared)
            .expect("create ai2d out tensor failed");

        /* 构建 ai2d */
        a->builder.reset(new ai2d_builder(
            a->in_shape, a->out_shape, ai2d_dtype,
            crop_param, shift_param, pad_param,
            resize_param, affine_param));
        a->builder->build_schedule();
        a->built = true;

    } catch (...) {
        KC_LOGE(TAG, "ai2d create failed");
        delete a;
        return KC_FAIL;
    }

    KC_LOGI(TAG, "ai2d created: (%d,%d,%d)→(%d,%d,%d)",
            src_ch, src_h, src_w, src_ch, dst_h, dst_w);

    *out = a;
    return KC_OK;
}

kc_err_t kpu_ai2d_run(kpu_ai2d_t *ai2d, kpu_model_t *model, int input_idx,
                       const void *data, size_t data_len)
{
    if (!ai2d || !model || !data || !ai2d->built)
        return KC_ERR_INVALID;

    try {
        /* 写入数据到 ai2d 输入 tensor */
        auto buf = ai2d->in_tensor.impl()->to_host().unwrap()
            ->buffer().as_host().unwrap()
            .map(map_access_::map_write).unwrap().buffer();

        size_t copy_len = data_len < buf.size_bytes() ?
                          data_len : buf.size_bytes();
        memcpy(reinterpret_cast<char *>(buf.data()), data, copy_len);

        hrt::sync(ai2d->in_tensor, sync_op_t::sync_write_back, true)
            .expect("ai2d sync write_back failed");

        /* 运行 ai2d */
        ai2d->builder->invoke(ai2d->in_tensor, ai2d->out_tensor)
            .expect("ai2d invoke failed");

        /* 将 ai2d 输出设为模型输入 */
        model->interp.input_tensor(input_idx, ai2d->out_tensor)
            .expect("cannot set model input from ai2d");

    } catch (...) {
        KC_LOGE(TAG, "ai2d run failed");
        return KC_FAIL;
    }

    return KC_OK;
}

void kpu_ai2d_free(kpu_ai2d_t *ai2d)
{
    if (ai2d) {
        delete ai2d;
        KC_LOGI(TAG, "ai2d freed");
    }
}

/* ── ai2d 仿射变换（per-frame 重建） ── */

kc_err_t kpu_ai2d_run_affine(kpu_model_t *model, int input_idx,
    const void *buf_data, size_t buf_size,
    int src_ch, int src_h, int src_w,
    int dst_h, int dst_w,
    const float affine_matrix[6])
{
    if (!model || !buf_data || !affine_matrix)
        return KC_ERR_INVALID;

    try {
        dims_t in_shape = {1, (size_t)src_ch, (size_t)src_h, (size_t)src_w};
        dims_t out_shape = {1, (size_t)src_ch, (size_t)dst_h, (size_t)dst_w};

        /* 创建临时输入 tensor */
        auto in_tensor = host_runtime_tensor::create(
            typecode_t::dt_uint8, in_shape, hrt::pool_shared)
            .expect("affine: create in tensor failed");

        /* 获取模型输入 tensor 作为输出（ai2d 结果直接写入模型输入） */
        auto out_tensor = host_runtime_tensor::create(
            typecode_t::dt_uint8, out_shape, hrt::pool_shared)
            .expect("affine: create out tensor failed");

        /* 写入源图像数据 */
        {
            auto buf = in_tensor.impl()->to_host().unwrap()
                ->buffer().as_host().unwrap()
                .map(map_access_::map_write).unwrap().buffer();
            size_t copy_len = buf_size < buf.size_bytes() ?
                              buf_size : buf.size_bytes();
            memcpy(reinterpret_cast<char *>(buf.data()), buf_data, copy_len);
            hrt::sync(in_tensor, sync_op_t::sync_write_back, true)
                .expect("affine: sync write_back failed");
        }

        /* ai2d 参数：仅启用 affine */
        ai2d_datatype_t ai2d_dtype{
            ai2d_format::NCHW_FMT, ai2d_format::NCHW_FMT,
            typecode_t::dt_uint8, typecode_t::dt_uint8};

        ai2d_crop_param_t crop_param{false, 0, 0, 0, 0};
        ai2d_shift_param_t shift_param{false, 0};
        ai2d_pad_param_t pad_param{false, {{0,0},{0,0},{0,0},{0,0}},
            ai2d_pad_mode::constant, {0, 0, 0}};
        ai2d_resize_param_t resize_param{false,
            ai2d_interp_method::tf_bilinear,
            ai2d_interp_mode::half_pixel};

        ai2d_affine_param_t affine_param{
            true,
            ai2d_interp_method::cv2_bilinear,
            0,      /* cord_round */
            0,      /* bound_ind */
            127,    /* bound_val (out-of-bounds fill) */
            1,      /* bound_smooth */
            {affine_matrix[0], affine_matrix[1], affine_matrix[2],
             affine_matrix[3], affine_matrix[4], affine_matrix[5]}};

        /* 构建并运行 ai2d */
        ai2d_builder builder(in_shape, out_shape, ai2d_dtype,
            crop_param, shift_param, pad_param,
            resize_param, affine_param);
        builder.build_schedule();
        builder.invoke(in_tensor, out_tensor)
            .expect("affine: invoke failed");

        /* 将 ai2d 输出设为模型输入 */
        model->interp.input_tensor(input_idx, out_tensor)
            .expect("affine: cannot set model input");

    } catch (...) {
        KC_LOGE(TAG, "ai2d affine run failed");
        return KC_FAIL;
    }

    return KC_OK;
}

} /* extern "C" */
