#include <stddef.h>
#include <stdint.h>

#include "dispatch.h"
#include "mem_pool.h"
#include "ring_buf.h"

extern atomic_int g_completed_cnt;

int g_subtask_cnt = 0;

#ifndef ED_A11_P2_BLOCKS
#define ED_A11_P2_BLOCKS 8192
#endif

static inline void set_task_type(uint16_t task_id, task_type_t type)
{
    g_basic_buf[task_id & RING_MASK].type = type;
}

void aicpu_orchestration_entry(const uint64_t orch_args)
{
    (void)orch_args;
    uint16_t preds[2];

    new_task(g_task_id, TASK_TYPE_CUBE, 1, 1);
    set_task_type(g_task_id, TASK_TYPE_CUBE);
    const uint16_t p1 = g_task_id;
    g_task_id++;

    new_task(g_task_id, TASK_TYPE_CUBE, ED_A11_P2_BLOCKS, 60000);
    set_task_type(g_task_id, TASK_TYPE_CUBE);
    const uint16_t p2 = g_task_id;
    g_task_id++;

    new_task(g_task_id, TASK_TYPE_VECTOR, 1, 1);
    set_task_type(g_task_id, TASK_TYPE_VECTOR);
    const uint16_t s = g_task_id;
    preds[0] = p1;
    preds[1] = p2;
    add_predecessors(g_task_id, preds, 2, 0);
    MAIN_LOGF("[a11] probe_s=%u", s);
    g_task_id++;

    g_completed_cnt++;
}
