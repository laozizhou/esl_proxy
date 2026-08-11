#include <stddef.h>
#include <stdint.h>

#include "dispatch.h"
#include "mem_pool.h"
#include "ring_buf.h"

extern atomic_int g_completed_cnt;

int g_subtask_cnt = 0;

#ifndef ED_A12_TASK_CNT
#define ED_A12_TASK_CNT 10000
#endif

static inline void set_task_type(uint16_t task_id, task_type_t type)
{
    g_basic_buf[task_id & RING_MASK].type = type;
}

void aicpu_orchestration_entry(const uint64_t orch_args)
{
    (void)orch_args;
    uint16_t pred[1];
    uint16_t prev = 0xFFFF;

    for (int i = 0; i < ED_A12_TASK_CNT; i++) {
        task_type_t ty = (i & 1) ? TASK_TYPE_VECTOR : TASK_TYPE_CUBE;
        new_task(g_task_id, ty, 1, 2);
        set_task_type(g_task_id, ty);
        if (prev != 0xFFFF) {
            pred[0] = prev;
            add_predecessors(g_task_id, pred, 1, 0);
        }
        prev = g_task_id;
        g_task_id++;
    }

    g_completed_cnt++;
}
