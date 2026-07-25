/*
 * dispatch.h - Task Dispatch with Shared Memory and Work-Stealing
 *
 * Distributes tasks to Executors via shared memory with work-stealing
 * load balancing across multiple Dispatch instances.
 *
 * Trust the Caller (Principle X): No input validation, undefined on invalid input.
 * C11 standard with _Atomic for lock-free concurrency.
 */

#ifndef ALGORITHM_DISPATCH_H
#define ALGORITHM_DISPATCH_H

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>

#include "conf.h"
#include "task.h"
#include "queue.h"


typedef struct ctrl {
    /*
     * free_bitmap：dispatcher 侧可分配槽位位图。
     * Step 2 起由 dispatch/executor 跨线程协作访问，必须使用 C11 原子类型。
     */
    _Atomic uint64_t free_bitmap[TASK_TYPE_CNT][AIC_OSTD];
    /*
     * msg_bitmap：executor 完成后发布 done bit，dispatcher 通过 exchange 唯一消费。
     */
    _Atomic uint64_t msg_bitmap[EXE_TYPE_CNT][AIC_OSTD];

    uint16_t task_id_map1[EXE_TYPE_CNT][AIC_CNT];
    uint16_t task_id_map2[EXE_TYPE_CNT][AIC_CNT];

    queue_t  ready_queue[TASK_TYPE_CNT];
    queue_t  completed_queue;
    queue_t  remote_completed_queue;
    uint16_t tid;
} ctrl_t;

void *dispatch_worker(void *arg);
void init_ctrl_t(void);

#endif /* ALGORITHM_DISPATCH_H */
