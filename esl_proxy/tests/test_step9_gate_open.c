/*
 * Step 9 - doorbell 开闸协议
 *
 * 新协议下 notify 只敲门铃，GATED -> RUNNABLE 的翻转、门铃清零、延迟 KPI
 * 收口全部由 executor 侧的 ed_poll_doorbell 完成。本测试直接驱动该函数，
 * 覆盖三种输入组合：
 *   1) GATED + 门铃置位   -> 开闸，门铃归零，计数与 KPI 各加一次
 *   2) GATED + 门铃为 0   -> 不动（依赖还没满足，谁也不能放行）
 *   3) 非 GATED + 门铃置位 -> 吸收残留门铃，且不得改变槽位状态
 * 并额外验证 notify -> 开闸 的端到端串联，以及重复轮询不会重复开闸。
 */

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "conf.h"
#include "dispatch.h"
#include "early_dispatch.h"
#include "ed_gate.h"
#include "executor.h"
#include "ring_buf.h"
#include "task.h"

/* g_state_buf 正常由 cutter.c 提供；本测试不链接 cutter.o，故给桩。 */
static task_state g_state_stub[RING_SIZE];
task_state *g_state_buf = g_state_stub;
extern atomic_bool g_is_done;

static int g_failures;

static void expect_bool(bool got, bool want, const char *msg)
{
    if (got != want) {
        fprintf(stderr, "FAIL: %s (got=%s want=%s)\n", msg,
                got ? "true" : "false", want ? "true" : "false");
        g_failures++;
    }
}

static void expect_u8(uint8_t got, uint8_t want, const char *msg)
{
    if (got != want) {
        fprintf(stderr, "FAIL: %s (got=%u want=%u)\n", msg, got, want);
        g_failures++;
    }
}

static void expect_u64(uint64_t got, uint64_t want, const char *msg)
{
    if (got != want) {
        fprintf(stderr, "FAIL: %s (got=%llu want=%llu)\n", msg,
                (unsigned long long)got, (unsigned long long)want);
        g_failures++;
    }
}

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
    memset(g_basic_buf, 0, sizeof(g_basic_buf));
    memset(g_successor_buf, 0, sizeof(g_successor_buf));
    memset(g_state_stub, 0, sizeof(g_state_stub));
    init_predecessors();
    init_ctrl_t();
    executor_init();
    ed_init();
}

/* 把槽位摆成 stager 发布 GATED 之后的样子 */
static void stage_slot(uint16_t task_id, int type, int core, int slot)
{
    g_executors[type][core].tasks[slot] = task_id;
    atomic_store_explicit(&g_executors[type][core].doorbell[slot], 0,
                          memory_order_relaxed);
    atomic_store_explicit(&g_executors[type][core].slot_state[slot],
                          EXE_SLOT_GATED, memory_order_release);
}

static void test_gate_opens_on_doorbell(void)
{
    reset_runtime_state();

    const uint16_t s = 501;
    const int core = 4;
    const int slot = 0;
    const int type = TASK_TYPE_CUBE;

    stage_slot(s, type, core, slot);
    /* 依赖未满足：门铃为 0，任何人都不该放行 */
    expect_bool(ed_poll_doorbell(type, core, slot), false,
                "GATED with doorbell=0 must not open the gate");
    expect_u8(atomic_load_explicit(&g_executors[type][core].slot_state[slot],
                                   memory_order_acquire),
              EXE_SLOT_GATED, "GATED with doorbell=0 must stay GATED");
    expect_u64(atomic_load_explicit(&g_ed_gate_open_cnt, memory_order_relaxed), 0,
               "no gate open should be counted yet");

    /* 模拟 notify 敲门铃 */
    atomic_store_explicit(&g_executors[type][core].doorbell[slot], 1,
                          memory_order_release);

    expect_bool(ed_poll_doorbell(type, core, slot), true,
                "GATED with doorbell=1 must open the gate");
    expect_u8(atomic_load_explicit(&g_executors[type][core].slot_state[slot],
                                   memory_order_acquire),
              EXE_SLOT_RUNNABLE, "opened slot must become RUNNABLE");
    expect_u8(atomic_load_explicit(&g_executors[type][core].doorbell[slot],
                                   memory_order_acquire),
              0, "opener must clear the doorbell");
    expect_u64(atomic_load_explicit(&g_ed_gate_open_cnt, memory_order_relaxed), 1,
               "gate_open_cnt should increase exactly once");

    /* 再轮询：槽位已 RUNNABLE 且门铃为 0，不应重复开闸 */
    expect_bool(ed_poll_doorbell(type, core, slot), false,
                "second poll must not open the gate again");
    expect_u64(atomic_load_explicit(&g_ed_gate_open_cnt, memory_order_relaxed), 1,
               "gate_open_cnt must not double count");
}

static void test_stale_doorbell_absorbed(void)
{
    reset_runtime_state();

    const int core = 9;
    const int slot = 1;
    const int type = TASK_TYPE_CUBE;

    /* 槽位已被回收（EMPTY），却收到一枚换代残留门铃 */
    atomic_store_explicit(&g_executors[type][core].slot_state[slot],
                          EXE_SLOT_EMPTY, memory_order_release);
    atomic_store_explicit(&g_executors[type][core].doorbell[slot], 1,
                          memory_order_release);

    expect_bool(ed_poll_doorbell(type, core, slot), false,
                "stale doorbell on EMPTY slot must not open a gate");
    expect_u8(atomic_load_explicit(&g_executors[type][core].slot_state[slot],
                                   memory_order_acquire),
              EXE_SLOT_EMPTY, "EMPTY slot must stay EMPTY");
    expect_u8(atomic_load_explicit(&g_executors[type][core].doorbell[slot],
                                   memory_order_acquire),
              0, "stale doorbell must be absorbed, otherwise next generation "
                 "would be released early");
    expect_u64(atomic_load_explicit(&g_ed_gate_open_cnt, memory_order_relaxed), 0,
               "absorbing a stale doorbell must not count as a gate open");
}

/* notify -> executor 开闸 的端到端串联，并与 hit/self 计数对账 */
static void test_notify_then_gate_open(void)
{
    reset_runtime_state();

    const uint32_t s = 602;
    const uint16_t s_idx = (uint16_t)(s & RING_MASK);
    const int core = 11;
    const int slot = 0;
    const int type = TASK_TYPE_CUBE;
    const uint64_t record =
        ED_PACK_RECORD(s, ED_PACK_SLOT((uint16_t)core, (uint8_t)slot, (uint8_t)type));

    ed_init_task_meta(s, 0);
    stage_slot((uint16_t)s, type, core, slot);
    atomic_store_explicit(&g_spec_state[s_idx], ED_SPEC_DISPATCHED,
                          memory_order_release);
    atomic_store_explicit(&g_staged_slot_record[s_idx], record, memory_order_seq_cst);

    ed_notify_once(s, record, ED_NOTIFY_HOOK2);

    /* notify 只敲门铃，不翻状态 */
    expect_u8(atomic_load_explicit(&g_executors[type][core].slot_state[slot],
                                   memory_order_acquire),
              EXE_SLOT_GATED, "notify alone must leave the slot GATED");
    expect_u8(atomic_load_explicit(&g_executors[type][core].doorbell[slot],
                                   memory_order_acquire),
              1, "notify must ring the doorbell");
    expect_u64(atomic_load_explicit(&g_ed_gate_open_cnt, memory_order_relaxed), 0,
               "no gate open before the executor polls");

    expect_bool(ed_poll_doorbell(type, core, slot), true,
                "executor must open the gate after notify");

    uint64_t notified = atomic_load_explicit(&g_ed_hit_cnt, memory_order_relaxed) +
                        atomic_load_explicit(&g_ed_self_notify_cnt, memory_order_relaxed);
    expect_u64(atomic_load_explicit(&g_ed_gate_open_cnt, memory_order_relaxed), notified,
               "gate_open_cnt must reconcile with hit+self_notify");
}

/*
 * 新执行模型：同一 core 同时只跑一个 slot。
 * 因此当 idx 指向 running slot 时，不应轮询其他 slot 的 doorbell。
 */
static void test_busy_core_does_not_poll_other_slot_doorbell(void)
{
    reset_runtime_state();

    const int type = TASK_TYPE_CUBE;
    const int core = 3;
    const int running_slot = 0;
    const int gated_slot = 1;
    const uint16_t running_task = 701;
    const uint16_t gated_task = 702;

    g_basic_buf[running_task & RING_MASK].count = 60000;
    g_basic_buf[running_task & RING_MASK].duration = 60000;
    g_basic_buf[gated_task & RING_MASK].count = 1;
    g_basic_buf[gated_task & RING_MASK].duration = 1;

    g_executors[type][core].tasks[running_slot] = running_task;
    g_executors[type][core].block_idx[running_slot] = 0;
    g_executors[type][core].duration[running_slot] = 60000;
    atomic_store_explicit(&g_executors[type][core].slot_state[running_slot],
                          EXE_SLOT_RUNNABLE, memory_order_release);
    atomic_store_explicit(&g_executors[type][core].doorbell[running_slot], 0,
                          memory_order_relaxed);

    g_executors[type][core].tasks[gated_slot] = gated_task;
    g_executors[type][core].block_idx[gated_slot] = 0;
    g_executors[type][core].duration[gated_slot] = 1;
    atomic_store_explicit(&g_executors[type][core].slot_state[gated_slot],
                          EXE_SLOT_GATED, memory_order_release);
    atomic_store_explicit(&g_executors[type][core].doorbell[gated_slot], 1,
                          memory_order_release);

    g_executors[type][core].idx = (uint8_t)running_slot;

    pthread_t worker;
    int rc = pthread_create(&worker, NULL, executor_worker, NULL);
    expect_bool(rc == 0, true, "executor_worker thread should start");
    if (rc != 0) {
        return;
    }

    sleep_ms(1);
    atomic_store_explicit(&g_is_done, true, memory_order_release);
    pthread_join(worker, NULL);

    expect_u8(atomic_load_explicit(&g_executors[type][core].slot_state[gated_slot],
                                   memory_order_acquire),
              EXE_SLOT_GATED, "busy core must not open other slot gate");
    expect_u8(atomic_load_explicit(&g_executors[type][core].doorbell[gated_slot],
                                   memory_order_acquire),
              1, "busy core must not consume other slot doorbell");
}

int main(void)
{
    test_gate_opens_on_doorbell();
    test_stale_doorbell_absorbed();
    test_notify_then_gate_open();
    test_busy_core_does_not_poll_other_slot_doorbell();

    if (g_failures == 0) {
        printf("test_step9_gate_open: PASS\n");
        return 0;
    }
    fprintf(stderr, "test_step9_gate_open: %d failure(s)\n", g_failures);
    return 1;
}
