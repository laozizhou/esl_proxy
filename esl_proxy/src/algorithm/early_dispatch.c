/*
 * early_dispatch.c - Early-dispatch 一期基础设施实现（Step 1）
 *
 * 本文件提供：ed 全局变量定义、ed_init、generation-tagged record helper、
 * per-pred 边锁、ring 换代元数据重置（ed_init_task_meta）。
 * Hook 0/1/2 在 Step 4–7 逐步接入；此处仅 stub 保证链接通过。
 */

#include "early_dispatch.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "dispatch.h"
#include "executor.h"
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
#if ED_ENABLE
    uint16_t s_idx = (uint16_t)(task_id & RING_MASK);
    /*
     * Step 7 ABA 防护：
     * 1) ring_task_tag 必须仍指向本代 task_id（拒绝旧代迟到通知）；
     * 2) staged_slot_record 必须与调用方 record 完全一致（拒绝旧位置误放行）。
     */
    uint32_t live_tag = atomic_load_explicit(&g_ring_task_tag[s_idx], memory_order_acquire);
    if (live_tag != task_id) {
        return;
    }
    uint64_t live_record =
        atomic_load_explicit(&g_staged_slot_record[s_idx], memory_order_seq_cst);
    if (live_record != record) {
        return;
    }
    if (!ed_record_tag_matches(record, task_id)) {
        return;
    }

    uint32_t packed = ED_RECORD_SLOT(record);
    uint16_t core = ED_UNPACK_CORE(packed);
    uint8_t slot = ED_UNPACK_SLOT(packed);
    uint8_t type = ED_UNPACK_TYPE(packed);
    if (core >= AIC_CNT || slot >= AIC_OSTD || type >= EXE_TYPE_CNT) {
        return;
    }

    uint8_t expected_claim = 0;
    if (!atomic_compare_exchange_strong_explicit(
            &g_notify_claimed[s_idx], &expected_claim, 1,
            memory_order_acq_rel, memory_order_acquire)) {
        return;
    }

    uint8_t expected_state = EXE_SLOT_GATED;
    if (!atomic_compare_exchange_strong_explicit(
            &g_executors[type][core].slot_state[slot], &expected_state,
            EXE_SLOT_RUNNABLE, memory_order_acq_rel, memory_order_acquire)) {
        return;
    }

    atomic_store_explicit(&g_executors[type][core].doorbell[slot], 1, memory_order_release);
    if (source == ED_NOTIFY_HOOK2) {
        atomic_fetch_add_explicit(&g_ed_hit_cnt, 1, memory_order_relaxed);
    } else {
        atomic_fetch_add_explicit(&g_ed_self_notify_cnt, 1, memory_order_relaxed);
    }
#else
    (void)task_id;
    (void)record;
    (void)source;
#endif
}

#if ED_ENABLE
static __thread unsigned int s_ed_rand_seed;

static inline void ed_enqueue_or_abandon(uint16_t task_id)
{
    if (enqueue(&g_ed_ready_queue, task_id)) {
        return;
    }

    uint16_t s_idx = (uint16_t)(task_id & RING_MASK);
    uint8_t expected = ED_SPEC_STAGING;
    (void)atomic_compare_exchange_strong_explicit(
        &g_spec_state[s_idx], &expected, ED_SPEC_NONE,
        memory_order_acq_rel, memory_order_acquire);
}

static inline void ed_maybe_enter_staging(uint16_t s_id, uint16_t s_idx, uint16_t fanin_now)
{
    if (fanin_now != g_dispatch_fanin_target[s_idx]) {
        return;
    }
    if (g_basic_buf[s_idx].count != 1) {
        return;
    }

    uint16_t unfin = atomic_load_explicit(&g_unfin_pred_cnt[s_idx], memory_order_acquire);
    if (unfin > ED_UNFIN_THRESHOLD) {
        return;
    }

    uint8_t expected = ED_SPEC_NONE;
    if (atomic_compare_exchange_strong_explicit(
            &g_spec_state[s_idx], &expected, ED_SPEC_STAGING,
            memory_order_acq_rel, memory_order_relaxed)) {
        ed_enqueue_or_abandon(s_id);
    }
}

void propagate_dispatch_fanin(uint16_t p_id)
{
    uint16_t p_idx = (uint16_t)(p_id & RING_MASK);
    ed_edge_lock(p_idx);
    if (atomic_load_explicit(&g_ring_task_tag[p_idx], memory_order_acquire) != (uint32_t)p_id) {
        ed_edge_unlock(p_idx);
        return;
    }

    /* 持久记录“该 generation 曾 dispatch”；完成时不清。 */
    atomic_store_explicit(&g_dispatch_tag[p_idx], (uint32_t)p_id, memory_order_release);

    uint16_t succ_cnt = g_successor_buf[p_idx].cnt;
    for (uint16_t k = 0; k < succ_cnt; k++) {
        uint16_t s_id = g_successor_buf[p_idx].node[k];
        uint16_t s_idx = (uint16_t)(s_id & RING_MASK);

        if (g_basic_buf[s_idx].count != 1) {
            continue;
        }

        uint16_t fanin_now = (uint16_t)(atomic_fetch_add_explicit(
                                 &g_dispatch_fanin[s_idx], 1, memory_order_relaxed) +
                             1);
#if ED_HOOK0_CONTRIB_STATS
        atomic_fetch_add_explicit(&g_ed_hook0_contrib_cnt, 1, memory_order_relaxed);
#endif
        ed_maybe_enter_staging(s_id, s_idx, fanin_now);
    }

    ed_edge_unlock(p_idx);
}

static int pick_stage_core(int tid, uint16_t s_id, task_type_t type, int *out_slot)
{
    uint16_t s_idx = (uint16_t)(s_id & RING_MASK);
    uint64_t pcore_bitmap = 0;
    ed_pred_snapshot_t *snap = &g_ed_pred_snapshot[s_idx];
    for (uint16_t k = 0; k < snap->cnt; k++) {
        uint16_t p_id = snap->node[k];
        uint16_t p_idx = (uint16_t)(p_id & RING_MASK);
        if (g_state_buf != NULL &&
            g_state_buf[p_idx].state == TASK_STATUS_COMPLETED) {
            continue;
        }

        uint64_t record = ed_task_dispatch_record_load(p_id);
        if (!ed_record_tag_matches(record, p_id)) {
            continue;
        }

        uint32_t packed = ED_RECORD_SLOT(record);
        if ((task_type_t)ED_UNPACK_TYPE(packed) != type) {
            continue;
        }
        uint16_t core = ED_UNPACK_CORE(packed);
        if (core >= AIC_CNT) {
            continue;
        }
        pcore_bitmap |= ((uint64_t)1u << core);
    }

    uint64_t free0 = atomic_load_explicit(&g_ctrl_t[tid].free_bitmap[type][0],
                                          memory_order_acquire);
    uint64_t free1 = atomic_load_explicit(&g_ctrl_t[tid].free_bitmap[type][1],
                                          memory_order_acquire);
    uint64_t free_any = free0 | free1;
    uint64_t candidate = pcore_bitmap & free_any;
    if (candidate == 0) {
        candidate = free_any;
    }
    if (candidate == 0) {
        return -1;
    }

    if (s_ed_rand_seed == 0) {
        s_ed_rand_seed = ((unsigned int)tid + 1u) ^ ((unsigned int)s_id << 8);
        if (s_ed_rand_seed == 0) {
            s_ed_rand_seed = 1u;
        }
    }
    int popcnt = __builtin_popcountll(candidate);
    int nth = (int)(rand_r(&s_ed_rand_seed) % (unsigned int)popcnt);
    int core = pick_nth_bit(candidate, nth);
    uint64_t mask = (uint64_t)1u << (uint64_t)core;
    *out_slot = (free0 & mask) != 0 ? 0 : 1;
    return core;
}

int try_early_dispatch(int tid)
{
    uint16_t s_id = 0;
    if (!dequeue(&g_ed_ready_queue, &s_id)) {
        return 0;
    }

    uint16_t s_idx = (uint16_t)(s_id & RING_MASK);
    if (atomic_load_explicit(&g_spec_state[s_idx], memory_order_seq_cst) !=
        ED_SPEC_STAGING) {
        return 0;
    }

    if (g_basic_buf[s_idx].count != 1) {
        return 0;
    }

    task_type_t type = g_basic_buf[s_idx].type;
    int slot = -1;
    int core = pick_stage_core(tid, s_id, type, &slot);
    if (core < 0) {
        goto re_push_slot_busy;
    }

    uint64_t core_mask = (uint64_t)1u << (uint64_t)core;
    uint64_t old_bm = atomic_fetch_and_explicit(
        &g_ctrl_t[tid].free_bitmap[type][slot], ~core_mask, memory_order_acq_rel);
    if ((old_bm & core_mask) == 0) {
        goto re_push_slot_busy;
    }

    uint16_t expected_blk = 0;
    if (!atomic_compare_exchange_strong_explicit(
            &g_next_block_idx[s_idx], &expected_blk, 1, memory_order_acq_rel,
            memory_order_relaxed)) {
        atomic_fetch_or_explicit(&g_ctrl_t[tid].free_bitmap[type][slot], core_mask,
                                 memory_order_release);
        atomic_fetch_add_explicit(&g_ed_block_cas_fail_cnt, 1, memory_order_relaxed);
        return 0;
    }

    assert(atomic_load_explicit(&g_executors[type][core].slot_state[slot],
                                memory_order_acquire) == EXE_SLOT_EMPTY);
    atomic_store_explicit(&g_executors[type][core].doorbell[slot], 0,
                          memory_order_relaxed);
    g_executors[type][core].tasks[slot] = s_id;
    g_executors[type][core].block_idx[slot] = 0;
    uint32_t raw_duration = g_basic_buf[s_idx].duration;
    g_executors[type][core].duration[slot] =
        (raw_duration > 10000) ? (uint16_t)(raw_duration / 10000) : 1;
    if (slot == 1) {
        g_ctrl_t[tid].task_id_map2[type][core] = s_id;
    } else {
        g_ctrl_t[tid].task_id_map1[type][core] = s_id;
    }
    atomic_store_explicit(&g_executors[type][core].slot_state[slot],
                          EXE_SLOT_GATED, memory_order_release);

    uint32_t packed = ED_PACK_SLOT((uint16_t)core, (uint8_t)slot, (uint8_t)type);
    uint64_t record = ED_PACK_RECORD((uint32_t)s_id, packed);
    atomic_store_explicit(&g_staged_slot_record[s_idx], record, memory_order_seq_cst);
    atomic_fetch_add_explicit(&g_ed_stage_cnt, 1, memory_order_relaxed);

    if (atomic_load_explicit(&g_spec_state[s_idx], memory_order_seq_cst) ==
        ED_SPEC_DISPATCHED) {
        ed_notify_once(s_id, record, ED_NOTIFY_HOOK1);
    }
    return 1;

re_push_slot_busy:
    if (!enqueue(&g_ed_ready_queue, s_id)) {
        uint8_t expected = ED_SPEC_STAGING;
        (void)atomic_compare_exchange_strong_explicit(
            &g_spec_state[s_idx], &expected, ED_SPEC_NONE, memory_order_acq_rel,
            memory_order_acquire);
    }
    atomic_fetch_add_explicit(&g_ed_slot_retry_cnt, 1, memory_order_relaxed);
    return 0;
}
#endif
