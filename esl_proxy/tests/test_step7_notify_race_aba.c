#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "conf.h"
#include "dispatch.h"
#include "early_dispatch.h"
#include "executor.h"
#include "ring_buf.h"
#include "task.h"

/*
 * g_state_buf normally comes from cutter.c. This test does not link cutter.o,
 * so provide a local stub to satisfy early_dispatch.c's extern dependency.
 */
static task_state g_state_stub[RING_SIZE];
task_state *g_state_buf = g_state_stub;

static int g_failures;

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
        fprintf(stderr, "FAIL: %s (got=%llu want=%llu)\n",
                msg,
                (unsigned long long)got,
                (unsigned long long)want);
        g_failures++;
    }
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

/*
 * Step 7 race order A:
 * Hook2 path wins notify_once claim and should increment hit counter.
 */
static void test_step7_hook2_notify_path(void)
{
    reset_runtime_state();

    const uint32_t s = 101;
    const uint16_t s_idx = (uint16_t)(s & RING_MASK);
    const uint16_t core = 3;
    const uint8_t slot = 0;
    const uint8_t type = TASK_TYPE_CUBE;
    const uint64_t record = ED_PACK_RECORD(s, ED_PACK_SLOT(core, slot, type));

    ed_init_task_meta(s, 0);
    atomic_store_explicit(&g_spec_state[s_idx], ED_SPEC_DISPATCHED, memory_order_release);
    atomic_store_explicit(&g_staged_slot_record[s_idx], record, memory_order_seq_cst);
    atomic_store_explicit(&g_executors[type][core].slot_state[slot], EXE_SLOT_GATED,
                          memory_order_release);
    atomic_store_explicit(&g_executors[type][core].doorbell[slot], 0,
                          memory_order_relaxed);

    ed_notify_once(s, record, ED_NOTIFY_HOOK2);

    expect_u8(atomic_load_explicit(&g_notify_claimed[s_idx], memory_order_acquire), 1,
              "Hook2 notify: claim bit should become 1");
    /* 新协议：notify 只敲门铃；开闸是 executor 的职责，本测试不跑 executor */
    expect_u8(
        atomic_load_explicit(&g_executors[type][core].slot_state[slot], memory_order_acquire),
        EXE_SLOT_GATED, "Hook2 notify: slot must stay GATED until executor opens the gate");
    expect_u8(atomic_load_explicit(&g_executors[type][core].doorbell[slot], memory_order_relaxed),
              1, "Hook2 notify: doorbell should be set to 1");
    expect_u64(atomic_load_explicit(&g_ed_hit_cnt, memory_order_relaxed), 1,
               "Hook2 notify: hit counter should increase");
    expect_u64(atomic_load_explicit(&g_ed_self_notify_cnt, memory_order_relaxed), 0,
               "Hook2 notify: self counter should stay zero");
}

/*
 * Step 7 race order B:
 * Hook1 self-notify path wins notify_once claim and should increment self counter.
 */
static void test_step7_hook1_self_notify_path(void)
{
    reset_runtime_state();

    const uint32_t s = 202;
    const uint16_t s_idx = (uint16_t)(s & RING_MASK);
    const uint16_t core = 8;
    const uint8_t slot = 1;
    const uint8_t type = TASK_TYPE_CUBE;
    const uint64_t record = ED_PACK_RECORD(s, ED_PACK_SLOT(core, slot, type));

    ed_init_task_meta(s, 0);
    atomic_store_explicit(&g_spec_state[s_idx], ED_SPEC_DISPATCHED, memory_order_release);
    atomic_store_explicit(&g_staged_slot_record[s_idx], record, memory_order_seq_cst);
    atomic_store_explicit(&g_executors[type][core].slot_state[slot], EXE_SLOT_GATED,
                          memory_order_release);
    atomic_store_explicit(&g_executors[type][core].doorbell[slot], 0,
                          memory_order_relaxed);

    ed_notify_once(s, record, ED_NOTIFY_HOOK1);

    expect_u8(atomic_load_explicit(&g_notify_claimed[s_idx], memory_order_acquire), 1,
              "Hook1 self-notify: claim bit should become 1");
    expect_u8(
        atomic_load_explicit(&g_executors[type][core].slot_state[slot], memory_order_acquire),
        EXE_SLOT_GATED, "Hook1 self-notify: slot must stay GATED until executor opens the gate");
    expect_u8(atomic_load_explicit(&g_executors[type][core].doorbell[slot], memory_order_relaxed),
              1, "Hook1 self-notify: doorbell should be set to 1");
    expect_u64(atomic_load_explicit(&g_ed_hit_cnt, memory_order_relaxed), 0,
               "Hook1 self-notify: hit counter should stay zero");
    expect_u64(atomic_load_explicit(&g_ed_self_notify_cnt, memory_order_relaxed), 1,
               "Hook1 self-notify: self counter should increase");
}

/*
 * Step 7 slot ABA guard:
 * old-generation stale notify MUST NOT release a new-generation staged slot.
 */
static void test_step7_stale_generation_notify_blocked(void)
{
    reset_runtime_state();

    const uint32_t old_task = 77;
    const uint32_t new_task = old_task + RING_SIZE;  /* same ring index, next generation */
    const uint16_t idx = (uint16_t)(old_task & RING_MASK);
    const uint16_t core = 5;
    const uint8_t slot = 0;
    const uint8_t type = TASK_TYPE_CUBE;
    const uint64_t old_record = ED_PACK_RECORD(old_task, ED_PACK_SLOT(core, slot, type));
    const uint64_t new_record = ED_PACK_RECORD(new_task, ED_PACK_SLOT(core, slot, type));

    ed_init_task_meta(new_task, 0);
    atomic_store_explicit(&g_spec_state[idx], ED_SPEC_DISPATCHED, memory_order_release);
    atomic_store_explicit(&g_staged_slot_record[idx], new_record, memory_order_seq_cst);
    atomic_store_explicit(&g_executors[type][core].slot_state[slot], EXE_SLOT_GATED,
                          memory_order_release);
    atomic_store_explicit(&g_executors[type][core].doorbell[slot], 0,
                          memory_order_relaxed);
    atomic_store_explicit(&g_notify_claimed[idx], 0, memory_order_release);

    ed_notify_once(old_task, old_record, ED_NOTIFY_HOOK2);

    expect_u8(atomic_load_explicit(&g_notify_claimed[idx], memory_order_acquire), 0,
              "stale notify: old generation must not claim notify bit");
    expect_u8(
        atomic_load_explicit(&g_executors[type][core].slot_state[slot], memory_order_acquire),
        EXE_SLOT_GATED, "stale notify: new generation slot must remain GATED");
    expect_u8(atomic_load_explicit(&g_executors[type][core].doorbell[slot], memory_order_relaxed),
              0, "stale notify: doorbell must remain 0");
    expect_u64(atomic_load_explicit(&g_ed_hit_cnt, memory_order_relaxed), 0,
               "stale notify: hit counter should not increase");
    expect_u64(atomic_load_explicit(&g_ed_self_notify_cnt, memory_order_relaxed), 0,
               "stale notify: self counter should not increase");
}

int main(void)
{
    test_step7_hook2_notify_path();
    test_step7_hook1_self_notify_path();
    test_step7_stale_generation_notify_blocked();

    if (g_failures == 0) {
        printf("PASS: step7 notify race + slot ABA guard\n");
        return EXIT_SUCCESS;
    }

    fprintf(stderr, "%d check(s) failed\n", g_failures);
    return EXIT_FAILURE;
}
