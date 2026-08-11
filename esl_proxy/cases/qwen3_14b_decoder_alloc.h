// Orchestration Function: qwen3_decode (dynamic tensormap, configurable-SPMD
// variant).
//
// Mirrors
// V200-benchmark/qwen3/qwen3_dynamic_tensormap/orchestration/qwen3_decode.cpp.
// Dependencies are discovered automatically via tensormap
// (tm_in/tm_out/tm_submit). SPMD tier is selected at compile time via
// QWEN3_SPMD_TIER (0=non-spmd .. 4=all-spmd).
//
// Durations are V200-benchmark per-subtask means (README.md §1.2.1 AICore View)
// in ns.
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "mem_pool.h"
#include "orch_config.h"
#include "tensormap.h"

Tensor g_tensors[MAX_TENSOR_NUM];

// Shared external tensors (initialized once by orchestrator_alloc, used by all desc threads)
Tensor ext_hidden_states;
Tensor ext_input_rms_weight;
Tensor ext_wq;
Tensor ext_wk;
Tensor ext_wv;
Tensor ext_q_norm_weight;
Tensor ext_k_norm_weight;
Tensor ext_seq_lens;
Tensor ext_block_table;
Tensor ext_slot_mapping;
Tensor ext_rope_cos;
Tensor ext_rope_sin;
Tensor ext_k_cache;
Tensor ext_v_cache;
Tensor ext_wo;
Tensor ext_post_rms_weight;
Tensor ext_w_gate;
Tensor ext_w_up;
Tensor ext_w_down;
Tensor ext_out;

static uint32_t alloc_task_id = 0;

static inline int qwen3_min_i(int a, int b) {
    return a < b ? a : b;
}

static inline int qwen3_blocks_per_task(int total_chunks) {
    static const int targets[5] = {1, 2, 4, 8, 1 << 30};
    int target = targets[QWEN3_SPMD_TIER];
    return qwen3_min_i(total_chunks, target);
}

int g_tensor_index = 0;


static inline int alloc_tensors_v2(uint32_t shape[], int dim, int bytes)
{
    size_t size = (size_t)shape[0] * (size_t)shape[1] * (size_t)dim * (size_t)bytes;
    uint64_t base = (uint64_t)(uintptr_t)mem_pool_alloc(&g_mem_pool, size);
    const uint32_t shapes[2] = {shape[0], shape[1]};
    g_tensors[g_tensor_index] = tensor_from_base_layout(base, shapes, 2, (dtype_t)bytes);
    g_tensor_index++;
    return (g_tensor_index - 1);
}

void orchestrator_alloc(const uint64_t orch_args) {
    uint64_t t_start = get_time_ns();

    // Initialize shared external tensors
    ext_hidden_states = tensor_from_base_layout(orch_args + 0, (uint32_t[]){90, 5120}, 2, BFLOAT16); // batch=90, hidden=5120
    ext_input_rms_weight = tensor_from_base_layout(orch_args + 1, (uint32_t[]){1, 5120}, 2, FLOAT32); // hidden=5120
    ext_wq = tensor_from_base_layout(orch_args + 2, (uint32_t[]){5120, 5120}, 2, BFLOAT16); // hidden=5120
    ext_wk = tensor_from_base_layout(orch_args + 3, (uint32_t[]){5120, 1024}, 2, BFLOAT16); // hidden=5120, kv_hidden=1024
    ext_wv = tensor_from_base_layout(orch_args + 4, (uint32_t[]){5120, 1024}, 2, BFLOAT16); // hidden=5120, kv_hidden=1024
    ext_q_norm_weight = tensor_from_base_layout(orch_args + 5, (uint32_t[]){1, 128}, 2, FLOAT32); // head_dim=128
    ext_k_norm_weight = tensor_from_base_layout(orch_args + 6, (uint32_t[]){1, 128}, 2, FLOAT32); // head_dim=128
    ext_seq_lens = tensor_from_base_layout(orch_args + 7, (uint32_t[]){90}, 1, INT32); // batch=90
    ext_block_table = tensor_from_base_layout(orch_args + 8, (uint32_t[]){2880}, 1, INT32); // num_blocks=2880
    ext_slot_mapping = tensor_from_base_layout(orch_args + 9, (uint32_t[]){90}, 1, INT32); // batch=90
    ext_rope_cos = tensor_from_base_layout(orch_args + 10, (uint32_t[]){4096, 128}, 2, FLOAT32); // max_seq=4096, head_dim=128
    ext_rope_sin = tensor_from_base_layout(orch_args + 11, (uint32_t[]){4096, 128}, 2, FLOAT32); // max_seq=4096, head_dim=128
    ext_k_cache = tensor_from_base_layout(orch_args + 12, (uint32_t[]){2949120, 128}, 2, BFLOAT16); // cache_rows=2880*8*128, head_dim=128
    ext_v_cache = tensor_from_base_layout(orch_args + 13, (uint32_t[]){2949120, 128}, 2, BFLOAT16); // cache_rows=2880*8*128, head_dim=128
    ext_wo = tensor_from_base_layout(orch_args + 14, (uint32_t[]){5120, 5120}, 2, BFLOAT16); // hidden=5120
    ext_post_rms_weight = tensor_from_base_layout(orch_args + 15, (uint32_t[]){1, 5120}, 2, FLOAT32); // hidden=5120
    ext_w_gate = tensor_from_base_layout(orch_args + 16, (uint32_t[]){5120, 17408}, 2, BFLOAT16); // hidden=5120, intermediate=17408
    ext_w_up = tensor_from_base_layout(orch_args + 17, (uint32_t[]){5120, 17408}, 2, BFLOAT16); // hidden=5120, intermediate=17408
    ext_w_down = tensor_from_base_layout(orch_args + 18, (uint32_t[]){17408, 5120}, 2, BFLOAT16); // intermediate=17408, hidden=5120
    ext_out = tensor_from_base_layout(orch_args + 19, (uint32_t[]){90, 5120}, 2, BFLOAT16); // batch=90, hidden=5120

    const int64_t user_batch = 90; 
    const int64_t batch_padded = 96;
    int t0 = alloc_tensors_v2((uint32_t[2]){batch_padded, 5120}, 2, FLOAT32);
    int q_proj = alloc_tensors_v2((uint32_t[2]){batch_padded, 1024}, 2, FLOAT32);
    int k_proj = alloc_tensors_v2((uint32_t[2]){batch_padded, 1024}, 2, FLOAT32);
    int v_proj = alloc_tensors_v2((uint32_t[2]){batch_padded, 5120}, 2, FLOAT32);
    int q_proj_norm = alloc_tensors_v2((uint32_t[2]){batch_padded, 1024}, 2, FLOAT32);
    int k_proj_norm = alloc_tensors_v2((uint32_t[2]){batch_padded, 1024}, 2, FLOAT32);
    for (int64_t b0 = 0; b0 < batch_padded; b0 += 16) {
        int t5 = alloc_tensors_v2((uint32_t[2]){16, 5120}, 2, BFLOAT16);
        alloc_task_id++;
        for (int base = 0; base < 20; base += qwen3_blocks_per_task(20)) {
            alloc_task_id++;
        }

        for (int base = 0; base < 8; base += qwen3_blocks_per_task(8)) {
            alloc_task_id++;
            alloc_task_id++;
        }
        alloc_task_id++;
    }
    int attn_out[6];
    for (int i = 0; i < 6; i++) {
        attn_out[i] = alloc_tensors_v2((uint32_t[2]){16, 5120}, 2, BFLOAT16);
    }
    for (int64_t b = 0; b < user_batch; b += 1) {
        int all_raw_scores = alloc_tensors_v2((uint32_t[2]){4096, 128}, 2, FLOAT32);
        int all_exp_padded = alloc_tensors_v2((uint32_t[2]){4096, 128}, 2, BFLOAT16);
        int all_cur_mi = alloc_tensors_v2((uint32_t[2]){4096, 1}, 2, FLOAT32);
        int all_cur_li = alloc_tensors_v2((uint32_t[2]){4096, 1}, 2, FLOAT32);
        int all_oi_tmp = alloc_tensors_v2((uint32_t[2]){4096, 128}, 2, FLOAT32);
        int q_padded_local = alloc_tensors_v2((uint32_t[2]){128, 128}, 2, BFLOAT16);
        int k_cache_update = alloc_tensors_v2((uint32_t[2]){8, 128}, 2, BFLOAT16); // ROPE KV write-back
        int v_cache_update = alloc_tensors_v2((uint32_t[2]){8, 128}, 2, BFLOAT16); // ROPE KV write-back
        alloc_task_id++;

        for (int base = 0; base < 4; base += qwen3_blocks_per_task(4)) {
            alloc_task_id++;
            alloc_task_id++;
            alloc_task_id++;
            alloc_task_id++;
        }
    }

    for (int64_t b0 = 0; b0 < batch_padded; b0 += 16) {
        int resid1_tile = alloc_tensors_v2((uint32_t[2]){16, 5120}, 2, FLOAT32);
        int gm_pipe_buffer_0 = alloc_tensors_v2((uint32_t[2]){16384, 40}, 2, FLOAT32);
        int post_norm_tile = alloc_tensors_v2((uint32_t[2]){16, 5120}, 2, BFLOAT16);
        int mlp_tile = alloc_tensors_v2((uint32_t[2]){16, 17408}, 2, BFLOAT16);
        int gate_tile = alloc_tensors_v2((uint32_t[2]){16, 17408}, 2, FLOAT32);
        int up_tile = alloc_tensors_v2((uint32_t[2]){16, 17408}, 2, FLOAT32);
        int down_tile = alloc_tensors_v2((uint32_t[2]){16, 5120}, 2, FLOAT32);
        for (int base = 0; base < 40; base += qwen3_blocks_per_task(40)) {
            alloc_task_id++;
        }
        alloc_task_id++;

        for (int base = 0; base < 34; base += qwen3_blocks_per_task(34)) {
            alloc_task_id++;
            alloc_task_id++;
            alloc_task_id++;
        }
        
        for (int base = 0; base < 40; base += qwen3_blocks_per_task(40)) {
            alloc_task_id++;
            alloc_task_id++;
        }
    }
    uint64_t t_end = get_time_ns();
    double elapsed_s = (double)(t_end - t_start) / 1e9;
    double throughput = (double)alloc_task_id / elapsed_s / 1e6;
    fprintf(stderr, "orchestrator_alloc throughput: %.3f MTasks/s (tasks=%u, time=%.6f s)\n",
            throughput, alloc_task_id, elapsed_s);
}
