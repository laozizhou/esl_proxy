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

#include "dispatch.h"
#include "mem_pool.h"
#include "orch_config.h"
#include "ring_buf.h"

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

static inline void add_tensor(uint32_t task_id, Tensor t)
{
    int ring_idx = task_id & RING_MASK;
    int idx = g_basic_buf[ring_idx].tensor_cnt++;
    g_basic_buf[ring_idx].data[idx] = t.buffer_addr;

    int idx2 = g_task_tensor_buf[ring_idx].tensor_cnt++;
    g_task_tensor_buf[ring_idx].data[idx2] = t;
}

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

static inline int qwen3_n_tasks(int total_chunks, int bpt) {
    int n = 0;
    for (int base = 0; base < total_chunks; base += bpt)
        n++;
    return n;
}

extern Tensor g_tensors[MAX_TENSOR_NUM];
extern int desc_thread_count;
extern int desc_batch_size;

static inline Tensor get_tensor(int idx)
{
    return g_tensors[idx];
}

/* Helper: if the current thread owns task_id, call the body macro with
 * (task_id), then increment; otherwise just increment task_id.
 * Use __VA_ARGS__ to pass multi-statement blocks. */
#define DESC_DO_OR_SKIP(task_id,...)                    \
    do {                                             \
        if ((task_id) < desc_end && (task_id) >= desc_start) { \
            int __did = (task_id);                       \
            desc_created_cnt++;                      \
            __VA_ARGS__                              \
        } else {          \
            if ((task_id) >= desc_end) {                         \
                desc_end += (desc_batch_size * desc_thread_count);  \
                desc_start += (desc_batch_size * desc_thread_count); \
            }                                                    \
        }                                \
        (task_id)++;                                     \
    } while (0)

// Global predecessor ID arrays – written by the creating thread, read by any
// thread that needs a predecessor reference.  The orchestration processes tasks
// in a fixed global order; stores happen before dependent reads within each
// thread's sequential DESC_DO_OR_SKIP loop.
static uint32_t g_rmsnorm_ids[6];          // per b0 tile
static uint32_t g_qk_norm_ids[6];          // per b0 tile
static uint32_t g_rope_ids[90];            // per batch element
static uint32_t g_post_rmsnorm_ids[6];     // per b0 tile
static uint32_t g_os_by_b[90][4];          // online_softmax per (b, chunk)
static uint32_t g_os_cnt_by_b[90];         // count of os tasks per batch

int orchestrator_desc_ap(const uint64_t orch_args, int thread_id, int *created_cnt) {
    int desc_task_id = 0;
    int desc_created_cnt = 0;
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
    
    const int64_t user_batch = 90; // batch=90
    const int64_t batch_padded = 96; // ((batch+15)/16)*16
    Tensor q_proj = get_tensor(tensor_index++);
    Tensor k_proj = get_tensor(tensor_index++);
    Tensor v_proj = get_tensor(tensor_index++);
    Tensor q_proj_norm = get_tensor(tensor_index++);
    Tensor k_proj_norm = get_tensor(tensor_index++);

    uint32_t batch_predecessors[90];
    uint32_t v_ids_per_tile[6][8];
    uint32_t v_cnt_per_tile[6];

    for (int64_t b0 = 0; b0 < batch_padded; b0 += 16) {
        const size_t tix = (size_t)(b0 / 16);
        Tensor normed_tile = get_tensor(tensor_index++);
        const int64_t cur_valid = (user_batch - b0 > 16) ? 16 : (user_batch - b0);

        uint32_t q_ids[20];
        uint32_t k_ids[8];
        uint32_t v_ids[8];

        // --- RMSNORM (+1) ---
        DESC_DO_OR_SKIP(desc_task_id,  {
            new_task(__did, TASK_TYPE_VECTOR, 1, DUR_RMSNORM);
            add_input(__did, ext_hidden_states);
            add_output(__did, normed_tile);
            add_input(__did, ext_input_rms_weight);
            add_scalar(__did, b0);
            add_scalar(__did, cur_valid);
            g_rmsnorm_ids[tix] = (uint32_t)__did;
        });

        // --- Q_PROJ (+2) ---
        for (int qi = 0, base = 0; base < 20; base += qwen3_blocks_per_task(20)) {
            int cur_blocks = qwen3_cur_blocks(20, base);
            Tensor q_piece = view(q_proj, (uint32_t)b0, base * 256u, 16u, cur_blocks * 256u);
            batch_predecessors[0] = g_rmsnorm_ids[tix];
            DESC_DO_OR_SKIP(desc_task_id, {
                new_task(__did, TASK_TYPE_CUBE, (uint32_t)cur_blocks, DUR_Q_PROJ);
                add_input(__did, normed_tile);
                add_input(__did, ext_wq);
                add_output(__did, q_piece);
                add_scalar(__did, b0);
                add_scalar(__did, base);
                add_predecessors(__did, batch_predecessors, 1, 0);
                q_ids[qi] = (uint32_t)__did;
            });
            qi++;
        }

        // --- K_PROJ & V_PROJ (+3, +4) ---
        for (int ki = 0, vi = 0, base = 0; base < 8; base += qwen3_blocks_per_task(8)) {
            int cur_blocks = qwen3_cur_blocks(8, base);
            Tensor k_piece = view(k_proj, (uint32_t)b0, base * 128u, 16u, cur_blocks * 128u);
            batch_predecessors[0] = g_rmsnorm_ids[tix];
            DESC_DO_OR_SKIP(desc_task_id,  {
                new_task(__did, TASK_TYPE_CUBE, (uint32_t)cur_blocks, DUR_K_PROJ);
                add_input(__did, normed_tile);
                add_input(__did, ext_wk);
                add_output(__did, k_piece);
                add_scalar(__did, b0);
                add_scalar(__did, base);
                add_predecessors(__did, batch_predecessors, 1, 0);
                k_ids[ki] = (uint32_t)__did;
            });
            ki++;

            Tensor v_piece = view(v_proj, (uint32_t)b0, base * 128u, 16u, cur_blocks * 128u);
            batch_predecessors[0] = g_rmsnorm_ids[tix];
            DESC_DO_OR_SKIP(desc_task_id,  {
                new_task(__did, TASK_TYPE_CUBE, (uint32_t)cur_blocks, DUR_V_PROJ);
                add_input(__did, normed_tile);
                add_input(__did, ext_wv);
                add_output(__did, v_piece);
                add_scalar(__did, b0);
                add_scalar(__did, base);
                add_predecessors(__did, batch_predecessors, 1, 0);
                v_ids[vi] = (uint32_t)__did;
            });
            vi++;
        }
        for (int i = 0; i < qwen3_n_tasks(8, qwen3_blocks_per_task(8)); i++)
            v_ids_per_tile[tix][i] = v_ids[i];
        v_cnt_per_tile[tix] = (uint32_t)qwen3_n_tasks(8, qwen3_blocks_per_task(8));

        // --- QK_NORM (+5) ---
        Tensor k0_norm = view(k_proj_norm, (uint32_t)b0, 0u, 16u, 1024u);
        Tensor q0_norm = view(q_proj_norm, (uint32_t)b0, 0u, 16u, 5120u);
        Tensor q0_in = view(q_proj, (uint32_t)b0, 0u, 16u, 5120u);
        Tensor k0_in = view(k_proj, (uint32_t)b0, 0u, 16u, 1024u);
        int q_cnt = qwen3_n_tasks(20, qwen3_blocks_per_task(20));
        int k_cnt = qwen3_n_tasks(8, qwen3_blocks_per_task(8));
        DESC_DO_OR_SKIP(desc_task_id,  {
            new_task(__did, TASK_TYPE_VECTOR, 1, DUR_QK_NORM);
            add_output(__did, k0_norm);
            add_output(__did, q0_norm);
            add_input(__did, q0_in);
            add_input(__did, ext_q_norm_weight);
            add_input(__did, ext_k_norm_weight);
            add_input(__did, k0_in);
            int idx = add_predecessors(__did, q_ids, (uint32_t)q_cnt, 0);
            add_predecessors(__did, k_ids, (uint32_t)k_cnt, idx);
            g_qk_norm_ids[tix] = (uint32_t)__did;
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
        const size_t tix = (size_t)(b / 16);
        const int64_t slot = b;
        const int64_t slot_block = slot / 128;
        const int64_t slot_offset = slot - slot_block * 128;

        uint32_t qk_ids[4];
        uint32_t sm_ids[4];
        uint32_t sv_ids[4];

        // --- ROPE_KV_CACHE (+6) ---
        {
            Tensor k0_norm_r = view(k_proj_norm, (uint32_t)b_tile0, 0u, 16u, 1024u);
            Tensor v0 = view(v_proj, (uint32_t)b_tile0, 0u, 16u, 1024u);
            Tensor q0_norm_r = view(q_proj_norm, (uint32_t)b_tile0, 0u, 16u, 5120u);
            batch_predecessors[0] = g_qk_norm_ids[tix];
            int tmp = 1;
            for (uint32_t i = 0; i < v_cnt_per_tile[tix]; i++)
                batch_predecessors[tmp++] = v_ids_per_tile[tix][i];
            DESC_DO_OR_SKIP(desc_task_id,  {
                new_task(__did, TASK_TYPE_VECTOR, 1, DUR_ROPE_KV_CACHE);
                add_output(__did, q_padded_local);
                add_output(__did, k_cache_local);
                add_output(__did, v_cache_local);
                add_output(__did, k_cache_update);
                add_output(__did, v_cache_update);
                add_input(__did, k0_norm_r);
                add_input(__did, ext_rope_cos);
                add_input(__did, ext_rope_sin);
                add_input(__did, ext_rope_cos);
                add_input(__did, ext_rope_sin);
                add_input(__did, v0);
                add_input(__did, q0_norm_r);
                add_scalar(__did, slot_block);
                add_scalar(__did, slot_offset);
                add_scalar(__did, b);
                add_predecessors(__did, batch_predecessors, (uint32_t)tmp, 0);
                g_rope_ids[b] = (uint32_t)__did;
            });
        }

        // --- QK_MATMUL (+7), SOFTMAX (+8), SV_MATMUL (+9), ONLINE_SOFTMAX (+10) ---
        for (int ci = 0, base = 0; base < 4; base += qwen3_blocks_per_task(4)) {
            int cur_blocks = qwen3_cur_blocks(4, base);
            Tensor row_piece = view(all_raw_scores, base * 1024u, 0u, (uint32_t)(cur_blocks * 1024), 128u);
            batch_predecessors[0] = g_rope_ids[b];
            DESC_DO_OR_SKIP(desc_task_id,  {
                new_task(__did, TASK_TYPE_CUBE, (uint32_t)cur_blocks, DUR_QK_MATMUL);
                add_input(__did, q_padded_local);
                add_output(__did, row_piece);
                add_input(__did, ext_block_table);
                add_input(__did, k_cache_update);
                add_scalar(__did, b);
                add_scalar(__did, 8);      // (1024+127)/128: KV context blocks
                add_scalar(__did, b * 32); // block_table row offset for batch b
                add_scalar(__did, base);
                add_predecessors(__did, batch_predecessors, 1, 0);
                qk_ids[ci] = (uint32_t)__did;
            });

            Tensor cur_li_piece = view(all_cur_li, base * 1024u, 0u, (uint32_t)(cur_blocks * 1024), 1u);
            Tensor cur_mi_piece = view(all_cur_mi, base * 1024u, 0u, (uint32_t)(cur_blocks * 1024), 1u);
            Tensor exp_padded_piece = view(all_exp_padded, base * 1024u, 0u, (uint32_t)(cur_blocks * 1024), 128u);
            batch_predecessors[0] = qk_ids[ci];
            DESC_DO_OR_SKIP(desc_task_id,  {
                new_task(__did, TASK_TYPE_VECTOR, (uint32_t)cur_blocks, DUR_SOFTMAX);
                add_output(__did, cur_li_piece);
                add_output(__did, cur_mi_piece);
                add_output(__did, exp_padded_piece);
                add_scalar(__did, 8);    // (1024+127)/128: KV context blocks
                add_scalar(__did, 1024); // context length (tokens)
                add_scalar(__did, base);
                add_predecessors(__did, batch_predecessors, 1, 0);
                sm_ids[ci] = (uint32_t)__did;
            });

            Tensor exp_piece = view(all_exp_padded, base * 1024u, 0u, (uint32_t)(cur_blocks * 1024), 128u);
            Tensor oi_tmp_piece = view(all_oi_tmp, base * 1024u, 0u, (uint32_t)(cur_blocks * 1024), 128u);
            batch_predecessors[0] = g_rope_ids[b];
            batch_predecessors[1] = sm_ids[ci];
            DESC_DO_OR_SKIP(desc_task_id,  {
                new_task(__did, TASK_TYPE_CUBE, (uint32_t)cur_blocks, DUR_SV_MATMUL);
                add_output(__did, oi_tmp_piece);
                add_input(__did, ext_block_table);
                add_input(__did, exp_piece);
                add_input(__did, v_cache_update);
                add_scalar(__did, 8);      // (1024+127)/128: KV context blocks
                add_scalar(__did, b * 32); // block_table row offset for batch b
                add_scalar(__did, base);
                add_predecessors(__did, batch_predecessors, 2, 0);
                sv_ids[ci] = (uint32_t)__did;
            });

            Tensor attn_out_piece = view(attn_out[b / 16], (uint32_t)(b % 16),
                base * 1280u, 1u, cur_blocks * 1280u);
            batch_predecessors[0] = sv_ids[ci];
            batch_predecessors[1] = sm_ids[ci];
            DESC_DO_OR_SKIP(desc_task_id,  {
                new_task(__did, TASK_TYPE_VECTOR, (uint32_t)cur_blocks, DUR_ONLINE_SOFTMAX);
                add_input(__did, oi_tmp_piece);
                add_input(__did, cur_mi_piece);
                add_input(__did, cur_li_piece);
                add_output(__did, attn_out_piece);
                add_scalar(__did, 8); // (1024+127)/128: KV context blocks
                add_scalar(__did, base);
                add_predecessors(__did, batch_predecessors, 2, 0);
                g_os_by_b[b][ci] = (uint32_t)__did;
            });
            ci++;
        }
        g_os_cnt_by_b[b] = (uint32_t)qwen3_n_tasks(4, qwen3_blocks_per_task(4));
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

        uint32_t op_ids[40];
        uint32_t gate_ids[34];
        uint32_t up_ids[34];
        uint32_t silu_ids[34];
        uint32_t down_ids[40];

        // --- OUT_PROJ (+11) ---
        for (int opi = 0, base = 0; base < 40; base += qwen3_blocks_per_task(40)) {
            // 40: out_proj SPMD total chunks; cols/chunk = 5120/40 = 128
            int cur_blocks = qwen3_cur_blocks(40, base);
            Tensor attn_out_tile = view(attn_out[b0 / 16], 0u, 0u, (uint32_t)cur_valid, 5120u);
            Tensor resid1_piece0 = view(resid1_tile, 0u, base * 128u, 16u, (uint32_t)(cur_blocks * 128));
            DESC_DO_OR_SKIP(desc_task_id,  {
                uint32_t pred_idx = 0;
                for (int64_t row = 0; row < cur_valid; row++) {
                    const int64_t bb = b0 + row;
                    pred_idx = (uint32_t)add_predecessors(__did, g_os_by_b[bb], g_os_cnt_by_b[bb], pred_idx);
                }
                new_task(__did, TASK_TYPE_MIX, (uint32_t)cur_blocks, DUR_OUT_PROJ);
                add_input(__did, ext_hidden_states);
                add_input(__did, attn_out_tile);
                add_input(__did, ext_wo);
                add_inout(__did, resid1_piece0);
                add_output(__did, gm_pipe_buffer_0);
                add_scalar(__did, b0);
                add_scalar(__did, cur_valid);
                add_scalar(__did, base);
                op_ids[opi] = (uint32_t)__did;
            });
            opi++;
        }

        // --- POST_RMSNORM (+12) ---
        DESC_DO_OR_SKIP(desc_task_id,  {
            new_task(__did, TASK_TYPE_VECTOR, 1, DUR_POST_RMSNORM);
            add_input(__did, resid1_tile);
            add_output(__did, post_norm_tile);
            add_input(__did, ext_post_rms_weight);
            add_predecessors(__did, op_ids, (uint32_t)qwen3_n_tasks(40, qwen3_blocks_per_task(40)), 0);
            g_post_rmsnorm_ids[b0 / 16] = (uint32_t)__did;
        });

        // --- GATE_PROJ, UP_PROJ, SILU (+13, +14, +15) ---
        for (int gi = 0, ui = 0, si = 0, base = 0; base < 34; base += qwen3_blocks_per_task(34)) {
            int cur_blocks = qwen3_cur_blocks(34, base);
            Tensor gate_piece = view(gate_tile, 0u, base * 512u, 16u, (uint32_t)(cur_blocks * 512));
            Tensor up_piece = view(up_tile, 0u, base * 512u, 16u, (uint32_t)(cur_blocks * 512));
            batch_predecessors[0] = g_post_rmsnorm_ids[b0 / 16];
            DESC_DO_OR_SKIP(desc_task_id,  {
                new_task(__did, TASK_TYPE_CUBE, (uint32_t)cur_blocks, DUR_GATE_PROJ);
                add_input(__did, post_norm_tile);
                add_input(__did, ext_w_gate);
                add_inout(__did, gate_piece);
                add_scalar(__did, base);
                add_predecessors(__did, batch_predecessors, 1, 0);
                gate_ids[gi] = (uint32_t)__did;
            });
            gi++;

            batch_predecessors[0] = g_post_rmsnorm_ids[b0 / 16];
            DESC_DO_OR_SKIP(desc_task_id,  {
                new_task(__did, TASK_TYPE_CUBE, (uint32_t)cur_blocks, DUR_UP_PROJ);
                add_input(__did, post_norm_tile);
                add_input(__did, ext_w_up);
                add_inout(__did, up_piece);
                add_scalar(__did, base);
                add_predecessors(__did, batch_predecessors, 1, 0);
                up_ids[ui] = (uint32_t)__did;
            });
            ui++;

            Tensor mlp_piece = view(mlp_tile, 0u, base * 512u, 16u, (uint32_t)(cur_blocks * 512));
            batch_predecessors[0] = gate_ids[si];
            batch_predecessors[1] = up_ids[si];
            DESC_DO_OR_SKIP(desc_task_id,  {
                new_task(__did, TASK_TYPE_VECTOR, (uint32_t)cur_blocks, DUR_SILU);
                add_input(__did, gate_piece);
                add_input(__did, up_piece);
                add_inout(__did, mlp_piece);
                add_scalar(__did, base);
                add_predecessors(__did, batch_predecessors, 2, 0);
                silu_ids[si] = (uint32_t)__did;
            });
            si++;
        }

        // --- DOWN_PROJ & DOWN_PROJ_RES (+16, +17) ---
        for (int di = 0, dri = 0, base = 0; base < 40; base += qwen3_blocks_per_task(40)) {
            int cur_blocks = qwen3_cur_blocks(40, base);
            Tensor down_piece = view(down_tile, 0u, base * 128u, 16u, (uint32_t)(cur_blocks * 128));
            Tensor resid1_piece1 = view(resid1_tile, 0u, base * 128u, 16u, (uint32_t)(cur_blocks * 128));
            DESC_DO_OR_SKIP(desc_task_id,  {
                new_task(__did, TASK_TYPE_CUBE, (uint32_t)cur_blocks, DUR_DOWN_PROJ);
                add_input(__did, mlp_tile);
                add_input(__did, ext_w_down);
                add_inout(__did, down_piece);
                add_scalar(__did, base);
                add_predecessors(__did, silu_ids, (uint32_t)qwen3_n_tasks(34, qwen3_blocks_per_task(34)), 0);
                down_ids[di] = (uint32_t)__did;
            });
            di++;

            batch_predecessors[0] = down_ids[dri];
            batch_predecessors[1] = op_ids[dri];
            DESC_DO_OR_SKIP(desc_task_id,  {
                new_task(__did, TASK_TYPE_VECTOR, (uint32_t)cur_blocks, DUR_DOWN_PROJ_RES);
                add_input(__did, down_piece);
                add_input(__did, resid1_piece1);
                add_output(__did, ext_out);
                add_scalar(__did, cur_valid);
                add_scalar(__did, b0);
                add_scalar(__did, base);
                add_predecessors(__did, batch_predecessors, 2, 0);
            });
            dri++;
        }
    }
    *created_cnt = desc_created_cnt;
    return desc_task_id;
}