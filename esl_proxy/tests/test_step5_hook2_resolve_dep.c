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

extern uint16_t g_predecessor_cnt[RING_SIZE];
extern task_state *g_state_buf;
void resolve_dep(uint16_t cnt, uint16_t* cq_buf, uint16_t rq_buf[][RQ_BATCH_SIZE], uint16_t* ready_cnt);

static int g_failures;

static void expect_u16(uint16_t got, uint16_t want, const char *msg)
{
    if (got != want) {
        fprintf(stderr, "FAIL: %s (got=%u want=%u)\n", msg, got, want);
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

static void reset_runtime_state(void)
{
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
}

/*
 * old spec_state = NONE:
 * Hook 2 应把 state 置为 DISPATCHED，并统一 push ready_queue。
 */
static void test_step5_resolve_dep_none_to_dispatched(void)
{
    reset_runtime_state();

    const uint16_t p = 11;
    const uint16_t s = 12;
    const uint16_t p_idx = (uint16_t)(p & RING_MASK);
    const uint16_t s_idx = (uint16_t)(s & RING_MASK);
    uint16_t cq_buf[1] = {p};
    uint16_t rq_buf[2][RQ_BATCH_SIZE] = {{0}};
    uint16_t ready_cnt[2] = {0, 0};

    g_basic_buf[s_idx].type = TASK_TYPE_CUBE;
    g_state_buf[p_idx].successor_cnt = 1;
    g_successor_buf[p_idx].node[0] = s;
    g_predecessor_cnt[s_idx] = 1;
    atomic_store_explicit(&g_unfin_pred_cnt[s_idx], 1, memory_order_relaxed);
    atomic_store_explicit(&g_spec_state[s_idx], ED_SPEC_NONE, memory_order_relaxed);

    resolve_dep(1, cq_buf, rq_buf, ready_cnt);

    expect_u16(g_predecessor_cnt[s_idx], 0, "NONE path: predecessor_cnt should reach 0");
    expect_u16((uint16_t)atomic_load_explicit(&g_unfin_pred_cnt[s_idx], memory_order_relaxed),
               0, "NONE path: unfin should reach 0");
    expect_u8((uint8_t)atomic_load_explicit(&g_spec_state[s_idx], memory_order_acquire),
              ED_SPEC_DISPATCHED, "NONE path: spec_state should become DISPATCHED");
    expect_u16(ready_cnt[TASK_TYPE_CUBE], 1, "NONE path: ready queue count should increase");
    expect_u16(rq_buf[TASK_TYPE_CUBE][0], s, "NONE path: ready queue should contain successor");
}

/*
 * old spec_state = STAGING:
 * Hook 2 仍要切到 DISPATCHED，并统一 push ready_queue（Step 5 暂不敲 doorbell）。
 */
static void test_step5_resolve_dep_staging_to_dispatched(void)
{
    reset_runtime_state();

    const uint16_t p = 21;
    const uint16_t s = 22;
    const uint16_t p_idx = (uint16_t)(p & RING_MASK);
    const uint16_t s_idx = (uint16_t)(s & RING_MASK);
    uint16_t cq_buf[1] = {p};
    uint16_t rq_buf[2][RQ_BATCH_SIZE] = {{0}};
    uint16_t ready_cnt[2] = {0, 0};

    g_basic_buf[s_idx].type = TASK_TYPE_CUBE;
    g_state_buf[p_idx].successor_cnt = 1;
    g_successor_buf[p_idx].node[0] = s;
    g_predecessor_cnt[s_idx] = 1;
    atomic_store_explicit(&g_unfin_pred_cnt[s_idx], 1, memory_order_relaxed);
    atomic_store_explicit(&g_spec_state[s_idx], ED_SPEC_STAGING, memory_order_relaxed);

    resolve_dep(1, cq_buf, rq_buf, ready_cnt);

    expect_u16(g_predecessor_cnt[s_idx], 0, "STAGING path: predecessor_cnt should reach 0");
    expect_u16((uint16_t)atomic_load_explicit(&g_unfin_pred_cnt[s_idx], memory_order_relaxed),
               0, "STAGING path: unfin should reach 0");
    expect_u8((uint8_t)atomic_load_explicit(&g_spec_state[s_idx], memory_order_acquire),
              ED_SPEC_DISPATCHED, "STAGING path: spec_state should become DISPATCHED");
    expect_u16(ready_cnt[TASK_TYPE_CUBE], 1, "STAGING path: ready queue count should increase");
    expect_u16(rq_buf[TASK_TYPE_CUBE][0], s, "STAGING path: ready queue should contain successor");
}

int main(void)
{
    test_step5_resolve_dep_none_to_dispatched();
    test_step5_resolve_dep_staging_to_dispatched();

    if (g_failures == 0) {
        printf("PASS: step5 hook2 resolve_dep transitions\n");
        return EXIT_SUCCESS;
    }

    fprintf(stderr, "%d check(s) failed\n", g_failures);
    return EXIT_FAILURE;
}
