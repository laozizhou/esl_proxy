/*
 * test_ed_record.c - Step 1 验收：ring slot 复用时 generation tag 拒绝旧 record
 *
 * 场景：task_id=0 与 task_id=RING_SIZE 映射同一 ring slot (idx=0)。
 * 先为旧代写入 dispatch record，再 ed_init_task_meta 换代，验证：
 *   - load/clear 旧 id 不会误清新代 record
 *   - 新代 store 后 tag 正确
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#include "conf.h"
#include "early_dispatch.h"
#include "task.h"

/* 单元测试桩：ed_init_task_meta 需访问的 cutter/ring 全局 */
struct node_list g_successor_buf[RING_SIZE];
static task_state g_state_stub[RING_SIZE];
task_state *g_state_buf = g_state_stub;

static int g_failures;

static void expect_true(bool cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        g_failures++;
    }
}

static void expect_u32(uint32_t got, uint32_t want, const char *msg)
{
    if (got != want) {
        fprintf(stderr, "FAIL: %s (got=%u want=%u)\n", msg, got, want);
        g_failures++;
    }
}

int main(void)
{
    const uint32_t old_id = 0u;
    const uint32_t new_id = (uint32_t)RING_SIZE; /* 同 idx=0，不同 generation */
    uint64_t rec;

    ed_init();

    /* 旧代：写入 dispatch record */
    ed_task_dispatch_record_store(old_id, /*core=*/3, /*slot=*/0, /*type=*/0);
    rec = ed_task_dispatch_record_load(old_id);
    expect_true(ed_record_tag_matches(rec, old_id),
                "old generation record tag should match old_id after store");

    /* ring 换代：模拟 add_successors 调用 ed_init_task_meta */
    ed_init_task_meta(new_id, /*predecessor_cnt=*/0);

    /* 旧代 clear 应被拒绝（record 已被换代重置或 tag 不匹配） */
    ed_task_dispatch_record_clear(old_id);
    rec = ed_task_dispatch_record_load(old_id);
    expect_true(!ed_record_tag_matches(rec, old_id),
                "clear old_id must not leave a valid old-generation record");

    /* 新代写入并校验 */
    ed_task_dispatch_record_store(new_id, /*core=*/5, /*slot=*/1, /*type=*/1);
    rec = ed_task_dispatch_record_load(new_id);
    expect_true(ed_record_tag_matches(rec, new_id),
                "new generation record tag should match new_id");
    expect_u32(ED_UNPACK_CORE(ED_RECORD_SLOT(rec)), 5u,
               "new generation core field");
    expect_u32(ED_UNPACK_SLOT(ED_RECORD_SLOT(rec)), 1u,
               "new generation slot field");

    /* 旧代 load 不得误读新代 record */
    rec = ed_task_dispatch_record_load(old_id);
    expect_true(!ed_record_tag_matches(rec, old_id),
                "load with old_id must reject new generation record");

    /* g_ring_task_tag 应指向新代 */
    expect_u32(atomic_load_explicit(&g_ring_task_tag[0], memory_order_relaxed),
               new_id, "g_ring_task_tag after init_task_meta");

    if (g_failures == 0) {
        printf("PASS: ed ring reuse tag rejection (%u checks)\n", 7u);
        return EXIT_SUCCESS;
    }

    fprintf(stderr, "%d check(s) failed\n", g_failures);
    return EXIT_FAILURE;
}
