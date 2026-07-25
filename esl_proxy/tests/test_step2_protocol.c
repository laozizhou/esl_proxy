/*
 * test_step2_protocol.c - Step 2 发布/回收协议回归测试
 *
 * 覆盖点：
 * 1) dispatch 发送后应发布 RUNNABLE，且不再 Fake Return。
 * 2) 同一 done 快照完成 task_id 映射与 free bit 回收。
 * 3) executor 完成后 slot_state 必须先归 EMPTY，再对外发布 msg。
 * 4) SPMD 任务不允许在首块结束就提前完成整任务。
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

extern atomic_int g_completed_cnt;
extern atomic_bool g_is_done;
extern atomic_bool g_orch_is_done;
int dispatch(int tid);

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

static void test_send_task_publishes_runnable_without_fake_return(void)
{
    reset_runtime_state();

    const uint16_t task_id = 7;
    g_basic_buf[task_id & RING_MASK].count = 1;
    g_basic_buf[task_id & RING_MASK].duration = 1;

    bool enq_ok = enqueue(&g_ctrl_t[0].ready_queue[TASK_TYPE_CUBE], task_id);
    expect_true(enq_ok, "enqueue cube task should succeed");

    int sent = dispatch(0);
    expect_true(sent == 1, "dispatch should send exactly one cube task");

    uint8_t state = atomic_load_explicit(
        &g_executors[TASK_TYPE_CUBE][0].slot_state[0], memory_order_acquire);
    expect_u8(state, EXE_SLOT_RUNNABLE,
              "send_task must release-publish slot_state=RUNNABLE");

    uint64_t mask = 1ULL << 0;
    uint64_t msg = g_ctrl_t[0].msg_bitmap[TASK_TYPE_CUBE][0];
    expect_true((msg & mask) == 0,
                "send_task must not fake-complete by writing msg_bitmap");
}

static void test_drain_snapshot_maps_task_id_and_recovers_free_bits(void)
{
    reset_runtime_state();

    const int cube_core = 3;
    const int vector_core = 5;
    const uint64_t cube_mask = 1ULL << cube_core;
    const uint64_t vector_mask = 1ULL << vector_core;
    const uint16_t cube_task = 101;
    const uint16_t vector_task = 202;

    g_ctrl_t[0].task_id_map1[TASK_TYPE_CUBE][cube_core] = cube_task;
    g_ctrl_t[0].task_id_map2[TASK_TYPE_VECTOR][vector_core] = vector_task;

    g_ctrl_t[0].free_bitmap[TASK_TYPE_CUBE][0] &= ~cube_mask;
    g_ctrl_t[0].free_bitmap[TASK_TYPE_VECTOR][1] &= ~vector_mask;

    g_ctrl_t[0].msg_bitmap[TASK_TYPE_CUBE][0] = cube_mask;
    g_ctrl_t[0].msg_bitmap[TASK_TYPE_VECTOR][1] = vector_mask;

    int sent = dispatch(0);
    expect_true(sent == 0, "dispatch should only drain completions in this test");

    uint16_t completed[8] = {0};
    uint16_t n = (uint16_t)(sizeof(completed) / sizeof(completed[0]));
    bool deq_ok = batch_dequeue(&g_ctrl_t[0].completed_queue, completed, &n);
    expect_true(deq_ok, "completed_queue should receive drained task ids");
    expect_u16(n, 2, "drain should enqueue two completed task ids");
    expect_u16(completed[0], cube_task, "slot0 snapshot should map to task_id_map1");
    expect_u16(completed[1], vector_task, "slot1 snapshot should map to task_id_map2");

    expect_true((g_ctrl_t[0].free_bitmap[TASK_TYPE_CUBE][0] & cube_mask) != 0,
                "cube free bit should be restored after drain");
    expect_true((g_ctrl_t[0].free_bitmap[TASK_TYPE_VECTOR][1] & vector_mask) != 0,
                "vector free bit should be restored after drain");
    expect_u64(g_ctrl_t[0].msg_bitmap[TASK_TYPE_CUBE][0], 0,
               "cube msg_bitmap slot0 should be consumed exactly once");
    expect_u64(g_ctrl_t[0].msg_bitmap[TASK_TYPE_VECTOR][1], 0,
               "vector msg_bitmap slot1 should be consumed exactly once");
    expect_true(atomic_load_explicit(&g_completed_cnt, memory_order_relaxed) == 2,
                "global completed counter should track drained snapshot count");
}

static void test_executor_completion_sets_slot_empty_then_publishes_msg(void)
{
    reset_runtime_state();

    const int type = TASK_TYPE_CUBE;
    const int core = 4;
    const int slot = 1;
    const uint16_t task_id = 303;
    const uint64_t mask = 1ULL << core;

    g_basic_buf[task_id & RING_MASK].count = 1;
    g_basic_buf[task_id & RING_MASK].duration = 1;

    g_executors[type][core].tasks[slot] = task_id;
    g_executors[type][core].block_idx[slot] = 0;
    g_executors[type][core].duration[slot] = 0;
    g_executors[type][core].idx = (uint8_t)slot;
    atomic_store_explicit(&g_executors[type][core].slot_state[slot],
                          EXE_SLOT_RUNNABLE, memory_order_release);
    g_ctrl_t[0].msg_bitmap[type][slot] = 0;

    pthread_t worker;
    int rc = pthread_create(&worker, NULL, executor_worker, NULL);
    expect_true(rc == 0, "executor_worker thread should start");
    if (rc != 0) {
        return;
    }

    sleep_ms(10);
    atomic_store_explicit(&g_is_done, true, memory_order_release);
    pthread_join(worker, NULL);

    expect_true((g_ctrl_t[0].msg_bitmap[type][slot] & mask) != 0,
                "executor should publish completion bit");
    expect_u8(atomic_load_explicit(&g_executors[type][core].slot_state[slot],
                                   memory_order_acquire),
              EXE_SLOT_EMPTY,
              "executor completion must leave slot_state as EMPTY");
}

static void test_spmd_does_not_complete_after_first_block(void)
{
    reset_runtime_state();

    const int type = TASK_TYPE_CUBE;
    const int core = 2;
    const int slot = 0;
    const uint16_t task_id = 404;
    const uint64_t mask = 1ULL << core;

    g_basic_buf[task_id & RING_MASK].count = 60000;
    g_basic_buf[task_id & RING_MASK].duration = 65535;

    g_executors[type][core].tasks[slot] = task_id;
    g_executors[type][core].block_idx[slot] = 0;
    g_executors[type][core].duration[slot] = 0;
    g_executors[type][core].idx = (uint8_t)slot;
    atomic_store_explicit(&g_executors[type][core].slot_state[slot],
                          EXE_SLOT_RUNNABLE, memory_order_release);
    g_ctrl_t[0].msg_bitmap[type][slot] = 0;

    pthread_t worker;
    int rc = pthread_create(&worker, NULL, executor_worker, NULL);
    expect_true(rc == 0, "executor_worker thread should start for SPMD case");
    if (rc != 0) {
        return;
    }

    sleep_ms(2);
    atomic_store_explicit(&g_is_done, true, memory_order_release);
    pthread_join(worker, NULL);

    expect_true(g_executors[type][core].block_idx[slot] > 0,
                "SPMD task should advance at least first block");
    expect_true((g_ctrl_t[0].msg_bitmap[type][slot] & mask) == 0,
                "SPMD task must not complete whole task after first block");
    expect_u8(atomic_load_explicit(&g_executors[type][core].slot_state[slot],
                                   memory_order_acquire),
              EXE_SLOT_RUNNABLE,
              "unfinished SPMD task should keep slot RUNNABLE");
}

int main(void)
{
    test_send_task_publishes_runnable_without_fake_return();
    test_drain_snapshot_maps_task_id_and_recovers_free_bits();
    test_executor_completion_sets_slot_empty_then_publishes_msg();
    test_spmd_does_not_complete_after_first_block();

    if (g_failures == 0) {
        printf("PASS: step2 protocol checks\n");
        return EXIT_SUCCESS;
    }

    fprintf(stderr, "%d check(s) failed\n", g_failures);
    return EXIT_FAILURE;
}
