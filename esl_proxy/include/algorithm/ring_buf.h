/*
 * ring_buf.h - Ring Buffer API for task data storage
 *
 * 4 global Ring Buffers for O(1) task data indexed by TaskID.
 * Lock-free operations using C11 atomics.
 * Naming follows Constitution XI: no dag_ prefix on types/functions.
 */

#ifndef DAG_RING_BUF_H
#define DAG_RING_BUF_H

#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#include "conf.h"
#include "log.h"
#include "mpmc_queue.h"
#include "task.h"
#include "queue.h"
#include "dispatch.h"
#include "spin.h"

#include "tensor.h"

struct node_list {
    uint32_t cnt;
    uint32_t node[CON_NODE_CNT];
    struct node_list* next;
};

extern atomic_int g_task_id;
extern atomic_int g_min_uncomplete_task;
extern atomic_flag g_lock_buf[RING_SIZE];
extern struct task_desc g_basic_buf[RING_SIZE];
extern struct predecessor_list g_predecessors[RING_SIZE];
extern struct ring_buf g_predecessor_ring;
extern struct node_list g_successor_buf[RING_SIZE];
extern struct node_list g_successor_exp_buf[HALF_RING_SIZE];
extern ctrl_t g_ctrl_t[DISPATCH_THREAD_CNT];

extern int g_subtask_cnt;

#ifdef ENABLE_DAG_DEDUP
/* Transitive-reduction support: g_ancestors[t] is the full transitive
 * ancestor set of task t (every task t depends on, directly or indirectly),
 * stored as a bitset of RING_SIZE bits. Populated incrementally by
 * add_predecessors() -- see the DAG dedup report for the algorithm (same
 * topological-order + reachability-bitset approach as dag_dedup.py,
 * cross-checked against simpler's deps_viewer.py). */
#define ANCESTOR_WORDS (RING_SIZE / 64)
extern uint64_t g_ancestors[RING_SIZE][ANCESTOR_WORDS];

static inline bool ancestor_test(uint32_t task, uint32_t candidate)
{
    return (g_ancestors[task][candidate / 64] >> (candidate % 64)) & 1u;
}

static inline void ancestor_set_bit(uint32_t task, uint32_t candidate)
{
    g_ancestors[task][candidate / 64] |= (1ULL << (candidate % 64));
}

static inline void ancestor_union(uint32_t dst, uint32_t src)
{
    for (int w = 0; w < ANCESTOR_WORDS; w++) {
        g_ancestors[dst][w] |= g_ancestors[src][w];
    }
}
#endif /* ENABLE_DAG_DEDUP */

struct ring_buf {
    uint32_t size;
    uint32_t* head;
    atomic_size_t start;   /* slot index into head[], not a pointer -- see add_predecessors().
                            * GCC's atomic_fetch_add on a uint32_t* _Atomic does not scale by
                            * sizeof(uint32_t) on at least two independent GCC builds we've
                            * tested (MinGW/UCRT64 gcc 16.1.0 on Windows, and RHEL gcc 11.5.0
                            * on Linux x86_64) -- confirmed with a standalone repro on both.
                            * Clang does not have this bug. Using a plain atomic integer index
                            * instead of an atomic pointer avoids the buggy code path entirely. */
    atomic_size_t tail;
};

static inline void ring_buf_init(void)
{
    for (size_t i = 0; i < RING_SIZE; i++) {
        g_successor_buf[i].next = NULL;
    }
    g_predecessor_ring.head = malloc(sizeof(uint32_t) * NODE_BUFF_SIZE);
    atomic_store(&g_predecessor_ring.tail, 0);
    atomic_store(&g_predecessor_ring.start, 0);
}

/* input/output/inout tensors are all recorded the same way: append the
 * tensor's buffer_addr to data[] and bump tensor_cnt. task_desc keeps a single
 * flat data[]/tensor_cnt with no direction field, so there is one implementation
 * here; the dependency direction (when needed) is tracked by the tensormap
 * layer, not the ring buffer. The distinct add_input/output/inout spellings are
 * kept only for call-site readability. */
static inline void add_tensor_addr(uint32_t task_id, uint64_t addr)
{
    int idx = g_basic_buf[task_id & RING_MASK].tensor_cnt++;
    g_basic_buf[task_id & RING_MASK].data[idx] = addr;
}

/* Only buffer_addr is recorded, so the macros read that 8-byte field directly
 * instead of copying the whole 128B Tensor. (t).buffer_addr is valid for both
 * lvalue and rvalue arguments, e.g. add_input(id, view(x, ...)). */
#define add_input(task_id, t)  add_tensor_addr((task_id), (t).buffer_addr)
#define add_output(task_id, t) add_tensor_addr((task_id), (t).buffer_addr)
#define add_inout(task_id, t)  add_tensor_addr((task_id), (t).buffer_addr)

static inline void add_scalar(uint32_t task_id, int64_t t)
{
    int idx = g_basic_buf[task_id & RING_MASK].scalar_cnt++;
    g_basic_buf[task_id & RING_MASK].scalar[idx] = t;
}

static inline void lock(int slotIdx)
{
    while (atomic_flag_test_and_set_explicit(&g_lock_buf[slotIdx], memory_order_acquire)) {
        spin_wait();
    }
}

static inline void unlock(int slotIdx)
{
    atomic_flag_clear_explicit(&g_lock_buf[slotIdx], memory_order_release);
}

static int add_predecessors(uint32_t task_id, uint32_t target[], uint32_t n, uint32_t start)
{
    // int slotIdx = task_id & RING_MASK;
    int slotIdx = task_id;
    struct predecessor_list *ptr = &g_predecessors[slotIdx];
    int cnt = start;
    if (ptr->cnt <= 0)
        ptr->exp = g_predecessor_ring.head + atomic_load(&g_predecessor_ring.tail);
    
    uint32_t min_uncomplete_task = atomic_load_explicit(&g_min_uncomplete_task, memory_order_acquire);
    for (uint32_t i = 0; i < n; i++)
    {
        if (target[i] < min_uncomplete_task)
            continue;

#ifdef ENABLE_DAG_DEDUP
        /* Transitive reduction: skip target[i] if it's already an ancestor
         * of some OTHER candidate in this same batch -- i.e. a path
         * target[i] -> ... -> target[j] -> task_id already exists, so the
         * direct target[i] -> task_id edge adds no new ordering constraint. */
        bool redundant = false;
        for (uint32_t j = 0; j < n; j++) {
            if (j == i)
                continue;
            if (ancestor_test(target[j], target[i])) {
                redundant = true;
                break;
            }
        }
        if (redundant)
            continue;
#endif /* ENABLE_DAG_DEDUP */

        WORKER_LOGF("succeed,task_id,%u,predecessor_id,%u,idx,%d", task_id, target[i], cnt);
        size_t idx = atomic_fetch_add(&g_predecessor_ring.tail, 1);
        if (idx >= NODE_BUFF_SIZE) {
            /* g_predecessor_ring.head has no wraparound; writing past
             * NODE_BUFF_SIZE silently corrupts adjacent heap chunks
             * (glibc later aborts with "malloc(): corrupted top size" at
             * an unrelated allocation, far from this actual bug site).
             * Fail loudly here instead. */
            MAIN_LOGF("[fatal] g_predecessor_ring overflow: idx=%zu >= NODE_BUFF_SIZE=%d", idx, NODE_BUFF_SIZE);
            abort();
        }
        g_predecessor_ring.head[idx] = target[i];
        cnt++;
    }
    ptr->cnt = cnt;

#ifdef ENABLE_DAG_DEDUP
    /* Record task_id's own transitive ancestor set (every predecessor plus
     * their own ancestors), regardless of which edges got written above --
     * task_id genuinely depends on all of target[], not just the ones kept
     * in predecessor_list, so future tasks must still see the full lineage
     * when they check ancestor_test() against task_id. */
    for (uint32_t i = 0; i < n; i++) {
        ancestor_set_bit(task_id, target[i]);
        ancestor_union(task_id, target[i]);
    }
#endif /* ENABLE_DAG_DEDUP */

    return cnt;
}

static inline bool new_task(uint32_t task_id, uint32_t type, uint32_t count, uint32_t duration)
{
    while ((task_id - atomic_load(&g_min_uncomplete_task)) >= RING_SIZE ) {
        MAIN_LOGF("[orchestration] task_id = %u g_min_uncomplete_task = %u", task_id, g_min_uncomplete_task);
        spin_wait();
    }
    if (count > 1)
        g_basic_buf[task_id & RING_MASK].mode = ORG_MODE_SPMD_SYNC;
    g_basic_buf[task_id & RING_MASK].type = (task_type_t)type;
    g_basic_buf[task_id & RING_MASK].count = count; 
    g_basic_buf[task_id & RING_MASK].duration = duration;
    g_subtask_cnt += count;
    WORKER_LOGF("new,task_id,%u,type,%d,subtask_cnt,%d", task_id, type, count);
    return true;
}

#endif /* DAG_RING_BUF_H */