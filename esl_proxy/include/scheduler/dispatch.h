/*
 * dispatch.h - Task Dispatch with Shared Memory and Work-Stealing
 *
 * Distributes tasks to Executors via shared memory with work-stealing
 * load balancing across multiple Dispatch instances.
 *
 * Trust the Caller (Principle X): No input validation, undefined on invalid input.
 * C11 standard with _Atomic for lock-free concurrency.
 */

#ifndef SCHEDULER_DISPATCH_H
#define SCHEDULER_DISPATCH_H

#include <stdint.h>
#include <stddef.h>

#include "scheduler/conf.h"
#include "common/queue.h"
#include "common/task.h"

/* Multi-reader ring buffer for remote completed queue.
 * One writer (dispatch thread), multiple readers (painter threads).
 * Each painter has its own read cursor; data is "erased" (overwritable)
 * only after ALL painters have passed it (write_pos - min_read_pos <= REMOTE_RING_SIZE). */
typedef struct {
    uint32_t ring[REMOTE_RING_SIZE];
    _Atomic uint64_t write_pos;
    _Atomic uint64_t read_pos[PAINTER_THREAD_CNT];
} remote_cq_t;

typedef struct ctrl {
    // 64CORES
    uint64_t free_bitmap[TASK_TYPE_CNT][AIC_OSTD];
    uint64_t msg_bitmap[EXE_TYPE_CNT][AIC_OSTD];
    
    uint64_t aicore_mask;

    uint32_t task_id_map1[EXE_TYPE_CNT][AIC_CNT];
    uint32_t task_id_map2[EXE_TYPE_CNT][AIC_CNT];

    uint64_t* aicore_spr_1[EXE_TYPE_CNT][AIC_CNT];
    uint64_t* aicore_spr_2[EXE_TYPE_CNT][AIC_CNT];

    queue_t  ready_queue[TASK_TYPE_CNT];
    queue_t  completed_queue;
    remote_cq_t remote_completed_queue;
    uint32_t tid;
} ctrl_t;


void *dispatch_worker(void *arg);
void init_ctrl_t(void);

/* Batch-write completed tasks to the multi-reader ring buffer. */
static inline void remote_cq_write_batch(remote_cq_t *rcq, uint32_t *items, uint32_t cnt)
{
    if (cnt == 0) return;
    uint64_t wpos = atomic_load_explicit(&rcq->write_pos, memory_order_relaxed);
    uint32_t *ring = rcq->ring;
    for (uint32_t i = 0; i < cnt; i++) {
        ring[wpos & REMOTE_RING_MASK] = items[i];
        wpos++;
    }
    atomic_store_explicit(&rcq->write_pos, wpos, memory_order_release);
}

/* Batch-read from the multi-reader ring buffer for a specific painter.
 * Returns the number of items actually read (0 if nothing new). */
static inline uint32_t remote_cq_read_batch(remote_cq_t *rcq, int painter_tid,
                                             uint32_t *buf, uint32_t max_cnt)
{
    _Atomic uint64_t *my_read = &rcq->read_pos[painter_tid];
    uint64_t rpos = atomic_load_explicit(my_read, memory_order_acquire);
    uint64_t wpos = atomic_load_explicit(&rcq->write_pos, memory_order_acquire);

    uint64_t avail = wpos - rpos;
    if (avail <= 0) return 0;

    uint32_t cnt = (uint32_t)(avail < max_cnt ? avail : max_cnt);
    uint32_t *ring = rcq->ring;
    for (uint32_t i = 0; i < cnt; i++) {
        buf[i] = ring[(rpos + i) & REMOTE_RING_MASK];
    }

    atomic_store_explicit(my_read, rpos + cnt, memory_order_release);
    return cnt;
}

#endif /* SCHEDULER_DISPATCH_H */
