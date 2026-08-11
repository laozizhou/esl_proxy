#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "conf.h"
#include "cutter.h"
#include "dispatch.h"
#include "early_dispatch.h"
#include "executor.h"
#include "ring_buf.h"

extern uint32_t g_predecessor_cnt[RING_SIZE];
extern task_state *g_state_buf;
void resolve_dep(uint32_t cnt, uint32_t *cq_buf, uint32_t rq_buf[][RQ_BATCH_SIZE],
                 uint32_t *ready_cnt);

static int g_failures;

static void expect_true(bool cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
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

static void reset_runtime_state(void)
{
    atomic_store_explicit(&g_task_id, 0, memory_order_relaxed);
    atomic_store_explicit(&g_min_uncomplete_task, 0, memory_order_relaxed);
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
}

static uint64_t stage_one_task(uint16_t task_id, task_type_t type)
{
    uint16_t s_idx = (uint16_t)(task_id & RING_MASK);
    ed_init_task_meta(task_id, 0);
    g_basic_buf[s_idx].type = type;
    g_basic_buf[s_idx].count = 1;
    g_basic_buf[s_idx].duration = 1;
    atomic_store_explicit(&g_spec_state[s_idx], ED_SPEC_STAGING, memory_order_release);
    expect_true(enqueue(&g_ed_ready_queue, task_id), "stage helper: enqueue should succeed");

    int staged = try_early_dispatch(0);
    expect_u16((uint16_t)staged, 1,
               "stage helper: try_early_dispatch should stage one task");
    if (staged != 1) {
        return ED_RECORD_INVALID;
    }
    return atomic_load_explicit(&g_staged_slot_record[s_idx], memory_order_seq_cst);
}

/*
 * Step 6 - Hook 1: stage 成功后，slot 必须先发布为 GATED（不是 RUNNABLE）。
 */
static void test_step6_hook1_stage_keeps_slot_gated(void)
{
    reset_runtime_state();

    const uint16_t s_id = 101;
    const uint16_t s_idx = (uint16_t)(s_id & RING_MASK);
    uint64_t record = stage_one_task(s_id, TASK_TYPE_CUBE);

    expect_true(ed_record_tag_matches(record, s_id), "Hook1 stage: record tag must match");
    uint32_t packed = ED_RECORD_SLOT(record);
    uint16_t core = ED_UNPACK_CORE(packed);
    uint8_t slot = ED_UNPACK_SLOT(packed);
    uint8_t type = ED_UNPACK_TYPE(packed);
    expect_true(core < AIC_CNT, "Hook1 stage: staged core must be valid");
    expect_true(slot < AIC_OSTD, "Hook1 stage: staged slot must be valid");
    expect_u8(type, TASK_TYPE_CUBE, "Hook1 stage: staged type must match task type");
    if (!ed_record_tag_matches(record, s_id) || core >= AIC_CNT || slot >= AIC_OSTD ||
        type >= EXE_TYPE_CNT) {
        return;
    }

    expect_u16(atomic_load_explicit(&g_next_block_idx[s_idx], memory_order_acquire), 1,
               "Hook1 stage: count==1 task should claim next_block_idx 0->1");
    expect_u8(atomic_load_explicit(&g_spec_state[s_idx], memory_order_acquire), ED_SPEC_STAGING,
              "Hook1 stage: spec_state remains STAGING before release");
    expect_u8(atomic_load_explicit(&g_notify_claimed[s_idx], memory_order_acquire), 0,
              "Hook1 stage: notify_claimed should stay 0 before notify_once");
    expect_u8(
        atomic_load_explicit(&g_executors[type][core].slot_state[slot], memory_order_acquire),
        EXE_SLOT_GATED, "Hook1 stage: slot_state must be GATED before dependency release");
    expect_u8(atomic_load_explicit(&g_executors[type][core].doorbell[slot], memory_order_relaxed), 0,
              "Hook1 stage: doorbell should stay 0 before notify_once");
    expect_u64(atomic_load_explicit(&g_ed_stage_cnt, memory_order_relaxed), 1,
               "Hook1 stage: stage counter should increase once");
}

/*
 * Step 6 - Hook 2: unfin 未归零时绝不 RUNNABLE；1->0 时统一走 ed_notify_once。
 */
static void test_step6_hook2_only_releases_on_unfin_zero(void)
{
    reset_runtime_state();

    const uint16_t p1 = 201;
    const uint16_t p2 = 202;
    const uint16_t s = 203;
    const uint16_t p1_idx = (uint16_t)(p1 & RING_MASK);
    const uint16_t p2_idx = (uint16_t)(p2 & RING_MASK);
    const uint16_t s_idx = (uint16_t)(s & RING_MASK);

    uint64_t record = stage_one_task(s, TASK_TYPE_CUBE);
    uint32_t packed = ED_RECORD_SLOT(record);
    uint16_t core = ED_UNPACK_CORE(packed);
    uint8_t slot = ED_UNPACK_SLOT(packed);
    uint8_t type = ED_UNPACK_TYPE(packed);
    if (!ed_record_tag_matches(record, s) || core >= AIC_CNT || slot >= AIC_OSTD ||
        type >= EXE_TYPE_CNT) {
        expect_true(false, "Hook2: staged record should be valid before resolve_dep");
        return;
    }

    g_state_buf[p1_idx].successor_cnt = 1;
    g_successor_buf[p1_idx].node[0] = s;
    g_state_buf[p2_idx].successor_cnt = 1;
    g_successor_buf[p2_idx].node[0] = s;
    g_predecessor_cnt[s_idx] = 2;
    atomic_store_explicit(&g_unfin_pred_cnt[s_idx], 2, memory_order_release);
    atomic_store_explicit(&g_spec_state[s_idx], ED_SPEC_STAGING, memory_order_release);

    uint32_t rq_buf[2][RQ_BATCH_SIZE] = {{0}};
    uint32_t ready_cnt[2] = {0, 0};

    uint32_t cq1[1] = {p1};
    resolve_dep(1, cq1, rq_buf, ready_cnt);
    expect_u16(ready_cnt[type], 0,
               "Hook2: when one predecessor remains, successor must not enter ready queue");
    expect_u8(
        atomic_load_explicit(&g_executors[type][core].slot_state[slot], memory_order_acquire),
        EXE_SLOT_GATED, "Hook2: with unfin>0, staged slot must remain GATED");
    expect_u8(atomic_load_explicit(&g_executors[type][core].doorbell[slot], memory_order_relaxed), 0,
              "Hook2: with unfin>0, doorbell must stay 0");
    expect_u8(atomic_load_explicit(&g_notify_claimed[s_idx], memory_order_acquire), 0,
              "Hook2: with unfin>0, notify_claimed must stay 0");
    expect_u64(atomic_load_explicit(&g_ed_hit_cnt, memory_order_relaxed), 0,
               "Hook2: with unfin>0, hit counter must stay 0");

    uint32_t cq2[1] = {p2};
    resolve_dep(1, cq2, rq_buf, ready_cnt);
    expect_u16(ready_cnt[type], 1,
               "Hook2: when unfin reaches 0, successor should enter ready queue once");
    expect_u16(rq_buf[type][0], s,
               "Hook2: ready queue should contain released successor");
    expect_u16(g_predecessor_cnt[s_idx], 0, "Hook2: predecessor count should reach 0");
    expect_u16(atomic_load_explicit(&g_unfin_pred_cnt[s_idx], memory_order_acquire), 0,
               "Hook2: unfin count should reach 0");
    expect_u8(atomic_load_explicit(&g_spec_state[s_idx], memory_order_acquire), ED_SPEC_DISPATCHED,
              "Hook2: spec_state should become DISPATCHED on 1->0");
    expect_u8(atomic_load_explicit(&g_notify_claimed[s_idx], memory_order_acquire), 1,
              "Hook2: notify_once should claim exactly once");
    /*
     * 新协议：notify 只敲门铃，不翻转 slot_state。
     * GATED->RUNNABLE 由 executor 轮询门铃后自行完成，本测试不跑 executor，
     * 因此槽位应仍为 GATED、门铃为 1。
     */
    expect_u8(
        atomic_load_explicit(&g_executors[type][core].slot_state[slot], memory_order_acquire),
        EXE_SLOT_GATED, "Hook2: notify must not flip slot_state; executor opens the gate");
    expect_u8(atomic_load_explicit(&g_executors[type][core].doorbell[slot], memory_order_relaxed), 1,
              "Hook2: winner should write doorbell once");
    expect_u64(atomic_load_explicit(&g_ed_hit_cnt, memory_order_relaxed), 1,
               "Hook2: hit counter should increase once");
    expect_u64(atomic_load_explicit(&g_ed_self_notify_cnt, memory_order_relaxed), 0,
               "Hook2: self-notify counter should stay 0 here");
    expect_u64(
        atomic_load_explicit(&g_ed_hit_cnt, memory_order_relaxed) +
            atomic_load_explicit(&g_ed_self_notify_cnt, memory_order_relaxed),
        atomic_load_explicit(&g_ed_stage_cnt, memory_order_relaxed),
        "Hook2: hit+self must equal stage count");
}

/*
 * Step 6 - notify_once: Hook 1/2 都能调用，但 doorbell/state 发布只能发生一次。
 */
static void test_step6_notify_once_deduplicates_cross_sources(void)
{
    reset_runtime_state();

    const uint16_t s = 301;
    const uint16_t s_idx = (uint16_t)(s & RING_MASK);
    const uint16_t core = 7;
    const uint8_t slot = 1;
    const uint8_t type = TASK_TYPE_CUBE;
    uint64_t record = ED_PACK_RECORD(s, ED_PACK_SLOT(core, slot, type));

    ed_init_task_meta(s, 0);
    atomic_store_explicit(&g_spec_state[s_idx], ED_SPEC_DISPATCHED, memory_order_release);
    atomic_store_explicit(&g_staged_slot_record[s_idx], record, memory_order_seq_cst);
    atomic_store_explicit(&g_executors[type][core].slot_state[slot], EXE_SLOT_GATED,
                          memory_order_release);
    atomic_store_explicit(&g_executors[type][core].doorbell[slot], 0, memory_order_relaxed);

    ed_notify_once(s, record, ED_NOTIFY_HOOK2);
    ed_notify_once(s, record, ED_NOTIFY_HOOK1);

    expect_u8(atomic_load_explicit(&g_notify_claimed[s_idx], memory_order_acquire), 1,
              "notify_once: claim flag should stay at 1 after duplicate calls");
    expect_u64(atomic_load_explicit(&g_ed_hit_cnt, memory_order_relaxed), 1,
               "notify_once: first Hook2 call should count as hit");
    expect_u64(atomic_load_explicit(&g_ed_self_notify_cnt, memory_order_relaxed), 0,
               "notify_once: second Hook1 call should not increment self counter");
    expect_u8(
        atomic_load_explicit(&g_executors[type][core].slot_state[slot], memory_order_acquire),
        EXE_SLOT_GATED, "notify_once: slot stays GATED until executor polls the doorbell");
    expect_u8(atomic_load_explicit(&g_executors[type][core].doorbell[slot], memory_order_relaxed), 1,
              "notify_once: doorbell should be set exactly once by winner");
}

int main(void)
{
    test_step6_hook1_stage_keeps_slot_gated();
    test_step6_hook2_only_releases_on_unfin_zero();
    test_step6_notify_once_deduplicates_cross_sources();

    if (g_failures == 0) {
        printf("PASS: step6 hook1 + notify_once semantics\n");
        return EXIT_SUCCESS;
    }

    fprintf(stderr, "%d check(s) failed\n", g_failures);
    return EXIT_FAILURE;
}
