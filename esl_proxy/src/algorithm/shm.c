/*
 * shm.c - Shared memory global definitions for ring buffer task data
 *
 * Naming follows Constitution XI: no dag_ prefix on types/functions.
 */

#include "mem_pool.h"
#include "ring_buf.h"
#include "executor.h"
#include "conf.h"
#include "dispatch.h"

atomic_int g_task_id = 0;
atomic_int g_min_uncomplete_task = 0;
// Keep Atomic For Multi Dispatch Thread
atomic_int g_completed_cnt = 0;
atomic_bool g_is_done = false;
atomic_bool g_orch_is_done = false;


struct task_desc g_basic_buf[RING_SIZE];
struct node_list g_successor_buf[RING_SIZE];
struct node_list g_successor_exp_buf[HALF_RING_SIZE];

struct predecessor_list g_predecessors[RING_SIZE];
struct ring_buf g_predecessor_ring;
#ifdef ENABLE_DAG_DEDUP
uint64_t g_ancestors[RING_SIZE][ANCESTOR_WORDS];
#endif /* ENABLE_DAG_DEDUP */

uint32_t g_task_id_buf[RING_SIZE];
executor_t g_executors[EXE_TYPE_CNT][AIC_CNT];
atomic_flag g_lock_buf[RING_SIZE];
mem_pool_t g_mem_pool;
ctrl_t g_ctrl_t[DISPATCH_THREAD_CNT];

void init_predecessors(void)
{
    uint64_t diag_start_ns = get_time_ns();
    for (size_t i = 0; i < RING_SIZE; i++) {
        g_predecessors[i].cnt = 0;
        g_predecessors[i].exp = NULL;
    }
#ifdef ENABLE_DAG_DEDUP
    uint64_t diag_before_memset_ns = get_time_ns();
    memset(g_ancestors, 0, sizeof(g_ancestors));
    uint64_t diag_after_memset_ns = get_time_ns();
    MAIN_LOGF("[diag] predecessors_loop_ns = %llu", (unsigned long long)(diag_before_memset_ns - diag_start_ns));
    MAIN_LOGF("[diag] ancestors_memset_ns = %llu (size=%zu bytes)", (unsigned long long)(diag_after_memset_ns - diag_before_memset_ns), sizeof(g_ancestors));
#else
    MAIN_LOGF("[diag] predecessors_loop_ns = %llu", (unsigned long long)(get_time_ns() - diag_start_ns));
#endif /* ENABLE_DAG_DEDUP */
}

