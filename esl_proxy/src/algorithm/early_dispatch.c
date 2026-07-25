/*
 * early_dispatch.c - Early-dispatch 一期基础设施实现（Step 1）
 *
 * 本文件提供：ed 全局变量定义、ed_init、generation-tagged record helper、
 * per-pred 边锁、ring 换代元数据重置（ed_init_task_meta）。
 * Hook 0/1/2 在 Step 4–7 逐步接入；此处仅 stub 保证链接通过。
 */

#include "early_dispatch.h"

#include <string.h>

#include "log.h"
#include "ring_buf.h"
#include "spin.h"
#include "task.h"

/* cutter.c 中分配；ed_init_task_meta 需清 successor_cnt */
extern task_state *g_state_buf;

/* -------------------------------------------------------------------------
 * 全局变量定义
 * ------------------------------------------------------------------------- */
_Atomic uint16_t  g_dispatch_fanin[RING_SIZE];
uint16_t          g_dispatch_fanin_target[RING_SIZE];
_Atomic uint16_t  g_unfin_pred_cnt[RING_SIZE];
_Atomic uint8_t   g_spec_state[RING_SIZE];
_Atomic uint64_t  g_staged_slot_record[RING_SIZE];
_Atomic uint8_t   g_notify_claimed[RING_SIZE];
_Atomic uint16_t  g_next_block_idx[RING_SIZE];
_Atomic uint64_t  g_task_dispatch_record[RING_SIZE];
_Atomic uint32_t  g_ring_task_tag[RING_SIZE];
_Atomic uint32_t  g_dispatch_tag[RING_SIZE];
atomic_flag       g_ed_edge_lock[RING_SIZE];
ed_pred_snapshot_t g_ed_pred_snapshot[RING_SIZE];
queue_t           g_ed_ready_queue;

_Atomic uint64_t g_ed_stage_cnt;
_Atomic uint64_t g_ed_hit_cnt;
_Atomic uint64_t g_ed_self_notify_cnt;
_Atomic uint64_t g_ed_slot_retry_cnt;
_Atomic uint64_t g_ed_block_cas_fail_cnt;
_Atomic uint64_t g_ed_send_skip_cnt;
_Atomic uint64_t g_ed_late_arrival_cnt;

#if ED_HOOK0_CONTRIB_STATS
_Atomic uint64_t g_ed_hook0_contrib_cnt;
#endif

/* -------------------------------------------------------------------------
 * 边锁：Hook 0 与 add_successors 第二趟共用，持锁时间极短
 * ------------------------------------------------------------------------- */
void ed_edge_lock(uint16_t task_idx)
{
    while (atomic_flag_test_and_set_explicit(&g_ed_edge_lock[task_idx],
                                             memory_order_acquire)) {
        spin_wait();
    }
}

void ed_edge_unlock(uint16_t task_idx)
{
    atomic_flag_clear_explicit(&g_ed_edge_lock[task_idx], memory_order_release);
}

/* -------------------------------------------------------------------------
 * generation tag 校验
 * ------------------------------------------------------------------------- */
bool ed_record_tag_matches(uint64_t record, uint32_t task_id)
{
    return ED_RECORD_TAG(record) == task_id;
}

/* -------------------------------------------------------------------------
 * g_task_dispatch_record：瞬时 dispatch 位置（完成时清；tag 不匹配则跳过）
 * ------------------------------------------------------------------------- */
uint64_t ed_task_dispatch_record_load(uint32_t task_id)
{
    uint16_t idx = (uint16_t)(task_id & RING_MASK);
    return atomic_load_explicit(&g_task_dispatch_record[idx], memory_order_acquire);
}

void ed_task_dispatch_record_store(uint32_t task_id, int core, int slot, int type)
{
    uint16_t idx = (uint16_t)(task_id & RING_MASK);
    uint32_t packed = ED_PACK_SLOT((uint16_t)core, (uint8_t)slot, (uint8_t)type);
    uint64_t record = ED_PACK_RECORD(task_id, packed);

    atomic_store_explicit(&g_task_dispatch_record[idx], record, memory_order_release);
}

void ed_task_dispatch_record_clear(uint32_t task_id)
{
    uint16_t idx = (uint16_t)(task_id & RING_MASK);
    uint64_t expected = atomic_load_explicit(&g_task_dispatch_record[idx],
                                            memory_order_acquire);

    if (!ed_record_tag_matches(expected, task_id)) {
        /* ring slot 已被下一代覆写，或 record 从未写入：拒绝清除，防误清新代 */
        WORKER_LOGF("stale_tag, dispatch_record_clear skipped, task=%u, record_tag=%u",
                    task_id, ED_RECORD_TAG(expected));
        return;
    }

    /*
     * CAS 清除：load 与 store 之间若同 slot 被新代覆写，CAS 失败并跳过，
     * 避免 TOCTOU 误清新代瞬时位置（rev Round 1 必修项）。
     */
    uint64_t invalid = ED_RECORD_INVALID;
    if (!atomic_compare_exchange_strong_explicit(
            &g_task_dispatch_record[idx], &expected, invalid,
            memory_order_acq_rel, memory_order_acquire)) {
        WORKER_LOGF("stale_tag, dispatch_record_clear cas failed, task=%u, saw_tag=%u",
                    task_id, ED_RECORD_TAG(expected));
    }
}

/* -------------------------------------------------------------------------
 * ed_init_task_meta：ring slot 换代时一次性重置全部代际字段（T0 事件，§5.20）
 * ------------------------------------------------------------------------- */
void ed_init_task_meta(uint32_t full_task_id, uint16_t predecessor_cnt)
{
#if ED_ENABLE
    uint16_t task_idx = (uint16_t)(full_task_id & RING_MASK);

    /*
     * 锁内清 outgoing edge 与 ring_task_tag，与 Hook 0 / add_successors 第二趟
     * 共用同一把 s 的边锁，防止 append 与换代 tearing。
     */
    ed_edge_lock(task_idx);
    atomic_store_explicit(&g_ring_task_tag[task_idx], full_task_id,
                          memory_order_release);
    atomic_store_explicit(&g_dispatch_tag[task_idx], ED_TASK_TAG_INVALID,
                          memory_order_relaxed);
    g_successor_buf[task_idx].cnt = 0;
    if (g_state_buf != NULL) {
        g_state_buf[task_idx].successor_cnt = 0;
    }
    ed_edge_unlock(task_idx);

    g_dispatch_fanin_target[task_idx] = predecessor_cnt;
    atomic_store_explicit(&g_dispatch_fanin[task_idx], 0, memory_order_relaxed);
    atomic_store_explicit(&g_unfin_pred_cnt[task_idx], predecessor_cnt,
                          memory_order_relaxed);
    atomic_store_explicit(&g_spec_state[task_idx], ED_SPEC_NONE,
                          memory_order_relaxed);
    atomic_store_explicit(&g_next_block_idx[task_idx], 0, memory_order_relaxed);
    atomic_store_explicit(&g_staged_slot_record[task_idx], ED_RECORD_INVALID,
                          memory_order_relaxed);
    atomic_store_explicit(&g_task_dispatch_record[task_idx], ED_RECORD_INVALID,
                          memory_order_relaxed);
    atomic_store_explicit(&g_notify_claimed[task_idx], 0, memory_order_relaxed);
    g_ed_pred_snapshot[task_idx].cnt = 0;
#else
    (void)full_task_id;
    (void)predecessor_cnt;
#endif
}

/* -------------------------------------------------------------------------
 * ed_init：进程启动时调用，所有 ring 表项置 INVALID
 * ------------------------------------------------------------------------- */
void ed_init(void)
{
    for (int i = 0; i < RING_SIZE; i++) {
        atomic_init(&g_dispatch_fanin[i], 0);
        g_dispatch_fanin_target[i] = 0;
        atomic_init(&g_unfin_pred_cnt[i], 0);
        atomic_init(&g_spec_state[i], ED_SPEC_NONE);
        atomic_init(&g_staged_slot_record[i], ED_RECORD_INVALID);
        atomic_init(&g_notify_claimed[i], 0);
        atomic_init(&g_next_block_idx[i], 0);
        atomic_init(&g_task_dispatch_record[i], ED_RECORD_INVALID);
        atomic_init(&g_ring_task_tag[i], ED_TASK_TAG_INVALID);
        atomic_init(&g_dispatch_tag[i], ED_TASK_TAG_INVALID);
        atomic_flag_clear(&g_ed_edge_lock[i]);
        g_ed_pred_snapshot[i].cnt = 0;
    }

    memset(&g_ed_ready_queue, 0, sizeof g_ed_ready_queue);
    atomic_flag_clear(&g_ed_ready_queue.lock);

    atomic_store_explicit(&g_ed_stage_cnt, 0, memory_order_relaxed);
    atomic_store_explicit(&g_ed_hit_cnt, 0, memory_order_relaxed);
    atomic_store_explicit(&g_ed_self_notify_cnt, 0, memory_order_relaxed);
    atomic_store_explicit(&g_ed_slot_retry_cnt, 0, memory_order_relaxed);
    atomic_store_explicit(&g_ed_block_cas_fail_cnt, 0, memory_order_relaxed);
    atomic_store_explicit(&g_ed_send_skip_cnt, 0, memory_order_relaxed);
    atomic_store_explicit(&g_ed_late_arrival_cnt, 0, memory_order_relaxed);
#if ED_HOOK0_CONTRIB_STATS
    atomic_store_explicit(&g_ed_hook0_contrib_cnt, 0, memory_order_relaxed);
#endif
}

/* Step 6 接入；Step 1 空实现 */
void ed_notify_once(uint32_t task_id, uint64_t record, ed_notify_source_t source)
{
    (void)task_id;
    (void)record;
    (void)source;
}

#if ED_ENABLE
void propagate_dispatch_fanin(uint16_t p_id)
{
    (void)p_id;
}

int try_early_dispatch(int tid)
{
    (void)tid;
    return 0;
}
#endif
