// Orchestration Function: qwen3_decode "detect" stage.
//
// Consumes g_task_tensor_buf (+ optional g_insert_tickets from describe) and
// re-derives tensor-overlap dependency edges as parallel detect threads.
//
// Insert-ticket 类型/开关定义在 qwen3_14b_decoder_desc.h（本文件用
// QWEN3_DESC_TYPES_ONLY 只拉类型，不拉 describe 图逻辑）。
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "mem_pool.h"
#include "orch_config.h"
#include "tensormap_core.h"
#include "ring_buf.h"

#define QWEN3_DESC_TYPES_ONLY 1
#include "qwen3_14b_decoder_desc.h"
#undef QWEN3_DESC_TYPES_ONLY

#ifndef DETECT_TMD_POOL_SIZE
#define DETECT_TMD_POOL_SIZE 8192u
#endif
#ifndef DETECT_TMD_NUM_BUCKETS
#define DETECT_TMD_NUM_BUCKETS ORCH_TM_NUM_BUCKETS
#endif
#ifndef DETECT_TMD_TASK_WINDOW
#define DETECT_TMD_TASK_WINDOW RING_SIZE
#endif
#ifndef DETECT_PENDING_MAX_PRED
#define DETECT_PENDING_MAX_PRED 64u
#endif

#define DETECT_TMD_BUF_BYTES                                                    \
    (sizeof(TmHeader) + (uint64_t)DETECT_TMD_NUM_BUCKETS * sizeof(int32_t) +    \
        TM_ENTRY_ALIGN + (uint64_t)DETECT_TMD_POOL_SIZE * sizeof(TmEntry) +     \
        (uint64_t)DETECT_TMD_POOL_SIZE * sizeof(int32_t) +                     \
        (uint64_t)DETECT_TMD_TASK_WINDOW * sizeof(int32_t))

extern struct task_tensor_desc g_task_tensor_buf[RING_SIZE];
#if ORCH_USE_INSERT_TICKETS
extern struct task_insert_tickets g_insert_tickets[RING_SIZE];
#endif

typedef struct {
    TmTensorMap map;
    _Alignas(TM_ENTRY_ALIGN) uint8_t buf[DETECT_TMD_BUF_BYTES];
} DetectTmState;

typedef struct {
    uint32_t consumer;
    uint32_t preds[DETECT_PENDING_MAX_PRED];
    int pn;
    bool is_inout;
    TmTensorMap *map;
} DetectCollectCtx;

static inline bool detect_collect_on_match(TmEntry *e, TmOverlap ov, void *ctx) {
    DetectCollectCtx *c = (DetectCollectCtx *)ctx;
    const uint32_t p = (uint32_t)tm_local_of(e->producer_id);
    if (p != c->consumer) {
        for (int i = 0; i < c->pn; i++) {
            if (c->preds[i] == p) {
                goto after_pred;
            }
        }
        if (c->pn < (int)DETECT_PENDING_MAX_PRED) {
            c->preds[c->pn++] = p;
        }
    }
after_pred:
    if (c->is_inout && ov == TM_OVERLAP_COVERED) {
        tm_remove(c->map, e);
    }
    return true;
}

/* 消费 describe 预填的 TmEntry：等价于 tm_insert_tensor，但跳过 copy。 */
static inline void detect_insert_prepared_entry(TmTensorMap *map,
    const TmEntry *prepared, uint32_t tid)
{
    const int32_t idx = tm_new_entry(map);
    if (idx < 0) {
        return;
    }
    TmEntry *e = &tm_pool(map)[idx];
    memcpy(e, prepared, sizeof(TmEntry));
    /* link 字段由 tm_link_entry 重写；hash/bucket 也在这里算。 */
    tm_link_entry(map, idx, prepared->base_addr, tm_make_id(0, tid));
}

static inline void detect_insert_outs(TmTensorMap *map, uint32_t task_id) {
    struct task_tensor_desc *td = &g_task_tensor_buf[task_id & RING_MASK];
#if ORCH_USE_INSERT_TICKETS
    struct task_insert_tickets *tk = &g_insert_tickets[task_id & RING_MASK];
    for (uint32_t i = 0; i < td->out_cnt; i++) {
        detect_insert_prepared_entry(map, &tk->out[i].entry, task_id);
    }
    for (uint32_t i = 0; i < td->inout_cnt; i++) {
        detect_insert_prepared_entry(map, &tk->inout[i].entry, task_id);
    }
#else
    /* 基线：copy+hash+link 全在 detect 的 tm_insert_tensor 内。 */
    for (uint32_t i = 0; i < td->out_cnt; i++) {
        tm_insert_tensor(map, &td->out_data[i], task_id);
    }
    for (uint32_t i = 0; i < td->inout_cnt; i++) {
        tm_insert_tensor(map, &td->inout_data[i], task_id);
    }
#endif
}

static inline void prepare_hash(TmTensorMap *map, uint32_t task_id) {
    struct task_tensor_desc *td = &g_task_tensor_buf[task_id & RING_MASK];

    DetectCollectCtx ctx = {.consumer = task_id, .pn = 0, .is_inout = true,
        .map = map};
    for (uint32_t i = 0; i < td->inout_cnt; i++) {
        tm_lookup_tensor(map, &td->inout_data[i], detect_collect_on_match, &ctx);
    }
    detect_insert_outs(map, task_id);
}

static inline void detect_task(TmTensorMap *map, uint32_t task_id) {
    struct task_tensor_desc *td = &g_task_tensor_buf[task_id & RING_MASK];
    DetectCollectCtx ctx = {.consumer = task_id, .pn = 0, .map = map};

    for (uint32_t i = 0; i < td->in_cnt; i++) {
        ctx.is_inout = false;
        tm_lookup_tensor(map, &td->in_data[i], detect_collect_on_match, &ctx);
    }
    for (uint32_t i = 0; i < td->inout_cnt; i++) {
        ctx.is_inout = true;
        tm_lookup_tensor(map, &td->inout_data[i], detect_collect_on_match, &ctx);
    }

    if (ctx.pn > 0) {
        add_predecessors(task_id, ctx.preds, (uint32_t)ctx.pn, 0);
    }
    detect_insert_outs(map, task_id);
}

extern int detect_thread_count;
extern int detect_batch_size;

int orchestrator_detect(const uint64_t orch_args, int thread_id, int total_task_cnt,
    int *created_cnt) {
    (void)orch_args;

    DetectTmState state;
    TmConfig cfg;
    cfg.num_buckets = DETECT_TMD_NUM_BUCKETS;
    cfg.pool_size = DETECT_TMD_POOL_SIZE;
    cfg.num_rings = 1;
    cfg.task_window[0] = DETECT_TMD_TASK_WINDOW;
    for (uint32_t r = 1; r < TM_MAX_RINGS; r++) {
        cfg.task_window[r] = 1;
    }
    tm_init(&state.map, state.buf, &cfg);

    int detect_start = thread_id * detect_batch_size;
    int detect_end = detect_start + detect_batch_size;
    int published_cnt = 0;
    int build_start = 0;
    while (detect_start < total_task_cnt) {
        if (detect_end > total_task_cnt) {
            detect_end = total_task_cnt;
        }
        for (int task_id = build_start; task_id < detect_start; task_id++) {
            prepare_hash(&state.map, (uint32_t)task_id);
        }
        for (int task_id = detect_start; task_id < detect_end; task_id++) {
            detect_task(&state.map, (uint32_t)task_id);
        }
        published_cnt += detect_end - detect_start;
        build_start = detect_end;
        detect_start += detect_thread_count * detect_batch_size;
        detect_end = detect_start + detect_batch_size;
    }

    *created_cnt = published_cnt;
    return total_task_cnt;
}
