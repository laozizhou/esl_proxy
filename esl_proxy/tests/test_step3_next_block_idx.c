/*
 * test_step3_next_block_idx.c - Step 3 回归测试
 *
 * 覆盖 §3.1 B1 / §6 Step 3 的整任务原子认领 (0 -> count) 协议：
 *
 *  1) count==1 时 send_task CAS 成功后 g_next_block_idx[T]==1，且 slot/free bit
 *     被正常消费；再次入队同 task 时第二次 send_task 必须走 skip 分支：
 *       - 返回 sent==0
 *       - g_ed_send_skip_cnt 自增
 *       - free_bitmap / slot_state / task_id_map 不被二次消费
 *  2) count>1 时 CAS 一次性写 0 -> count（而不是 0 -> 1），只派发一次；
 *     配合 executor 线程能完整跑完全部 block 并置 msg_bitmap done bit。
 *  3) CAS 已被 stager 预占（nbi[T]==count）时 send_task 必须：
 *       - 返回 sent==0
 *       - g_ed_send_skip_cnt 自增
 *       - free_bitmap / slot_state / task_id_map 保持原状（不消费 slot）
 *
 * RED 期望：Step 2 基线里 send_task 完全不动 g_next_block_idx，也不递增
 * g_ed_send_skip_cnt，所以 (1) 中的 nbi==1、(2) 中的 nbi==count、(3) 中的
 * skip_cnt++/side-effect-free 三条主要断言都会失败。
 */

#define _POSIX_C_SOURCE 199309L

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "conf.h"
#include "dispatch.h"
#include "early_dispatch.h"
#include "executor.h"
#include "ring_buf.h"
#include "task.h"

extern atomic_int g_completed_cnt;
extern atomic_bool g_is_done;
extern atomic_bool g_orch_is_done;
int dispatch(int tid);

/*
 * g_state_buf 平常在 cutter.c 定义；本测试不联 cutter.o，
 * 提供本地 stub 让 ed_init_task_meta 的 NULL 分支之外也能安全跑到（这里只需存在符号）。
 */
static task_state g_state_stub[RING_SIZE];
task_state *g_state_buf = g_state_stub;

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
                msg,
                (unsigned long long)got,
                (unsigned long long)want);
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

#if ED_ENABLE
static void sleep_ms(long ms)
{
    struct timespec req;
    req.tv_sec = ms / 1000;
    req.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&req, NULL);
}

/*
 * reset_runtime_state:
 *   把跨用例可能残留的 dispatcher / executor / ed 全局状态重置。
 *   这里不用 cutter，测试范围限定在 send_task 的 Step 3 行为。
 */
static void reset_runtime_state(void)
{
    atomic_store_explicit(&g_task_id, 0, memory_order_relaxed);
    atomic_store_explicit(&g_completed_cnt, 0, memory_order_relaxed);
    atomic_store_explicit(&g_orch_is_done, false, memory_order_relaxed);
    atomic_store_explicit(&g_is_done, false, memory_order_relaxed);

    memset(g_basic_buf, 0, sizeof(g_basic_buf));
    init_predecessors();
    init_ctrl_t();
    executor_init();
    ed_init();

#if WORKER_LOG
    g_worker_log = 0;
#endif
}

/*
 * Test 0：count==0 的脏槽必须直接 skip，不允许 baseline 派发。
 *   断言 sent==0，且 free/msg bitmap、slot_state、task_id_map、nbi 全不变。
 */
static void test_step3_count_zero_skips_without_side_effects(void)
{
    reset_runtime_state();

    const int type = TASK_TYPE_CUBE;
    const uint16_t task_id = 3;
    const uint16_t s_idx = task_id & RING_MASK;
    g_basic_buf[s_idx].count = 0;
    g_basic_buf[s_idx].duration = 1;

    uint64_t free0_before = atomic_load_explicit(&g_ctrl_t[0].free_bitmap[type][0],
                                                 memory_order_relaxed);
    uint64_t free1_before = atomic_load_explicit(&g_ctrl_t[0].free_bitmap[type][1],
                                                 memory_order_relaxed);
    uint64_t msg0_before = atomic_load_explicit(&g_ctrl_t[0].msg_bitmap[type][0],
                                                memory_order_relaxed);
    uint64_t msg1_before = atomic_load_explicit(&g_ctrl_t[0].msg_bitmap[type][1],
                                                memory_order_relaxed);
    uint16_t map1_before[AIC_CNT];
    uint16_t map2_before[AIC_CNT];
    memcpy(map1_before, g_ctrl_t[0].task_id_map1[type], sizeof(map1_before));
    memcpy(map2_before, g_ctrl_t[0].task_id_map2[type], sizeof(map2_before));
    uint8_t slot_before[AIC_CNT][AIC_OSTD];
    for (int core = 0; core < AIC_CNT; core++) {
        for (int slot = 0; slot < AIC_OSTD; slot++) {
            slot_before[core][slot] = atomic_load_explicit(
                &g_executors[type][core].slot_state[slot], memory_order_acquire);
        }
    }
    uint16_t nbi_before = atomic_load_explicit(&g_next_block_idx[s_idx],
                                               memory_order_acquire);
    uint64_t skip_before = atomic_load_explicit(&g_ed_send_skip_cnt,
                                                memory_order_relaxed);

    enqueue(&g_ctrl_t[0].ready_queue[type], task_id);
    int sent = dispatch(0);
    expect_int(sent, 0, "count==0: dispatch must skip invalid task");

    uint64_t skip_after = atomic_load_explicit(&g_ed_send_skip_cnt,
                                               memory_order_relaxed);
    expect_u64(skip_after - skip_before, 1ULL,
               "count==0: skip counter should increase by exactly 1");
    expect_u16(atomic_load_explicit(&g_next_block_idx[s_idx], memory_order_acquire),
               nbi_before,
               "count==0: nbi must stay unchanged");
    expect_u64(atomic_load_explicit(&g_ctrl_t[0].free_bitmap[type][0],
                                    memory_order_relaxed),
               free0_before, "count==0: free_bitmap[type][0] unchanged");
    expect_u64(atomic_load_explicit(&g_ctrl_t[0].free_bitmap[type][1],
                                    memory_order_relaxed),
               free1_before, "count==0: free_bitmap[type][1] unchanged");
    expect_u64(atomic_load_explicit(&g_ctrl_t[0].msg_bitmap[type][0],
                                    memory_order_relaxed),
               msg0_before, "count==0: msg_bitmap[type][0] unchanged");
    expect_u64(atomic_load_explicit(&g_ctrl_t[0].msg_bitmap[type][1],
                                    memory_order_relaxed),
               msg1_before, "count==0: msg_bitmap[type][1] unchanged");
    expect_true(memcmp(map1_before, g_ctrl_t[0].task_id_map1[type],
                       sizeof(map1_before)) == 0,
                "count==0: task_id_map1 unchanged");
    expect_true(memcmp(map2_before, g_ctrl_t[0].task_id_map2[type],
                       sizeof(map2_before)) == 0,
                "count==0: task_id_map2 unchanged");

    for (int core = 0; core < AIC_CNT; core++) {
        for (int slot = 0; slot < AIC_OSTD; slot++) {
            uint8_t now = atomic_load_explicit(&g_executors[type][core].slot_state[slot],
                                               memory_order_acquire);
            if (now != slot_before[core][slot]) {
                fprintf(stderr,
                        "FAIL: count==0: slot_state changed at core=%d slot=%d (got=%u want=%u)\n",
                        core, slot, now, slot_before[core][slot]);
                g_failures++;
            }
        }
    }
}

/*
 * Test 1a：count==1 时 send_task 必须 CAS 0 -> 1，并把 slot 正常发布 RUNNABLE。
 *   RED 期望：Step 2 不动 nbi，故 nbi[T] 结束时仍是 0，第一条断言即失败。
 */
static void test_step3_count1_claims_full_count(void)
{
    reset_runtime_state();

    const uint16_t task_id = 7;
    const uint16_t s_idx = task_id & RING_MASK;
    g_basic_buf[s_idx].count = 1;
    g_basic_buf[s_idx].duration = 1;

    bool enq_ok = enqueue(&g_ctrl_t[0].ready_queue[TASK_TYPE_CUBE], task_id);
    expect_true(enq_ok, "count==1: enqueue should succeed");

    int sent = dispatch(0);
    expect_int(sent, 1, "count==1: dispatch should send exactly one task");

    uint16_t nbi = atomic_load_explicit(&g_next_block_idx[s_idx],
                                        memory_order_acquire);
    expect_u16(nbi, 1,
               "count==1: g_next_block_idx must be atomically claimed 0->count");

    uint8_t state = atomic_load_explicit(
        &g_executors[TASK_TYPE_CUBE][0].slot_state[0], memory_order_acquire);
    expect_u16((uint16_t)state, (uint16_t)EXE_SLOT_RUNNABLE,
               "count==1: slot_state must be RUNNABLE after send");
    expect_u16(g_ctrl_t[0].task_id_map1[TASK_TYPE_CUBE][0], task_id,
               "count==1: task_id_map1 must map to dispatched task");
    expect_u64(atomic_load_explicit(&g_ed_send_skip_cnt, memory_order_relaxed),
               0ULL, "count==1: send_skip_cnt stays 0 on successful CAS");
}

/*
 * Test 1b：接续 1a 的场景，把同一 task 再次入队。
 *   期望：nbi 已经是 count，send_task 侧 CAS 必然失败，走 skip 路径。
 *   RED 期望：Step 2 不做 CAS，第二次 dispatch 会正常派发到新的 slot，
 *   free_bitmap 会被继续消费；skip_cnt 也不会自增。
 */
static void test_step3_count1_second_send_skips(void)
{
    reset_runtime_state();

    const uint16_t task_id = 11;
    const uint16_t s_idx = task_id & RING_MASK;
    g_basic_buf[s_idx].count = 1;
    g_basic_buf[s_idx].duration = 1;

    /* 第一次派发：应成功认领 slot 0 core 0 */
    enqueue(&g_ctrl_t[0].ready_queue[TASK_TYPE_CUBE], task_id);
    int sent1 = dispatch(0);
    expect_int(sent1, 1, "count==1 (dedup): first dispatch sends once");

    /* 快照：全部与 (type=CUBE) 相关的 dispatcher 状态 */
    uint64_t free0_before = atomic_load_explicit(
        &g_ctrl_t[0].free_bitmap[TASK_TYPE_CUBE][0], memory_order_relaxed);
    uint64_t free1_before = atomic_load_explicit(
        &g_ctrl_t[0].free_bitmap[TASK_TYPE_CUBE][1], memory_order_relaxed);
    uint64_t msg0_before = atomic_load_explicit(
        &g_ctrl_t[0].msg_bitmap[TASK_TYPE_CUBE][0], memory_order_relaxed);
    uint64_t msg1_before = atomic_load_explicit(
        &g_ctrl_t[0].msg_bitmap[TASK_TYPE_CUBE][1], memory_order_relaxed);
    uint16_t map1_snapshot[AIC_CNT];
    uint16_t map2_snapshot[AIC_CNT];
    memcpy(map1_snapshot, g_ctrl_t[0].task_id_map1[TASK_TYPE_CUBE],
           sizeof(map1_snapshot));
    memcpy(map2_snapshot, g_ctrl_t[0].task_id_map2[TASK_TYPE_CUBE],
           sizeof(map2_snapshot));

    /* 复位 slot0 core0 的 slot_state：模拟 executor 尚未完成，dispatcher 侧
     * 期望不会二次派发到同 slot。这里不动 slot_state，让 assert 生效检查。 */
    uint64_t skip_before = atomic_load_explicit(&g_ed_send_skip_cnt,
                                                memory_order_relaxed);

    /* 再次入队同 task：send_task 应命中 CAS 失败并 skip */
    enqueue(&g_ctrl_t[0].ready_queue[TASK_TYPE_CUBE], task_id);
    int sent2 = dispatch(0);
    expect_int(sent2, 0, "count==1 (dedup): second dispatch must skip");

    uint64_t skip_after = atomic_load_explicit(&g_ed_send_skip_cnt,
                                               memory_order_relaxed);
    expect_u64(skip_after - skip_before, 1ULL,
               "count==1 (dedup): send_skip_cnt should increase by exactly 1");

    /* free_bitmap / msg_bitmap 不能被 skip 分支进一步动过 */
    expect_u64(atomic_load_explicit(&g_ctrl_t[0].free_bitmap[TASK_TYPE_CUBE][0],
                                    memory_order_relaxed),
               free0_before,
               "count==1 (dedup): free_bitmap[cube][0] unchanged on skip");
    expect_u64(atomic_load_explicit(&g_ctrl_t[0].free_bitmap[TASK_TYPE_CUBE][1],
                                    memory_order_relaxed),
               free1_before,
               "count==1 (dedup): free_bitmap[cube][1] unchanged on skip");
    expect_u64(atomic_load_explicit(&g_ctrl_t[0].msg_bitmap[TASK_TYPE_CUBE][0],
                                    memory_order_relaxed),
               msg0_before,
               "count==1 (dedup): msg_bitmap[cube][0] unchanged on skip");
    expect_u64(atomic_load_explicit(&g_ctrl_t[0].msg_bitmap[TASK_TYPE_CUBE][1],
                                    memory_order_relaxed),
               msg1_before,
               "count==1 (dedup): msg_bitmap[cube][1] unchanged on skip");
    expect_true(memcmp(map1_snapshot, g_ctrl_t[0].task_id_map1[TASK_TYPE_CUBE],
                       sizeof(map1_snapshot)) == 0,
                "count==1 (dedup): task_id_map1 unchanged on skip");
    expect_true(memcmp(map2_snapshot, g_ctrl_t[0].task_id_map2[TASK_TYPE_CUBE],
                       sizeof(map2_snapshot)) == 0,
                "count==1 (dedup): task_id_map2 unchanged on skip");
}

/*
 * Test 2：count>1 时 CAS 认领整任务（0 -> count）。
 *   RED 期望：Step 2 不动 nbi，故 nbi[T] 结束时仍是 0；此断言失败即 RED 生效。
 *   同时验证：send_task 只派发一次（不逐块），slot_state 只被写一次。
 */
static void test_step3_count_gt1_claims_full_count(void)
{
    reset_runtime_state();

    const uint16_t task_id = 17;
    const uint16_t s_idx = task_id & RING_MASK;
    const uint16_t count = 4;
    g_basic_buf[s_idx].count = count;
    g_basic_buf[s_idx].duration = 1;

    enqueue(&g_ctrl_t[0].ready_queue[TASK_TYPE_CUBE], task_id);
    int sent = dispatch(0);
    expect_int(sent, 1, "count>1: dispatch sends exactly one condensed entry");

    uint16_t nbi = atomic_load_explicit(&g_next_block_idx[s_idx],
                                        memory_order_acquire);
    expect_u16(nbi, count,
               "count>1: g_next_block_idx must be atomically claimed 0->count");

    uint8_t state = atomic_load_explicit(
        &g_executors[TASK_TYPE_CUBE][0].slot_state[0], memory_order_acquire);
    expect_u16((uint16_t)state, (uint16_t)EXE_SLOT_RUNNABLE,
               "count>1: slot_state must be RUNNABLE after send");
    expect_u16(g_ctrl_t[0].task_id_map1[TASK_TYPE_CUBE][0], task_id,
               "count>1: task_id_map1 must map to dispatched task");
}

/*
 * Test 3：count>1 场景下，全流程（send_task + executor_worker）能跑完全部 block。
 *   验证 Step 3 的 `0 -> count` 认领配合 Step 2 executor SPMD 分支能正确完成，
 *   且只有一个 slot 被消费。RED 期望：Step 2 send_task 未接 CAS 时依然能跑完
 *   （已在 Step 2 通过），这个 case 主要是 GREEN 时的 SPMD 完整性回归。
 */
static void test_step3_count_gt1_executor_completes(void)
{
    reset_runtime_state();

    const int type = TASK_TYPE_CUBE;
    const int core = 0;
    const uint16_t task_id = 23;
    const uint16_t s_idx = task_id & RING_MASK;
    const uint16_t count = 3;
    const uint64_t mask = 1ULL << core;

    g_basic_buf[s_idx].count = count;
    g_basic_buf[s_idx].duration = 1;

    enqueue(&g_ctrl_t[0].ready_queue[type], task_id);
    int sent = dispatch(0);
    expect_int(sent, 1, "SPMD full-run: send once");
    expect_u16(atomic_load_explicit(&g_next_block_idx[s_idx],
                                    memory_order_acquire),
               count, "SPMD full-run: nbi == count after send");

    pthread_t worker;
    int rc = pthread_create(&worker, NULL, executor_worker, NULL);
    expect_true(rc == 0, "SPMD full-run: executor_worker thread should start");
    if (rc != 0) {
        return;
    }

    /* 每 block ~1 tick + block 切换若干次；20ms 足够小 count 完成。 */
    sleep_ms(20);
    atomic_store_explicit(&g_is_done, true, memory_order_release);
    pthread_join(worker, NULL);

    expect_true(
        (atomic_load_explicit(&g_ctrl_t[0].msg_bitmap[type][0],
                              memory_order_acquire) &
         mask) != 0,
        "SPMD full-run: executor must publish msg_bitmap done bit");
    expect_u16((uint16_t)atomic_load_explicit(
                   &g_executors[type][core].slot_state[0],
                   memory_order_acquire),
               (uint16_t)EXE_SLOT_EMPTY,
               "SPMD full-run: slot_state returns to EMPTY on completion");
    expect_true(g_executors[type][core].block_idx[0] == 0,
                "SPMD full-run: block_idx reset to 0 on completion");
}

/*
 * Test 4：CAS 已经被预占（模拟 stager 已经抢块）时，send_task 必须 skip；
 *   slot / free bitmap / task_id_map 一位都不能动。
 *   RED 期望：Step 2 不看 nbi，仍会正常派发，slot_state 会变为 RUNNABLE，
 *   send_skip_cnt 不会自增，多条断言全部失败。
 */
static void test_step3_cas_preclaimed_no_side_effects(void)
{
    reset_runtime_state();

    const uint16_t task_id = 29;
    const uint16_t s_idx = task_id & RING_MASK;
    const uint16_t count = 1;
    g_basic_buf[s_idx].count = count;
    g_basic_buf[s_idx].duration = 1;

    /* 预占：模拟 stager (Hook 1) 已经 CAS 抢块 */
    atomic_store_explicit(&g_next_block_idx[s_idx], count, memory_order_release);

    /* 快照全部下游状态 */
    uint64_t free0_before = atomic_load_explicit(
        &g_ctrl_t[0].free_bitmap[TASK_TYPE_CUBE][0], memory_order_relaxed);
    uint64_t free1_before = atomic_load_explicit(
        &g_ctrl_t[0].free_bitmap[TASK_TYPE_CUBE][1], memory_order_relaxed);
    uint64_t msg0_before = atomic_load_explicit(
        &g_ctrl_t[0].msg_bitmap[TASK_TYPE_CUBE][0], memory_order_relaxed);
    uint64_t msg1_before = atomic_load_explicit(
        &g_ctrl_t[0].msg_bitmap[TASK_TYPE_CUBE][1], memory_order_relaxed);
    uint16_t map1_before[AIC_CNT];
    uint16_t map2_before[AIC_CNT];
    memcpy(map1_before, g_ctrl_t[0].task_id_map1[TASK_TYPE_CUBE],
           sizeof(map1_before));
    memcpy(map2_before, g_ctrl_t[0].task_id_map2[TASK_TYPE_CUBE],
           sizeof(map2_before));
    uint8_t slot0_state_before = atomic_load_explicit(
        &g_executors[TASK_TYPE_CUBE][0].slot_state[0], memory_order_acquire);
    uint8_t slot1_state_before = atomic_load_explicit(
        &g_executors[TASK_TYPE_CUBE][0].slot_state[1], memory_order_acquire);
    uint64_t skip_before = atomic_load_explicit(&g_ed_send_skip_cnt,
                                                memory_order_relaxed);

    enqueue(&g_ctrl_t[0].ready_queue[TASK_TYPE_CUBE], task_id);
    int sent = dispatch(0);
    expect_int(sent, 0, "pre-claimed: dispatch must return 0 (skipped)");

    uint64_t skip_after = atomic_load_explicit(&g_ed_send_skip_cnt,
                                               memory_order_relaxed);
    expect_u64(skip_after - skip_before, 1ULL,
               "pre-claimed: send_skip_cnt must increase by exactly 1");

    expect_u64(atomic_load_explicit(&g_ctrl_t[0].free_bitmap[TASK_TYPE_CUBE][0],
                                    memory_order_relaxed),
               free0_before,
               "pre-claimed: free_bitmap[cube][0] unchanged");
    expect_u64(atomic_load_explicit(&g_ctrl_t[0].free_bitmap[TASK_TYPE_CUBE][1],
                                    memory_order_relaxed),
               free1_before,
               "pre-claimed: free_bitmap[cube][1] unchanged");
    expect_u64(atomic_load_explicit(&g_ctrl_t[0].msg_bitmap[TASK_TYPE_CUBE][0],
                                    memory_order_relaxed),
               msg0_before,
               "pre-claimed: msg_bitmap[cube][0] unchanged");
    expect_u64(atomic_load_explicit(&g_ctrl_t[0].msg_bitmap[TASK_TYPE_CUBE][1],
                                    memory_order_relaxed),
               msg1_before,
               "pre-claimed: msg_bitmap[cube][1] unchanged");
    expect_true(memcmp(map1_before, g_ctrl_t[0].task_id_map1[TASK_TYPE_CUBE],
                       sizeof(map1_before)) == 0,
                "pre-claimed: task_id_map1 unchanged");
    expect_true(memcmp(map2_before, g_ctrl_t[0].task_id_map2[TASK_TYPE_CUBE],
                       sizeof(map2_before)) == 0,
                "pre-claimed: task_id_map2 unchanged");
    expect_u16((uint16_t)atomic_load_explicit(
                   &g_executors[TASK_TYPE_CUBE][0].slot_state[0],
                   memory_order_acquire),
               (uint16_t)slot0_state_before,
               "pre-claimed: slot0 slot_state unchanged");
    expect_u16((uint16_t)atomic_load_explicit(
                   &g_executors[TASK_TYPE_CUBE][0].slot_state[1],
                   memory_order_acquire),
               (uint16_t)slot1_state_before,
               "pre-claimed: slot1 slot_state unchanged");
    expect_u16(atomic_load_explicit(&g_next_block_idx[s_idx],
                                    memory_order_acquire),
               count, "pre-claimed: nbi remains at pre-claim value");
}

/*
 * Test 5：同一 batch 中，前项 CAS 失败 skip 后，后项正常任务必须仍可派发。
 *   重点验证：skip 不得错误推进 send_task 的本地 free_bitmap 快照。
 */
static void test_step3_batch_skip_then_next_dispatches(void)
{
    reset_runtime_state();

    const int type = TASK_TYPE_CUBE;
    const uint16_t skip_task = 37;
    const uint16_t send_task_id = 38;
    const uint16_t skip_idx = skip_task & RING_MASK;
    const uint16_t send_idx = send_task_id & RING_MASK;
    const uint16_t count = 1;
    const uint64_t first_core_mask = 1ULL << 0;

    g_basic_buf[skip_idx].count = count;
    g_basic_buf[skip_idx].duration = 1;
    g_basic_buf[send_idx].count = count;
    g_basic_buf[send_idx].duration = 1;

    /* 第一项预占：模拟 stager 已抢到块，dispatcher 必须 skip 它。 */
    atomic_store_explicit(&g_next_block_idx[skip_idx], count, memory_order_release);

    uint64_t free0_before = atomic_load_explicit(&g_ctrl_t[0].free_bitmap[type][0],
                                                 memory_order_relaxed);
    uint64_t free1_before = atomic_load_explicit(&g_ctrl_t[0].free_bitmap[type][1],
                                                 memory_order_relaxed);
    uint64_t skip_before = atomic_load_explicit(&g_ed_send_skip_cnt,
                                                memory_order_relaxed);

    enqueue(&g_ctrl_t[0].ready_queue[type], skip_task);
    enqueue(&g_ctrl_t[0].ready_queue[type], send_task_id);
    int sent = dispatch(0);
    expect_int(sent, 1,
               "batch skip+send: second task must still dispatch in same batch");

    uint64_t skip_after = atomic_load_explicit(&g_ed_send_skip_cnt,
                                               memory_order_relaxed);
    expect_u64(skip_after - skip_before, 1ULL,
               "batch skip+send: skip counter should increase by one");
    expect_u16(atomic_load_explicit(&g_next_block_idx[skip_idx], memory_order_acquire),
               count, "batch skip+send: pre-claimed task keeps claimed nbi");
    expect_u16(atomic_load_explicit(&g_next_block_idx[send_idx], memory_order_acquire),
               count, "batch skip+send: second task should claim nbi to count");

    expect_u16(g_ctrl_t[0].task_id_map1[type][0], send_task_id,
               "batch skip+send: second task should use core0 slot0");
    expect_u16((uint16_t)atomic_load_explicit(&g_executors[type][0].slot_state[0],
                                              memory_order_acquire),
               (uint16_t)EXE_SLOT_RUNNABLE,
               "batch skip+send: core0 slot0 must be RUNNABLE");
    expect_u64(atomic_load_explicit(&g_ctrl_t[0].free_bitmap[type][0],
                                    memory_order_relaxed),
               free0_before & ~first_core_mask,
               "batch skip+send: only first core bit should be consumed");
    expect_u64(atomic_load_explicit(&g_ctrl_t[0].free_bitmap[type][1],
                                    memory_order_relaxed),
               free1_before,
               "batch skip+send: slot1 free bitmap must stay unchanged");
}

int main(void)
{
    test_step3_count_zero_skips_without_side_effects();
    test_step3_count1_claims_full_count();
    test_step3_count1_second_send_skips();
    test_step3_count_gt1_claims_full_count();
    test_step3_count_gt1_executor_completes();
    test_step3_cas_preclaimed_no_side_effects();
    test_step3_batch_skip_then_next_dispatches();

    if (g_failures == 0) {
        printf("PASS: step3 next_block_idx claim protocol\n");
        return EXIT_SUCCESS;
    }

    fprintf(stderr, "%d check(s) failed\n", g_failures);
    return EXIT_FAILURE;
}
#else
int main(void)
{
    printf("SKIP: test-step3 requires ED_ENABLE=1\n");
    return 0;
}
#endif
