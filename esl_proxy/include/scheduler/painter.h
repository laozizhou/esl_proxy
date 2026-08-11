/*
 * cutter.h - Dependency resolution worker
 */

#ifndef PAINTER_H
#define PAINTER_H

#include <stdatomic.h>

#include "scheduler/conf.h"
#include "scheduler/dispatch.h"

#ifndef SCHEDULER_CASE
#define SCHEDULER_CASE cases/qwen3_14b_decode_subgraph.h
#endif

/* Macro to stringify the include directive properly */
#define __INCLUDE(x) #x
#define _INCLUDE_FILE(x) __INCLUDE(x)
#include _INCLUDE_FILE(SCHEDULER_CASE)

#include "common/task.h"
#include "common/queue.h"

struct node_list {
    uint32_t cnt;
    uint32_t node[CON_NODE_CNT];
    struct node_list* next;
};

/* Extern declarations from ring_buf / task system (avoid pulling in algorithm/ring_buf.h
 * which conflicts with common/queue.h) */
extern atomic_int g_task_id;
extern atomic_int g_min_uncomplete_task;


void *painter(void *arg);
void init_state_buf(void);
void buf_init(void);

#endif
