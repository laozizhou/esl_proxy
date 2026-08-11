#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "conf.h"
#include "cutter.h"
#include "early_dispatch.h"
#include "ring_buf.h"

/* cutter.c 内部符号，测试里显式声明。 */
extern uint32_t g_commit_task_id;
extern uint32_t g_predecessor_cnt[RING_SIZE];
extern task_state *g_state_buf;
void add_successors(uint32_t ready_cnt[], uint32_t rq_buf[][RQ_BATCH_SIZE]);

static int g_failures;

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
    ed_init();

    if (g_state_buf == NULL) {
        init_state_buf();
    }
    for (size_t i = 0; i < RING_SIZE; i++) {
        g_state_buf[i].state = TASK_STATUS_CREATING;
        g_state_buf[i].task_id = 0;
        g_state_buf[i].successor_cnt = 0;
    }
    g_commit_task_id = 0;
}

/*
 * 场景 1：add_successors 先挂边（pred 尚未 dispatch）；
 *        后续 Hook 0 才贡献 fanin，达到 target 后进入 STAGING。
 */
static void test_step4_hook0_contributes_after_append(void)
{
    reset_runtime_state();

    const uint16_t p_full = 1;
    const uint16_t s_full = 2;
    const uint16_t p_idx = (uint16_t)(p_full & RING_MASK);
    const uint16_t s_idx = (uint16_t)(s_full & RING_MASK);
    static uint32_t preds[1];

    preds[0] = p_full;
    g_basic_buf[s_idx].type = TASK_TYPE_CUBE;
    g_basic_buf[s_idx].count = 1;
    g_predecessors[s_idx].cnt = 1;
    g_predecessors[s_idx].exp = preds;
    g_state_buf[p_idx].state = TASK_STATUS_CREATING;

    atomic_store_explicit(&g_ring_task_tag[p_idx], p_full, memory_order_release);
    atomic_store_explicit(&g_dispatch_tag[p_idx], ED_TASK_TAG_INVALID, memory_order_relaxed);

    g_commit_task_id = s_full;
    atomic_store_explicit(&g_task_id, s_full + 1, memory_order_release);

    uint32_t ready_cnt[2] = {0, 0};
    uint32_t rq_buf[2][RQ_BATCH_SIZE] = {{0}};
    add_successors(ready_cnt, rq_buf);

    expect_u16(g_successor_buf[p_idx].cnt, 1,
               "add_successors should append edge to predecessor successor list");
    expect_u16(g_predecessor_cnt[s_idx], 1,
               "add_successors should record one unfinished predecessor");
    expect_u16((uint16_t)atomic_load_explicit(&g_dispatch_fanin[s_idx], memory_order_relaxed), 0,
               "before Hook0: dispatch_fanin should still be 0");

    propagate_dispatch_fanin(p_full);

    expect_u16((uint16_t)atomic_load_explicit(&g_dispatch_tag[p_idx], memory_order_acquire), p_full,
               "Hook0 should publish dispatch_tag for this generation");
    expect_u16((uint16_t)atomic_load_explicit(&g_dispatch_fanin[s_idx], memory_order_relaxed), 1,
               "Hook0 should contribute exactly one fanin for this edge");
    expect_u16(g_dispatch_fanin_target[s_idx], 1,
               "fanin target should be fixed to predecessor_cnt");
    expect_u16((uint16_t)atomic_load_explicit(&g_spec_state[s_idx], memory_order_acquire),
               ED_SPEC_STAGING,
               "target reached should move s-task to STAGING");
    expect_u64(g_ed_ready_queue.cnt, 1,
               "STAGING s-task should be enqueued into ed_ready_queue");
}

/*
 * 场景 2：pred 在挂边前已 dispatch（late-arrival）；
 *        add_successors 第二趟应补一次 fanin，并计入 late_arrival_cnt。
 */
static void test_step4_late_arrival_contributes_in_add_successors(void)
{
    reset_runtime_state();

    const uint16_t p_full = 5;
    const uint16_t s_full = 6;
    const uint16_t p_idx = (uint16_t)(p_full & RING_MASK);
    const uint16_t s_idx = (uint16_t)(s_full & RING_MASK);
    static uint32_t preds[1];

    preds[0] = p_full;
    g_basic_buf[s_idx].type = TASK_TYPE_CUBE;
    g_basic_buf[s_idx].count = 1;
    g_predecessors[s_idx].cnt = 1;
    g_predecessors[s_idx].exp = preds;
    g_state_buf[p_idx].state = TASK_STATUS_CREATING;

    atomic_store_explicit(&g_ring_task_tag[p_idx], p_full, memory_order_release);
    atomic_store_explicit(&g_dispatch_tag[p_idx], p_full, memory_order_release);

    g_commit_task_id = s_full;
    atomic_store_explicit(&g_task_id, s_full + 1, memory_order_release);

    uint32_t ready_cnt[2] = {0, 0};
    uint32_t rq_buf[2][RQ_BATCH_SIZE] = {{0}};
    add_successors(ready_cnt, rq_buf);

    expect_u16(g_successor_buf[p_idx].cnt, 1,
               "late-arrival path should still append successor edge");
    expect_u16((uint16_t)atomic_load_explicit(&g_dispatch_fanin[s_idx], memory_order_relaxed), 1,
               "late-arrival should compensate fanin immediately");
    expect_u16(g_dispatch_fanin_target[s_idx], 1,
               "fanin target should remain predecessor_cnt");
    expect_u16((uint16_t)atomic_load_explicit(&g_spec_state[s_idx], memory_order_acquire),
               ED_SPEC_STAGING,
               "late-arrival fanin hit should also move s-task to STAGING");
    expect_u64(atomic_load_explicit(&g_ed_late_arrival_cnt, memory_order_relaxed), 1,
               "late-arrival compensation counter should increase by one");
    expect_u64(g_ed_ready_queue.cnt, 1,
               "late-arrival path should enqueue s-task once");
}

int main(void)
{
    test_step4_hook0_contributes_after_append();
    test_step4_late_arrival_contributes_in_add_successors();

    if (g_failures == 0) {
        printf("PASS: step4 hook0 + late-arrival linearization\n");
        return EXIT_SUCCESS;
    }

    fprintf(stderr, "%d check(s) failed\n", g_failures);
    return EXIT_FAILURE;
}
