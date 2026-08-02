// Orchestration Function: qwen3_decode (dynamic tensormap, configurable-SPMD
// variant).
//
// Mirrors
// V200-benchmark/qwen3/qwen3_dynamic_tensormap/orchestration/qwen3_decode.cpp.
// Dependencies are discovered automatically via tensormap
// (tm_in/tm_out). SPMD tier is selected at compile time via
// QWEN3_SPMD_TIER (0=non-spmd .. 4=all-spmd).
//
// Durations are V200-benchmark per-subtask means (README.md §1.2.1 AICore View)
// in ns.
#include <stddef.h>
#include <stdint.h>

#include "mem_pool.h"
#include "orch_config.h"

#define DUR_RMSNORM 23950
#define DUR_Q_PROJ 26060
#define DUR_K_PROJ 18170
#define DUR_V_PROJ 17890
#define DUR_QK_NORM 13190
#define DUR_ROPE_KV_CACHE 9480
#define DUR_QK_MATMUL 29350
#define DUR_SOFTMAX 19400
#define DUR_SV_MATMUL 31650
#define DUR_ONLINE_SOFTMAX 20820
#define DUR_OUT_PROJ 40750
#define DUR_POST_RMSNORM 24390
#define DUR_GATE_PROJ 95700
#define DUR_UP_PROJ 97140
#define DUR_SILU 2820
#define DUR_DOWN_PROJ 72220
#define DUR_DOWN_PROJ_RES 2590

int g_subtask_cnt = 0;
struct task_tensor_desc g_task_tensor_buf[RING_SIZE];

static inline int qwen3_min_i(int a, int b) {
    return a < b ? a : b;
}

static inline int qwen3_blocks_per_task(int total_chunks) {
    static const int targets[5] = {1, 2, 4, 8, 1 << 30};
    int target = targets[QWEN3_SPMD_TIER];
    return qwen3_min_i(total_chunks, target);
}

static inline int qwen3_cur_blocks(int total_chunks, int base) {
    return qwen3_min_i(qwen3_blocks_per_task(total_chunks), total_chunks - base);
}

extern Tensor g_tensors[MAX_TENSOR_NUM];
extern int desc_thread_count;
extern int desc_batch_size;

static inline Tensor get_tensor(int idx)
{
    return g_tensors[idx];
}

static inline void add_tensor_ro(uint16_t task_id, Tensor t)
{
    int idx = g_basic_buf[task_id & RING_MASK].tensor_cnt++;
    g_basic_buf[task_id & RING_MASK].data[idx] = t.buffer_addr;
}

static inline void add_tensor(uint16_t task_id, Tensor t)
{
    int ring_idx = task_id & RING_MASK;
    int idx = g_basic_buf[ring_idx].tensor_cnt++;
    g_basic_buf[ring_idx].data[idx] = t.buffer_addr;

    int idx2 = g_task_tensor_buf[ring_idx].tensor_cnt++;
    g_task_tensor_buf[ring_idx].data[idx2] = t;
}

/* Determine whether the current thread should process the given desc_task_id.
 * Thread T processes desc_task_ids for which
 *   (desc_task_id / desc_batch_size) % DESC_THREAD_COUNT == T
 * e.g. 4 threads, desc_batch_size=60:
 *   Thread 0: 0~59, 240~299, 480~539, ...
 *   Thread 1: 60~119, 300~359, 540~599, ...
 *   Thread 2: 120~179, 360~419, 600~659, ...
 *   Thread 3: 180~239, 420~479, 660~719, ...
 */
static inline int desc_should_process(int desc_task_id, int desc_end, int desc_start) {
    return desc_task_id < desc_end && desc_task_id >= desc_start;
}

/* Helper: if the current thread owns task_id, call the body macro with
 * (task_id), then increment; otherwise just increment task_id.
 * Use __VA_ARGS__ to pass multi-statement blocks. */
#define DESC_DO_OR_SKIP(task_id, desc_end, desc_start, ...)                    \
    do {                                             \
        if (desc_should_process((task_id), (desc_end), (desc_start))) { \
            int __did = (task_id);                       \
            __VA_ARGS__                              \
        } else {          \
            (desc_end) += desc_batch_size * desc_thread_count;  \
            (desc_start) +=  desc_batch_size *  desc_thread_count;       \
        }                                \
        (task_id)++;                                     \ 
    } while (0)

int orchestrator_desc(const uint64_t orch_args, int thread_id) {
    int desc_task_id = 0;
    int desc_start = thread_id * desc_batch_size;
    int desc_end = (thread_id  + 1 ) * desc_batch_size;
    int tensor_index = 0;
    Tensor ext_hidden_states = tensor_from_base_layout(orch_args + 0, (uint32_t[]){90, 5120}, 2, BFLOAT16); // batch=90, hidden=5120
    Tensor ext_input_rms_weight = tensor_from_base_layout(orch_args + 1, (uint32_t[]){1, 5120}, 2, FLOAT32); // hidden=5120
    Tensor ext_wq = tensor_from_base_layout(orch_args + 2, (uint32_t[]){5120, 5120}, 2, BFLOAT16); // hidden=5120
    Tensor ext_wk = tensor_from_base_layout(orch_args + 3, (uint32_t[]){5120, 1024}, 2, BFLOAT16); // hidden=5120, kv_hidden=1024
    Tensor ext_wv = tensor_from_base_layout(orch_args + 4, (uint32_t[]){5120, 1024}, 2, BFLOAT16); // hidden=5120, kv_hidden=1024
    Tensor ext_q_norm_weight = tensor_from_base_layout(orch_args + 5, (uint32_t[]){1, 128}, 2, FLOAT32); // head_dim=128
    Tensor ext_k_norm_weight = tensor_from_base_layout(orch_args + 6, (uint32_t[]){1, 128}, 2, FLOAT32); // head_dim=128
    Tensor ext_seq_lens = tensor_from_base_layout(orch_args + 7, (uint32_t[]){90}, 1, INT32); // batch=90
    Tensor ext_block_table = tensor_from_base_layout(orch_args + 8, (uint32_t[]){2880}, 1, INT32); // num_blocks=2880
    Tensor ext_slot_mapping = tensor_from_base_layout(orch_args + 9, (uint32_t[]){90}, 1, INT32); // batch=90
    Tensor ext_rope_cos = tensor_from_base_layout(orch_args + 10, (uint32_t[]){4096, 128}, 2, FLOAT32); // max_seq=4096, head_dim=128
    Tensor ext_rope_sin = tensor_from_base_layout(orch_args + 11, (uint32_t[]){4096, 128}, 2, FLOAT32); // max_seq=4096, head_dim=128
    Tensor ext_k_cache = tensor_from_base_layout(orch_args + 12, (uint32_t[]){2949120, 128}, 2, BFLOAT16); // cache_rows=2880*8*128, head_dim=128
    Tensor ext_v_cache = tensor_from_base_layout(orch_args + 13, (uint32_t[]){2949120, 128}, 2, BFLOAT16); // cache_rows=2880*8*128, head_dim=128
    Tensor ext_wo = tensor_from_base_layout(orch_args + 14, (uint32_t[]){5120, 5120}, 2, BFLOAT16); // hidden=5120
    Tensor ext_post_rms_weight = tensor_from_base_layout(orch_args + 15, (uint32_t[]){1, 5120}, 2, FLOAT32); // hidden=5120
    Tensor ext_w_gate = tensor_from_base_layout(orch_args + 16, (uint32_t[]){5120, 17408}, 2, BFLOAT16); // hidden=5120, intermediate=17408
    Tensor ext_w_up = tensor_from_base_layout(orch_args + 17, (uint32_t[]){5120, 17408}, 2, BFLOAT16); // hidden=5120, intermediate=17408
    Tensor ext_w_down = tensor_from_base_layout(orch_args + 18, (uint32_t[]){17408, 5120}, 2, BFLOAT16); // intermediate=17408, hidden=5120
    Tensor ext_out = tensor_from_base_layout(orch_args + 19, (uint32_t[]){90, 5120}, 2, BFLOAT16); // batch=90, hidden=5120
    (void)ext_seq_lens;
    (void)ext_slot_mapping;
    tm_deps_init();
    const int64_t user_batch = 90; // batch=90
    const int64_t batch_padded = 96; // ((batch+15)/16)*16
    Tensor q_proj = get_tensor(tensor_index++);
    Tensor k_proj = get_tensor(tensor_index++);
    Tensor v_proj = get_tensor(tensor_index++);
    Tensor q_proj_norm = get_tensor(tensor_index++);
    Tensor k_proj_norm = get_tensor(tensor_index++);

    for (int64_t b0 = 0; b0 < batch_padded; b0 += 16) {
        Tensor normed_tile = get_tensor(tensor_index++);
        const int64_t cur_valid = (user_batch - b0 > 16) ? 16 : (user_batch - b0);
        DESC_DO_OR_SKIP(desc_task_id, desc_end, desc_start, {
            new_task(__did, TASK_TYPE_VECTOR, 1, DUR_RMSNORM);
            add_tensor_ro(__did, ext_hidden_states);
            add_tensor(__did, normed_tile);
            add_tensor_ro(__did, ext_input_rms_weight);
            add_scalar(__did, b0);
            add_scalar(__did, cur_valid);
        });

        for (int base = 0; base < 20; base += qwen3_blocks_per_task(20)) {
            int cur_blocks = qwen3_cur_blocks(20, base);
            Tensor q_piece = view(q_proj, (uint32_t)b0, base * 256u, 16u, cur_blocks * 256u);
            DESC_DO_OR_SKIP(desc_task_id, desc_end, desc_start, {
                new_task(__did, TASK_TYPE_CUBE, (uint32_t)cur_blocks, DUR_Q_PROJ);
                add_tensor(__did, normed_tile);
                add_tensor_ro(__did, ext_wq);
                add_tensor(__did, q_piece);
                add_scalar(__did, b0);
                add_scalar(__did, base);
            });
        }

        for (int base = 0; base < 8; base += qwen3_blocks_per_task(8)) {
            int cur_blocks = qwen3_cur_blocks(8, base);
            Tensor k_piece = view(k_proj, (uint32_t)b0, base * 128u, 16u, cur_blocks * 128u);
            DESC_DO_OR_SKIP(desc_task_id, desc_end, desc_start, {
                new_task(__did, TASK_TYPE_CUBE, (uint32_t)cur_blocks, DUR_K_PROJ);
                add_tensor(__did, normed_tile);
                add_tensor_ro(__did, ext_wk);
                add_tensor(__did, k_piece);
                add_scalar(__did, b0);
                add_scalar(__did, base);
            });

            Tensor v_piece = view(v_proj, (uint32_t)b0, base * 128u, 16u, cur_blocks * 128u);
            DESC_DO_OR_SKIP(desc_task_id, desc_end, desc_start, {
                new_task(__did, TASK_TYPE_CUBE, (uint32_t)cur_blocks, DUR_V_PROJ);
                add_tensor(__did, normed_tile);
                add_tensor_ro(__did, ext_wv);
                add_tensor(__did, v_piece);
                add_scalar(__did, b0);
                add_scalar(__did, base);
            });
        }

        Tensor k0_norm = view(k_proj_norm, (uint32_t)b0, 0u, 16u, 1024u);
        Tensor q0_norm = view(q_proj_norm, (uint32_t)b0, 0u, 16u, 5120u);
        Tensor q0_in = view(q_proj, (uint32_t)b0, 0u, 16u, 5120u);
        Tensor k0_in = view(k_proj, (uint32_t)b0, 0u, 16u, 1024u);
        DESC_DO_OR_SKIP(desc_task_id, desc_end, desc_start, {
            new_task(__did, TASK_TYPE_VECTOR, 1, DUR_QK_NORM);
            add_tensor(__did, k0_norm);
            add_tensor(__did, q0_norm);
            add_tensor(__did, q0_in);
            add_tensor_ro(__did, ext_q_norm_weight);
            add_tensor_ro(__did, ext_k_norm_weight);
            add_tensor(__did, k0_in);
        });
    }

    Tensor attn_out[6];
    for (int i = 0; i < 6; i++) {
        attn_out[i] = get_tensor(tensor_index++);
    }

    for (int64_t b = 0; b < user_batch; b += 1) {
        Tensor all_raw_scores = get_tensor(tensor_index++);
        Tensor all_exp_padded = get_tensor(tensor_index++);
        Tensor all_cur_mi = get_tensor(tensor_index++);
        Tensor all_cur_li = get_tensor(tensor_index++);
        Tensor all_oi_tmp = get_tensor(tensor_index++);
        Tensor q_padded_local = get_tensor(tensor_index++);
        Tensor k_cache_local = view(ext_k_cache, (uint32_t)b * 8u, 0u, 8u, 128u); // batch b: 8*head_dim=1024 kv_hidden
        Tensor v_cache_local = view(ext_v_cache, (uint32_t)b * 8u, 0u, 8u, 128u); // batch b: 8*head_dim=1024 kv_hidden
        Tensor k_cache_update = get_tensor(tensor_index++); // ROPE KV write-back
        Tensor v_cache_update = get_tensor(tensor_index++); // ROPE KV write-back
        const int64_t b_tile0 = (b / 16) * 16;
        const int64_t slot = b;
        const int64_t slot_block = slot / 128;
        const int64_t slot_offset = slot - slot_block * 128;
        {
            Tensor k0_norm_r = view(k_proj_norm, (uint32_t)b_tile0, 0u, 16u, 1024u);
            Tensor v0 = view(v_proj, (uint32_t)b_tile0, 0u, 16u, 1024u);
            Tensor q0_norm_r = view(q_proj_norm, (uint32_t)b_tile0, 0u, 16u, 5120u);
            DESC_DO_OR_SKIP(desc_task_id, desc_end, desc_start, {
                new_task(__did, TASK_TYPE_VECTOR, 1, DUR_ROPE_KV_CACHE);
                add_tensor(__did, q_padded_local);
                add_tensor_ro(__did, k_cache_local);
                add_tensor_ro(__did, v_cache_local);
                add_tensor(__did, k_cache_update);
                add_tensor(__did, v_cache_update);
                add_tensor(__did, k0_norm_r);
                add_tensor_ro(__did, ext_rope_cos);
                add_tensor_ro(__did, ext_rope_sin);
                add_tensor_ro(__did, ext_rope_cos);
                add_tensor_ro(__did, ext_rope_sin);
                add_tensor(__did, v0);
                add_tensor(__did, q0_norm_r);
                add_scalar(__did, slot_block);
                add_scalar(__did, slot_offset);
                add_scalar(__did, b);
            });
        }

        for (int base = 0; base < 4; base += qwen3_blocks_per_task(4)) {
            int cur_blocks = qwen3_cur_blocks(4, base);
            Tensor row_piece = view(all_raw_scores, base * 1024u, 0u, (uint32_t)(cur_blocks * 1024), 128u);
            DESC_DO_OR_SKIP(desc_task_id, desc_end, desc_start, {
                new_task(__did, TASK_TYPE_CUBE, (uint32_t)cur_blocks, DUR_QK_MATMUL);
                add_tensor(__did, q_padded_local);
                add_tensor(__did, row_piece);
                add_tensor_ro(__did, ext_block_table);
                add_tensor(__did, k_cache_update);
                add_scalar(__did, b);
                add_scalar(__did, 8);      // (1024+127)/128: KV context blocks
                add_scalar(__did, b * 32); // block_table row offset for batch b
                add_scalar(__did, base);
            });

            Tensor cur_li_piece = view(all_cur_li, base * 1024u, 0u, (uint32_t)(cur_blocks * 1024), 1u);
            Tensor cur_mi_piece = view(all_cur_mi, base * 1024u, 0u, (uint32_t)(cur_blocks * 1024), 1u);
            Tensor exp_padded_piece = view(all_exp_padded, base * 1024u, 0u, (uint32_t)(cur_blocks * 1024), 128u);
            DESC_DO_OR_SKIP(desc_task_id, desc_end, desc_start, {
                new_task(__did, TASK_TYPE_VECTOR, (uint32_t)cur_blocks, DUR_SOFTMAX);
                add_tensor(__did, cur_li_piece);
                add_tensor(__did, cur_mi_piece);
                add_tensor(__did, exp_padded_piece);
                add_scalar(__did, 8);    // (1024+127)/128: KV context blocks
                add_scalar(__did, 1024); // context length (tokens)
                add_scalar(__did, base);
            });

            Tensor exp_piece = view(all_exp_padded, base * 1024u, 0u, (uint32_t)(cur_blocks * 1024), 128u);
            Tensor oi_tmp_piece = view(all_oi_tmp, base * 1024u, 0u, (uint32_t)(cur_blocks * 1024), 128u);
            DESC_DO_OR_SKIP(desc_task_id, desc_end, desc_start, {
                new_task(__did, TASK_TYPE_CUBE, (uint32_t)cur_blocks, DUR_SV_MATMUL);
                add_tensor(__did, oi_tmp_piece);
                add_tensor_ro(__did, ext_block_table);
                add_tensor(__did, exp_piece);
                add_tensor(__did, v_cache_update);
                add_scalar(__did, 8);      // (1024+127)/128: KV context blocks
                add_scalar(__did, b * 32); // block_table row offset for batch b
                add_scalar(__did, base);
            });

            Tensor attn_out_piece = view(attn_out[b / 16], (uint32_t)(b % 16),
                base * 1280u, 1u, cur_blocks * 1280u);
            DESC_DO_OR_SKIP(desc_task_id, desc_end, desc_start, {
                new_task(__did, TASK_TYPE_VECTOR, (uint32_t)cur_blocks, DUR_ONLINE_SOFTMAX);
                add_tensor(__did, oi_tmp_piece);
                add_tensor(__did, cur_mi_piece);
                add_tensor(__did, cur_li_piece);
                add_tensor(__did, attn_out_piece);
                add_scalar(__did, 8); // (1024+127)/128: KV context blocks
                add_scalar(__did, base);
            });
        }
    }

    for (int64_t b0 = 0; b0 < batch_padded; b0 += 16) {
        Tensor resid1_tile = get_tensor(tensor_index++);
        Tensor gm_pipe_buffer_0 = get_tensor(tensor_index++);
        Tensor post_norm_tile = get_tensor(tensor_index++);
        Tensor mlp_tile = get_tensor(tensor_index++);
        Tensor gate_tile = get_tensor(tensor_index++);
        Tensor up_tile = get_tensor(tensor_index++);
        Tensor down_tile = get_tensor(tensor_index++);
        const int64_t cur_valid = (user_batch - b0 > 16) ? 16 : (user_batch - b0);
        for (int base = 0; base < 40; base += qwen3_blocks_per_task(40)) {
            // 40: out_proj SPMD total chunks; cols/chunk = 5120/40 = 128
            int cur_blocks = qwen3_cur_blocks(40, base);
            Tensor attn_out_tile = view(attn_out[b0 / 16], 0u, 0u, (uint32_t)cur_valid, 5120u);
            Tensor resid1_piece0 = view(resid1_tile, 0u, base * 128u, 16u, (uint32_t)(cur_blocks * 128));
            DESC_DO_OR_SKIP(desc_task_id, desc_end, desc_start, {
                new_task(__did, TASK_TYPE_MIX, (uint32_t)cur_blocks, DUR_OUT_PROJ);
                add_tensor_ro(__did, ext_hidden_states);
                add_tensor(__did, attn_out_tile);
                add_tensor_ro(__did, ext_wo);
                add_tensor(__did, resid1_piece0);
                add_tensor(__did, gm_pipe_buffer_0);
                add_scalar(__did, b0);
                add_scalar(__did, cur_valid);
                add_scalar(__did, base);
            });
        }

        DESC_DO_OR_SKIP(desc_task_id, desc_end, desc_start, {
            new_task(__did, TASK_TYPE_VECTOR, 1, DUR_POST_RMSNORM);
            add_tensor(__did, resid1_tile);
            add_tensor(__did, post_norm_tile);
            add_tensor_ro(__did, ext_post_rms_weight);
        });

        for (int base = 0; base < 34; base += qwen3_blocks_per_task(34)) {
            int cur_blocks = qwen3_cur_blocks(34, base);
            Tensor gate_piece = view(gate_tile, 0u, base * 512u, 16u, (uint32_t)(cur_blocks * 512));
            Tensor up_piece = view(up_tile, 0u, base * 512u, 16u, (uint32_t)(cur_blocks * 512));
            DESC_DO_OR_SKIP(desc_task_id, desc_end, desc_start, {
                new_task(__did, TASK_TYPE_CUBE, (uint32_t)cur_blocks, DUR_GATE_PROJ);
                add_tensor(__did, post_norm_tile);
                add_tensor_ro(__did, ext_w_gate);
                add_tensor(__did, gate_piece);
                add_scalar(__did, base);
            });

            DESC_DO_OR_SKIP(desc_task_id, desc_end, desc_start, {
                new_task(__did, TASK_TYPE_CUBE, (uint32_t)cur_blocks, DUR_UP_PROJ);
                add_tensor(__did, post_norm_tile);
                add_tensor_ro(__did, ext_w_up);
                add_tensor(__did, up_piece);
                add_scalar(__did, base);
            });

            Tensor mlp_piece = view(mlp_tile, 0u, base * 512u, 16u, (uint32_t)(cur_blocks * 512));
            DESC_DO_OR_SKIP(desc_task_id, desc_end, desc_start, {
                new_task(__did, TASK_TYPE_VECTOR, (uint32_t)cur_blocks, DUR_SILU);
                add_tensor(__did, gate_piece);
                add_tensor(__did, up_piece);
                add_tensor(__did, mlp_piece);
                add_scalar(__did, base);
            });
        }
        for (int base = 0; base < 40; base += qwen3_blocks_per_task(40)) {
            int cur_blocks = qwen3_cur_blocks(40, base);
            Tensor down_piece = view(down_tile, 0u, base * 128u, 16u, (uint32_t)(cur_blocks * 128));
            Tensor resid1_piece1 = view(resid1_tile, 0u, base * 128u, 16u, (uint32_t)(cur_blocks * 128));
            DESC_DO_OR_SKIP(desc_task_id, desc_end, desc_start, {
                new_task(__did, TASK_TYPE_CUBE, (uint32_t)cur_blocks, DUR_DOWN_PROJ);
                add_tensor(__did, mlp_tile);
                add_tensor_ro(__did, ext_w_down);
                add_tensor(__did, down_piece);
                add_scalar(__did, base);
            });

            DESC_DO_OR_SKIP(desc_task_id, desc_end, desc_start, {
                new_task(__did, TASK_TYPE_VECTOR, (uint32_t)cur_blocks, DUR_DOWN_PROJ_RES);
                add_tensor(__did, down_piece);
                add_tensor(__did, resid1_piece1);
                add_tensor_ro(__did, ext_out);
                add_scalar(__did, cur_valid);
                add_scalar(__did, b0);
                add_scalar(__did, base);
            });
        }
    }
    return desc_task_id;
}