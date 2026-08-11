/*
 * test_ed_spmd_multiblock.c - SPMD（count>1）整任务 ED 回归测试
 *
 * 背景：一期 ED 把自己锁死在 count==1，count>1 的 SPMD 任务永远进不了 ED。
 * 放开之后语义是「整任务 ED」——整个 task 作为一个条目 stage 到单个槽位，
 * executor 照旧在该槽位上连续跑完全部 block，完成语义（一个 task 一次完成）不变。
 *
 * 本文件锁死放开过程中最容易回退的五条不变式：
 *
 *  1) try_early_dispatch 必须原子认领 0 -> count，不是 0 -> 1。
 *     写字面量 1 会让 main.c 的收尾自检（nbi != count 即 block_leaked）误报泄漏。
 *  2) 已被 ED 抢走的 SPMD 任务，send_task 必须走 skip，不得二次占槽。
 *  3) 端到端只完成一次：这是最关键的一条。若同一 task 被 ED 和 send_task 各派发
 *     一次，就会产生两条 completed 记录，resolve_dep 会把后继的 predecessor_cnt
 *     多减一次，导致后继提前 ready —— 这类 bug 极难定位。
 *  4) count==0 的 phantom 必须在 CAS 之前被拒。CAS(nbi, expect 0, desire 0) 会
 *     平凡成功并返回 true，若不显式拦，ED 会把脏槽发布进 executor。
 *  5) propagate_dispatch_fanin 必须对 count>1 的后继照样累加 fanin。
 *     这条是「覆盖率为 0」的真正原因：staging 第一道条件是 fanin == target，
 *     计数不涨则条件永不满足，其余闸门放开也没用。
 */

#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "conf.h"
#include "cutter.h"
#include "dispatch.h"
#include "early_dispatch.h"
#include "executor.h"
#include "ring_buf.h"
#include "task.h"

extern atomic_int g_completed_cnt;
extern atomic_bool g_is_done;
extern atomic_bool g_orch_is_done;
extern uint32_t g_predecessor_cnt[RING_SIZE];
extern task_state *g_state_buf;

int dispatch(int tid);
void resolve_dep(uint32_t cnt, uint32_t *cq_buf, uint32_t rq_buf[][RQ_BATCH_SIZE],
                 uint32_t *ready_cnt);

#if WORKER_LOG
extern int g_worker_log;
#endif

static int g_failures;

static void expect_true(bool cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        g_failures++;
    }
}

static void expect_u16(uint16_t got, uint16_t want, const char *msg)
{
    if (got != want) {
        fprintf(stderr, "FAIL: %s (got=%u want=%u)\n", msg, got, want);
        g_failures++;
    }
}

static void expect_u64(uint64_t got, uint64_t want, const char *msg)
{
    if (got != want) {
        fprintf(stderr, "FAIL: %s (got=%llu want=%llu)\n",
                msg, (unsigned long long)got, (unsigned long long)want);
        g_failures++;
    }
}

static void expect_int(int got, int want, const char *msg)
{
    if (got != want) {
        fprintf(stderr, "FAIL: %s (got=%d want=%d)\n", msg, got, want);
        g_failures++;
    }
}

/*
 * 本文件断言 count>1 能进 ED，因此需要 ED_SPMD_MAX_BLOCKS 至少放到 8
 * （用例里最大的 count）。旋钮被调小时应当跳过而不是报失败。
 */
#if ED_ENABLE && ED_SPMD_MAX_BLOCKS >= 8

static void sleep_ms(long ms)
{
    struct timespec req;
    req.tv_sec = ms / 1000;
    req.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&req, NULL);
}

static void reset_runtime_state(void)
{
    atomic_store_explicit(&g_task_id, 0, memory_order_relaxed);
    atomic_store_explicit(&g_min_uncomplete_task, 0, memory_order_relaxed);
    atomic_store_explicit(&g_completed_cnt, 0, memory_order_relaxed);
    atomic_store_explicit(&g_orch_is_done, false, memory_order_relaxed);
    atomic_store_explicit(&g_is_done, false, memory_order_relaxed);

    memset(g_basic_buf, 0, sizeof(g_basic_buf));
    memset(g_successor_buf, 0, sizeof(g_successor_buf));
    memset(g_predecessor_cnt, 0, sizeof(g_predecessor_cnt));
    init_predecessors();
    init_ctrl_t();
    executor_init();
    ed_init();

    if (g_state_buf == NULL) {
        init_state_buf();
    }
    for (size_t i = 0; i < RING_SIZE; i++) {
        g_state_buf[i].state = TASK_STATUS_CREATING;
        g_state_buf[i].task_id = 0;
        g_state_buf[i].successor_cnt = 0;
    }

#if WORKER_LOG
    g_worker_log = 0;
#endif
}

/* 把一个 count 任意的任务喂进 ED 流程；返回 staged record（失败为 INVALID） */
static uint64_t stage_task(uint16_t task_id, task_type_t type, uint16_t count,
                           uint16_t predecessor_cnt)
{
    uint16_t s_idx = (uint16_t)(task_id & RING_MASK);
    ed_init_task_meta(task_id, predecessor_cnt);
    g_basic_buf[s_idx].type = type;
    g_basic_buf[s_idx].count = count;
    g_basic_buf[s_idx].duration = 1;
    atomic_store_explicit(&g_spec_state[s_idx], ED_SPEC_STAGING, memory_order_release);
    expect_true(enqueue(&g_ed_ready_queue, task_id), "stage helper: enqueue should succeed");

    if (try_early_dispatch(0) != 1) {
        return ED_RECORD_INVALID;
    }
    return atomic_load_explicit(&g_staged_slot_record[s_idx], memory_order_seq_cst);
}

/* 全场扫描非 EMPTY 槽位数，用于证明"一个都没占"或"只占了一个" */
static int count_non_empty_slots(void)
{
    int n = 0;
    for (int t = 0; t < EXE_TYPE_CNT; t++) {
        for (int c = 0; c < AIC_CNT; c++) {
            for (int s = 0; s < AIC_OSTD; s++) {
                if (atomic_load_explicit(&g_executors[t][c].slot_state[s],
                                         memory_order_acquire) != EXE_SLOT_EMPTY) {
                    n++;
                }
            }
        }
    }
    return n;
}

/*
 * 不变式 1：count>1 的任务必须能 stage，且认领量是 count 而不是 1。
 */
static void test_ed_spmd_stage_claims_full_count(void)
{
    reset_runtime_state();

    const uint16_t s_id = 41;
    const uint16_t s_idx = (uint16_t)(s_id & RING_MASK);
    const uint16_t count = 4;

    uint64_t record = stage_task(s_id, TASK_TYPE_CUBE, count, 0);
    expect_true(record != ED_RECORD_INVALID,
                "spmd stage: count>1 task must be admitted into ED");
    if (record == ED_RECORD_INVALID) {
        return;
    }
    expect_true(ed_record_tag_matches(record, s_id), "spmd stage: record tag must match");

    expect_u16(atomic_load_explicit(&g_next_block_idx[s_idx], memory_order_acquire),
               count,
               "spmd stage: nbi must be claimed 0->count (literal 1 breaks block_leaked)");

    uint32_t packed = ED_RECORD_SLOT(record);
    uint16_t core = ED_UNPACK_CORE(packed);
    uint8_t slot = ED_UNPACK_SLOT(packed);
    uint8_t type = ED_UNPACK_TYPE(packed);
    expect_true(core < AIC_CNT && slot < AIC_OSTD && type < EXE_TYPE_CNT,
                "spmd stage: staged position must be valid");
    if (core >= AIC_CNT || slot >= AIC_OSTD || type >= EXE_TYPE_CNT) {
        return;
    }

    expect_u16((uint16_t)atomic_load_explicit(&g_executors[type][core].slot_state[slot],
                                              memory_order_acquire),
               (uint16_t)EXE_SLOT_GATED,
               "spmd stage: slot must be published GATED, not RUNNABLE");
    expect_int(count_non_empty_slots(), 1,
               "spmd stage: exactly one slot may be consumed");
    expect_u64(atomic_load_explicit(&g_ed_stage_cnt, memory_order_relaxed), 1,
               "spmd stage: stage_cnt should increase once");
    expect_u64(atomic_load_explicit(&g_ed_stage_spmd_cnt, memory_order_relaxed), 1,
               "spmd stage: stage_spmd_cnt should attribute this to the SPMD bucket");
}

/*
 * 不变式 2：ED 抢走之后，send_task 必须 skip，且一位槽位状态都不能改。
 */
static void test_ed_spmd_staged_task_is_skipped_by_send(void)
{
    reset_runtime_state();

    const uint16_t s_id = 47;
    const uint16_t s_idx = (uint16_t)(s_id & RING_MASK);
    const uint16_t count = 3;

    uint64_t record = stage_task(s_id, TASK_TYPE_CUBE, count, 0);
    if (record == ED_RECORD_INVALID) {
        expect_true(false, "spmd skip: precondition stage should succeed");
        return;
    }
    uint32_t packed = ED_RECORD_SLOT(record);
    uint16_t core = ED_UNPACK_CORE(packed);
    uint8_t slot = ED_UNPACK_SLOT(packed);
    uint8_t type = ED_UNPACK_TYPE(packed);

    uint64_t skip_before = atomic_load_explicit(&g_ed_send_skip_cnt, memory_order_relaxed);

    /* Hook 2 无条件把 s 推进 normal ready_queue，这里模拟那一步 */
    expect_true(enqueue(&g_ctrl_t[0].ready_queue[TASK_TYPE_CUBE], s_id),
                "spmd skip: normal ready_queue enqueue should succeed");
    int sent = dispatch(0);

    expect_int(sent, 0, "spmd skip: send_task must not dispatch an ED-claimed task");
    expect_u64(atomic_load_explicit(&g_ed_send_skip_cnt, memory_order_relaxed),
               skip_before + 1,
               "spmd skip: send_skip_cnt should increase by exactly one");
    expect_u16(atomic_load_explicit(&g_next_block_idx[s_idx], memory_order_acquire),
               count, "spmd skip: nbi stays at the ED-claimed value");
    expect_u16((uint16_t)atomic_load_explicit(&g_executors[type][core].slot_state[slot],
                                              memory_order_acquire),
               (uint16_t)EXE_SLOT_GATED,
               "spmd skip: staged slot must remain GATED, not overwritten to RUNNABLE");
    expect_int(count_non_empty_slots(), 1,
               "spmd skip: no second slot may be consumed");
}

/*
 * 不变式 3（最关键）：端到端只完成一次。
 * 走完整链路 stage -> Hook2 敲门铃 -> executor 开闸跑完全部 block -> dispatch 收口，
 * 断言 completed 计数恰好为 1。若出现重复派发，这里会是 2。
 */
static void test_ed_spmd_full_run_completes_exactly_once(void)
{
    reset_runtime_state();

    const uint16_t p_id = 51;
    const uint16_t s_id = 52;
    const uint16_t p_idx = (uint16_t)(p_id & RING_MASK);
    const uint16_t s_idx = (uint16_t)(s_id & RING_MASK);
    const uint16_t count = 3;

    /* s 有一个前驱 p，stage 时依赖未满足 —— 这才是 ED 的正常场景 */
    uint64_t record = stage_task(s_id, TASK_TYPE_CUBE, count, 1);
    if (record == ED_RECORD_INVALID) {
        expect_true(false, "spmd full-run: precondition stage should succeed");
        return;
    }
    uint32_t packed = ED_RECORD_SLOT(record);
    uint16_t core = ED_UNPACK_CORE(packed);
    uint8_t slot = ED_UNPACK_SLOT(packed);
    uint8_t type = ED_UNPACK_TYPE(packed);

    /* 建一条 p -> s 的边，供 resolve_dep 走 unfin 1->0 */
    ed_init_task_meta(p_id, 0);
    g_state_buf[p_idx].task_id = p_id;
    g_state_buf[p_idx].successor_cnt = 1;
    g_successor_buf[p_idx].cnt = 1;
    g_successor_buf[p_idx].node[0] = s_id;
    g_predecessor_cnt[s_idx] = 1;

    uint32_t rq_buf[2][RQ_BATCH_SIZE] = {{0}};
    uint32_t ready_cnt[2] = {0, 0};
    uint32_t cq[1] = {p_id};
    resolve_dep(1, cq, rq_buf, ready_cnt);

    expect_u16((uint16_t)atomic_load_explicit(&g_executors[type][core].doorbell[slot],
                                              memory_order_acquire),
               1, "spmd full-run: Hook2 should ring the doorbell exactly once");

    /* Hook 2 也会把 s 推进 normal ready_queue；照实推进去，验证不会二次派发 */
    expect_u16(ready_cnt[TASK_TYPE_CUBE], 1,
               "spmd full-run: released successor should enter ready buffer");
    expect_true(enqueue(&g_ctrl_t[0].ready_queue[TASK_TYPE_CUBE], s_id),
                "spmd full-run: normal ready_queue enqueue should succeed");

    pthread_t worker;
    int rc = pthread_create(&worker, NULL, executor_worker, NULL);
    expect_true(rc == 0, "spmd full-run: executor thread should start");
    if (rc != 0) {
        return;
    }

    /* executor 开闸后每 block ~2 tick；同时让 dispatcher 尝试重复派发 */
    for (int i = 0; i < 64; i++) {
        dispatch(0);
        sleep_ms(1);
    }
    atomic_store_explicit(&g_is_done, true, memory_order_release);
    pthread_join(worker, NULL);
    dispatch(0);

    expect_u64(atomic_load_explicit(&g_ed_gate_open_cnt, memory_order_relaxed), 1,
               "spmd full-run: gate should open exactly once");
    expect_u16((uint16_t)atomic_load_explicit(&g_executors[type][core].slot_state[slot],
                                              memory_order_acquire),
               (uint16_t)EXE_SLOT_EMPTY,
               "spmd full-run: slot returns to EMPTY after all blocks finish");
    expect_true(g_executors[type][core].block_idx[slot] == 0,
                "spmd full-run: block_idx reset on completion");
    expect_int(count_non_empty_slots(), 0,
               "spmd full-run: no slot may be left occupied");
    expect_int((int)atomic_load_explicit(&g_completed_cnt, memory_order_relaxed), 1,
               "spmd full-run: task must complete EXACTLY once (2 => double dispatch)");
    expect_u16(atomic_load_explicit(&g_next_block_idx[s_idx], memory_order_acquire),
               count, "spmd full-run: nbi == count keeps block_leaked self-check clean");
}

/*
 * 不变式 4：count==0 的 phantom 必须在 CAS 之前被拒。
 */
static void test_ed_rejects_phantom_zero_count(void)
{
    reset_runtime_state();

    const uint16_t s_id = 61;
    const uint16_t s_idx = (uint16_t)(s_id & RING_MASK);

    ed_init_task_meta(s_id, 0);
    g_basic_buf[s_idx].type = TASK_TYPE_CUBE;
    g_basic_buf[s_idx].count = 0;
    g_basic_buf[s_idx].duration = 1;
    atomic_store_explicit(&g_spec_state[s_idx], ED_SPEC_STAGING, memory_order_release);
    expect_true(enqueue(&g_ed_ready_queue, s_id), "phantom: enqueue should succeed");

    int staged = try_early_dispatch(0);

    expect_int(staged, 0, "phantom: count==0 must never be staged");
    expect_u16(atomic_load_explicit(&g_next_block_idx[s_idx], memory_order_acquire), 0,
               "phantom: nbi must stay 0 (CAS 0->0 succeeds trivially!)");
    expect_int(count_non_empty_slots(), 0,
               "phantom: no slot may be published for a phantom task");
    expect_u64(atomic_load_explicit(&g_ed_stage_cnt, memory_order_relaxed), 0,
               "phantom: stage_cnt must stay 0");
}

/*
 * 不变式 5：Hook 0 必须给 count>1 的后继累加 fanin，否则它永远进不了 staging。
 */
static void test_ed_fanin_propagates_to_spmd_successor(void)
{
    reset_runtime_state();

    const uint16_t p_id = 71;
    const uint16_t s_id = 72;
    const uint16_t p_idx = (uint16_t)(p_id & RING_MASK);
    const uint16_t s_idx = (uint16_t)(s_id & RING_MASK);

    /* s 是 count=8 的 SPMD 任务，单前驱 p */
    ed_init_task_meta(s_id, 1);
    g_basic_buf[s_idx].type = TASK_TYPE_CUBE;
    g_basic_buf[s_idx].count = 8;
    g_basic_buf[s_idx].duration = 1;

    /* ed_init_task_meta 会清 successor list，故必须先初始化 p 再建边 */
    ed_init_task_meta(p_id, 0);
    g_basic_buf[p_idx].type = TASK_TYPE_CUBE;
    g_basic_buf[p_idx].count = 1;
    g_successor_buf[p_idx].cnt = 1;
    g_successor_buf[p_idx].node[0] = s_id;

    propagate_dispatch_fanin(p_id);

    expect_u16(atomic_load_explicit(&g_dispatch_fanin[s_idx], memory_order_acquire), 1,
               "hook0: fanin must be accumulated for count>1 successors too");
    expect_u16((uint16_t)atomic_load_explicit(&g_spec_state[s_idx], memory_order_acquire),
               (uint16_t)ED_SPEC_STAGING,
               "hook0: SPMD successor should enter STAGING once fanin meets target");

    uint32_t queued = 0;
    expect_true(dequeue(&g_ed_ready_queue, &queued),
                "hook0: SPMD successor should be queued for try_early_dispatch");
    expect_u16(queued, s_id, "hook0: queued id should be the SPMD successor");
}

int main(void)
{
    test_ed_spmd_stage_claims_full_count();
    test_ed_spmd_staged_task_is_skipped_by_send();
    test_ed_spmd_full_run_completes_exactly_once();
    test_ed_rejects_phantom_zero_count();
    test_ed_fanin_propagates_to_spmd_successor();

    if (g_failures == 0) {
        printf("PASS: ed spmd multi-block (count>1) task-level early dispatch\n");
        return EXIT_SUCCESS;
    }
    fprintf(stderr, "%d check(s) failed\n", g_failures);
    return EXIT_FAILURE;
}

#else /* ED 关闭，或 ED_SPMD_MAX_BLOCKS 被调到 8 以下 */

int main(void)
{
    printf("SKIP: ed spmd multi-block test needs ED_ENABLE=1 and ED_SPMD_MAX_BLOCKS >= 8\n");
    return EXIT_SUCCESS;
}

#endif
