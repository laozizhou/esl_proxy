#include <stddef.h>
#include <stdint.h>

#include "dispatch.h"
#include "mem_pool.h"
#include "ring_buf.h"

extern atomic_int g_completed_cnt;

int g_subtask_cnt = 0;

#ifndef ED_A10_PAIR_CNT
#define ED_A10_PAIR_CNT 64
#endif

static inline void set_task_type(uint16_t task_id, task_type_t type)
{
    g_basic_buf[task_id & RING_MASK].type = type;
}

void aicpu_orchestration_entry(const uint64_t orch_args)
{
    (void)orch_args;
    uint16_t pred[1];

    for (int i = 0; i < ED_A10_PAIR_CNT; i++) {
        new_task(g_task_id, TASK_TYPE_CUBE, 1, 1);
        set_task_type(g_task_id, TASK_TYPE_CUBE);
        uint16_t p = g_task_id;
        g_task_id++;

        new_task(g_task_id, TASK_TYPE_VECTOR, 1, 1);
        set_task_type(g_task_id, TASK_TYPE_VECTOR);
        pred[0] = p;
        add_predecessors(g_task_id, pred, 1, 0);
        g_task_id++;
    }

    g_completed_cnt++;
}
