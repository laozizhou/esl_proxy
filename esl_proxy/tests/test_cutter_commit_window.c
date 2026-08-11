#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "conf.h"
#include "ring_buf.h"

/* cutter.h 暂未导出该内部函数与游标，测试里显式声明。 */
extern uint32_t g_commit_task_id;
void add_successors(uint32_t ready_cnt[], uint32_t rq_buf[][RQ_BATCH_SIZE]);

static int g_failures;

static void expect_u16(uint16_t got, uint16_t want, const char *msg)
{
    if (got != want) {
        fprintf(stderr, "FAIL: %s (got=%u want=%u)\n", msg, got, want);
        g_failures++;
    }
}

static void reset_state(void)
{
    atomic_store_explicit(&g_task_id, 0, memory_order_relaxed);
    atomic_store_explicit(&g_min_uncomplete_task, 0, memory_order_relaxed);
    memset(g_basic_buf, 0, sizeof(g_basic_buf));
    memset(g_successor_buf, 0, sizeof(g_successor_buf));
    init_predecessors();
    g_commit_task_id = 0;
}

/*
 * g_task_id 是“下一待分配 ID”，提交窗口必须是 [g_commit_task_id, end) 右开区间。
 * 这个用例验证 add_successors 不能处理等于 g_task_id 的 phantom task。
 */
static void test_add_successors_uses_right_open_window(void)
{
    reset_state();

    atomic_store_explicit(&g_task_id, 1, memory_order_release); /* 仅 task 0 已发布 */
    g_basic_buf[0].type = TASK_TYPE_CUBE;
    g_basic_buf[1].type = TASK_TYPE_VECTOR; /* 若误处理到 1，可被本测试观察到 */

    uint32_t ready_cnt[2] = {0, 0};
    uint32_t rq_buf[2][RQ_BATCH_SIZE] = {{0}};
    add_successors(ready_cnt, rq_buf);

    expect_u16(g_commit_task_id, 1,
               "right-open: commit cursor must stop at g_task_id");
    expect_u16((uint16_t)(ready_cnt[TASK_TYPE_CUBE] + ready_cnt[TASK_TYPE_VECTOR]),
               1, "right-open: only one real task should be marked ready");
    expect_u16(rq_buf[TASK_TYPE_CUBE][0], 0,
               "right-open: ready queue should only contain task 0");
    expect_u16(ready_cnt[TASK_TYPE_VECTOR], 0,
               "right-open: phantom vector task must not be enqueued");
}

/*
 * PRE_BATCH_SIZE 限流也必须保持右开语义：当 end=min(g_task_id, commit+PRE_BATCH_SIZE)
 * 时，本轮最多处理 PRE_BATCH_SIZE 个任务，不能越界到 end 本身。
 */
static void test_add_successors_respects_pre_batch_cap(void)
{
    reset_state();

    const uint16_t published = (uint16_t)(PRE_BATCH_SIZE + 1);
    atomic_store_explicit(&g_task_id, published, memory_order_release);
    for (uint16_t i = 0; i < published; i++) {
        g_basic_buf[i].type = TASK_TYPE_CUBE;
    }

    uint32_t ready_cnt[2] = {0, 0};
    uint32_t rq_buf[2][RQ_BATCH_SIZE] = {{0}};
    add_successors(ready_cnt, rq_buf);

    expect_u16(g_commit_task_id, PRE_BATCH_SIZE,
               "pre-batch: commit cursor must advance by PRE_BATCH_SIZE only");
    expect_u16(ready_cnt[TASK_TYPE_CUBE], PRE_BATCH_SIZE,
               "pre-batch: exactly PRE_BATCH_SIZE tasks should be ready");
}

int main(void)
{
    test_add_successors_uses_right_open_window();
    test_add_successors_respects_pre_batch_cap();

    if (g_failures == 0) {
        printf("PASS: cutter commit window is right-open\n");
        return EXIT_SUCCESS;
    }

    fprintf(stderr, "%d check(s) failed\n", g_failures);
    return EXIT_FAILURE;
}
