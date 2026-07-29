/*
 * dispatch.c - Dispatch Worker Thread Implementation
 *
 * Worker thread entry point for Dispatch.
 * This file is compiled separately as it contains pthread-specific code.
 */
#include <stdint.h>
#include <stdio.h>

#include "scheduler/dispatch.h"
#include "common/task.h"
#include "common/log.h"
#include "platform/a6.h"

extern atomic_bool g_is_done;

ctrl_t g_ctrl_t[DISPATCH_THREAD_CNT];

void init_ctrl_t(void)
{
    for (int tid = 0; tid < DISPATCH_THREAD_CNT; tid++) {
        g_ctrl_t[tid].tid = (uint32_t)tid;
        if (AIC_CNT_PER_THREAD >= 64) {
            g_ctrl_t[tid].aicore_mask = ~0ULL;
        } else {
            g_ctrl_t[tid].aicore_mask = ~0ULL >> (64 - AIC_CNT_PER_THREAD);
        }

        // Initialize free_bitmap for TASK_TYPE
        for (int i = 0; i < TASK_TYPE_CNT; i++) {
            for (int j = 0; j < AIC_OSTD; j++) {
                g_ctrl_t[tid].free_bitmap[i][j] = g_ctrl_t[tid].aicore_mask;
            }
        }
        // set_mix(tid);
        // Initialize msg_bitmap for EXE_TYPE
        for (int i = 0; i < EXE_TYPE_CNT; i++) {
            for (int j = 0; j < AIC_OSTD; j++) {
                g_ctrl_t[tid].msg_bitmap[i][j] = 0x0;
            }
        }
        
        // Init task_id_map
        for (int i = 0; i < EXE_TYPE_CNT; i++) {
            for (int j = 0; j < AIC_CNT; j++) {
                g_ctrl_t[tid].task_id_map1[i][j] = 0;
                g_ctrl_t[tid].task_id_map2[i][j] = 0;
            }
        }

        // Init aicore_spr
        uint64_t base = 0;
        uint64_t idx = 0;
        for (size_t i = 0; i < EXE_TYPE_CNT; i++)
        {
            idx = 0;
            for (size_t j = AIC_CNT_PER_THREAD * tid; j < AIC_CNT_PER_THREAD * (tid + 1); j++)
            {
                base = AICORE_SPR_BASE;
                base += (i == 0 ? AICORE_CUBE_OFFSET : AICORE_VECTOR_OFFSET);
                if (j >= AIC_CNT_PER_DIE) {
                    base += AICORE_DIE_OFFSET + AICORE_OFFSET * (j - AIC_CNT_PER_DIE);
                } else {
                    base += AICORE_OFFSET * j;
                }

                g_ctrl_t[tid].aicore_spr_1[i][idx] = (uint64_t*)base;
                g_ctrl_t[tid].aicore_spr_2[i][idx] = (uint64_t*)(base + AICORE_SPR_OFFSET); 
                idx++;
            }
        }
        
        // Init queues
        for (int i = 0; i < TASK_TYPE_CNT; i++) {
            memset(&g_ctrl_t[tid].ready_queue[i], 0, sizeof(queue_t));
            atomic_flag_clear_explicit(&g_ctrl_t[tid].ready_queue[i].lock, memory_order_release);
        }
        memset(&g_ctrl_t[tid].completed_queue, 0, sizeof(queue_t));
        atomic_flag_clear_explicit(&g_ctrl_t[tid].completed_queue.lock, memory_order_release);
        memset(&g_ctrl_t[tid].remote_completed_queue, 0, sizeof(queue_t));
        atomic_flag_clear_explicit(&g_ctrl_t[tid].remote_completed_queue.lock, memory_order_release);
    }
}

/* Kept for reference only and NOT called - see the explanation at the end of read_msgq()
 * before re-enabling it. As written it clobbers the vector bitmap rather than a separate MIX
 * one, which deadlocks the type-1 half of the DAG once a task spans more than one round. */
__attribute__((unused)) static void set_mix(int tid)
{
    for (int j = 0; j < AIC_OSTD; j++) {
        g_ctrl_t[tid].free_bitmap[TASK_TYPE_MIX][j] =
            g_ctrl_t[tid].free_bitmap[TASK_TYPE_CUBE][j] &
            g_ctrl_t[tid].free_bitmap[TASK_TYPE_VECTOR][j];
    }
}

/*
        for (size_t i = 0; i < EXE_TYPE_CNT; i++)
        {
            idx = 0;
            for (size_t j = AIC_CNT_PER_THREAD * tid; j < AIC_CNT_PER_THREAD * (tid + 1); j++)
            {
                base = AICORE_SPR_BASE;
                base += (i == 0 ? AICORE_CUBE_OFFSET : AICORE_VECTOR_OFFSET);
                if (j >= AIC_CNT_PER_DIE) {
                    base += AICORE_DIE_OFFSET + AICORE_OFFSET * (j - AIC_CNT_PER_DIE);
                } else {
                    base += AICORE_OFFSET * j;
                }

                g_ctrl_t[tid].aicore_spr_1[i][idx] = (uint64_t*)base;
                g_ctrl_t[tid].aicore_spr_2[i][idx] = (uint64_t*)(base + AICORE_SPR_OFFSET); 
                idx++;
            }
        }

        for (size_t i = 0; i < EXE_TYPE_CNT; i++)
        {        
            hand_shake(tid, g_ctrl_t[tid].aicore_spr_1[i], i);
            hand_shake((tid + 1), g_ctrl_t[tid].aicore_spr_2[i], i);
        }
*/


static void hand_shake(int cpu_idx, uint64_t* aicore_spr[], int type, int ostd2_offset) {
    uint64_t base = AICPU_MSGQ_BASE + cpu_idx * AICPU_OFFSET + ostd2_offset * AICPU_MSGQ_OFFSET;
    uint64_t msgq_addr = 0;

    for (size_t i = 0; i < AIC_CNT_PER_THREAD; i++)
    {
        uint64_t offset = type == 0 ? 0 : 128;
        msgq_addr = base + (i + offset)  * AICPU_MSGQ_OFFSET;
        #ifdef REAL_CHIP
        *aicore_spr[i] = HAND_SHAKE_VAL | (msgq_addr & LOAW_ADDR_MASK);
        #endif
        // WORKER_LOGF("cpu_idx,%d, index,%d, aicore_spr,%lx, msgq_addr,%lx", cpu_idx, i, aicore_spr[i], msgq_addr);
    }
}

#if defined(SIM_LATENCY) && !defined(REAL_CHIP)
/* ---------------------------------------------------------------------------
 * Simulated AICore execution.
 *
 * The default build fakes completion inside send_task(): msg_bitmap is set in the same round
 * the task is dispatched, so a task is resident for less than one dispatch round. That makes
 * every multi-round property of the hardware unobservable - in particular a core is never
 * found with one busy slot and one free slot, which is the precondition for planting a
 * successor behind a resident predecessor. This model replaces that fake return with a
 * per (exe_type, core) in-order arbiter over the AIC_OSTD slots:
 *
 *   - pick the slot the phase pointer addresses; if empty, pick its sibling
 *   - decrement that slot's tick counter, and only on reaching zero set msg_bitmap and free it
 *   - flip the phase pointer after a retirement, so the sibling slot is examined next
 *   - reset the phase pointer to slot 0 whenever both slots are empty
 *
 * The last two rules are the hardware model early dispatch is designed against: alternate while
 * busy, favour slot 0 from idle.
 *
 * Ticked synchronously from dispatch(), by the thread that owns the ctrl_t. It must NOT become a
 * separate thread or timer: msg_bitmap is a plain uint64_t and get_completed() clears bits with a
 * non-atomic read-modify-write, so a concurrent setter would have completions silently dropped.
 *
 * Limitation: every task gets the same SIM_TICKS duration. Real per-task durations live in
 * test_graph[].duration, which painter has and dispatch does not. That is fine for reachability
 * and for the ordering oracle below, but it means this model must not be used to argue about
 * makespan - see doc/early-dispatch-case-b.md section 9.
 * ------------------------------------------------------------------------- */
#ifndef SIM_TICKS
#define SIM_TICKS 4
#endif

typedef struct {
    uint32_t task_id;
    uint16_t remaining;
    uint8_t occupied;
    uint8_t started;
} sim_slot_t;

static sim_slot_t g_sim[DISPATCH_THREAD_CNT][EXE_TYPE_CNT][AIC_CNT][AIC_OSTD];
static uint8_t g_sim_next[DISPATCH_THREAD_CNT][EXE_TYPE_CNT][AIC_CNT];
static uint64_t g_sim_now[DISPATCH_THREAD_CNT];

/* Ordering oracle. Written only by the simulator, i.e. only by the owning dispatch thread, so
 * an assertion over these is sound where reading g_state_buf would be racy and lagging. */
uint64_t g_sim_start[RING_SIZE];
uint64_t g_sim_retire[RING_SIZE];

static inline void sim_place(int tid, int type, int core, int slot, uint32_t task_id)
{
    sim_slot_t *s = &g_sim[tid][type][core][slot];
    s->task_id = task_id;
    s->remaining = SIM_TICKS;
    s->occupied = 1;
    s->started = 0;
}

static void sim_tick(int tid)
{
    g_sim_now[tid]++;
    for (int type = 0; type < EXE_TYPE_CNT; type++) {
        for (int core = 0; core < AIC_CNT_PER_THREAD; core++) {
            sim_slot_t *s = g_sim[tid][type][core];
            uint8_t n = g_sim_next[tid][type][core];
            int run = s[n].occupied ? n : (s[n ^ 1].occupied ? (n ^ 1) : -1);
            if (run < 0) {
                g_sim_next[tid][type][core] = 0; /* idle resets the phase pointer */
                continue;
            }
            if (!s[run].started) {
                s[run].started = 1;
                g_sim_start[s[run].task_id] = g_sim_now[tid];
            }
            if (--s[run].remaining == 0) {
                g_sim_retire[s[run].task_id] = g_sim_now[tid];
                s[run].occupied = 0;
                s[run].started = 0;
                g_ctrl_t[tid].msg_bitmap[type][run] |= (uint64_t)0x1 << core;
                g_sim_next[tid][type][core] = run ^ 1; /* examine the sibling next */
            }
        }
    }
}
#endif /* SIM_LATENCY && !REAL_CHIP */

static inline void read_msgq(int tid)
{
    #ifdef REAL_CHIP
    uint64_t msgq_value[4];
    READ_REG(g_ctrl_t[tid].msg_bitmap[0][0], MSGQ_VLD0);
    WRITE_REG(MSGQ_VLD0, g_ctrl_t[tid].msg_bitmap[0][0]);

    READ_REG(g_ctrl_t[tid].msg_bitmap[0][1], MSGQ_VLD1);
    WRITE_REG(MSGQ_VLD1, g_ctrl_t[tid].msg_bitmap[0][1]);

    READ_REG(g_ctrl_t[tid].msg_bitmap[1][0], MSGQ_VLD2);
    WRITE_REG(MSGQ_VLD2, g_ctrl_t[tid].msg_bitmap[1][0]);

    READ_REG(g_ctrl_t[tid].msg_bitmap[1][1], MSGQ_VLD3);
    WRITE_REG(MSGQ_VLD3, g_ctrl_t[tid].msg_bitmap[1][1]);
    #endif

    for (int i = 0; i < EXE_TYPE_CNT; i++) {
        for (int j = 0; j < AIC_OSTD; j++) {
            g_ctrl_t[tid].free_bitmap[i][j] |= g_ctrl_t[tid].msg_bitmap[i][j];
        }
    }

    /* set_mix() is deliberately not called.
     *
     * TASK_TYPE_MIX == TASK_TYPE_VECTOR == 1 (common/task.h), so it expands to
     *     free_bitmap[1] = free_bitmap[0] & free_bitmap[1]
     * i.e. it clobbers vector's own bitmap instead of maintaining a separate MIX one.
     * Bits in free_bitmap[1] are only ever set by the msg_bitmap[1] merge above (or by the
     * initial mask in init_ctrl_t), while set_mix() only clears. So a core busy with a
     * type-0 task loses its type-1 bit as collateral, and the type-0 completion restores
     * free_bitmap[0] alone - the type-1 bit never comes back.
     *
     * This is invisible today only because send_task() fakes completion in the same round it
     * dispatches (#ifndef REAL_CHIP), so free_bitmap[0] is already restored by the time
     * set_mix() runs and the AND is a no-op. The moment a task holds a core for more than one
     * round, every core drifts out of type-1 eligibility and the type-1 half of the DAG
     * (462 of 864 tasks) can never be dispatched.
     *
     * Dropping it costs nothing: the 60 mix tasks (out_proj) are encoded as type 1 in
     * cases/qwen3_14b_decode_subgraph.h and are indistinguishable from cube tasks, so no
     * dual-unit reservation is expressible either way. Real MIX semantics would need a third
     * type value from the graph generator, TASK_TYPE_CNT raised to 3, and set_mix() rewritten
     * against that separate index. */
}

static inline void get_completed(uint64_t* bitmap, uint32_t task_id[], int *complete_cnt,
                                 const uint32_t task_id_map[])
{
    int cnt = __builtin_popcountll(*bitmap);
    while (cnt > 0) {
        uint64_t idx = (uint64_t)__builtin_ctzll(*bitmap);
        task_id[(*complete_cnt)] = task_id_map[idx];
        WORKER_LOGF("completed,task_id,%u,complete_cnt,%d,core,%d,bitmap,%u",task_id_map[idx], *complete_cnt,  idx, *bitmap);
        (*complete_cnt)++;
        cnt--;
        *bitmap &= (*bitmap - 1);
    }
}

static inline void push_2_completed_queue(int tid)
{
    uint32_t task_id[240];
    int complete_cnt = 0;
    for (int i = 0; i < EXE_TYPE_CNT; i++) {
        get_completed(&g_ctrl_t[tid].msg_bitmap[i][0], task_id, &complete_cnt,
                      g_ctrl_t[tid].task_id_map1[i]);
        get_completed(&g_ctrl_t[tid].msg_bitmap[i][1], task_id, &complete_cnt,
                      g_ctrl_t[tid].task_id_map2[i]);
    }
    batch_enqueue(&g_ctrl_t[tid].completed_queue, task_id, (uint32_t)complete_cnt);
    batch_enqueue(&g_ctrl_t[tid].remote_completed_queue, task_id, (uint32_t)complete_cnt);
}

static inline int send_task(ctrl_t *ctrl, int type)
{
    /* A core is a candidate when EITHER of its AIC_OSTD slots is free, so a task can be queued
     * behind one that is still executing. With '&' here a core was only ever selected while both
     * slots were free, which made slot 1 unreachable and capped every core at one outstanding
     * task - and it is exactly that queueing that early dispatch needs, because a successor can
     * only be planted behind a predecessor that is already resident.
     *
     * cnt must equal the number of tasks the loop below can actually place. That loop drops the
     * whole core from free_bitmap after placing one task, so the bound is popcount of the OR,
     * not the sum of the two popcounts: asking batch_dequeue for more would hand back tasks the
     * loop cannot place and then call __builtin_ctzll(0), whose result is undefined. A core with
     * both slots free therefore takes one task this round and the second one next round. */
    uint64_t free_bitmap = ctrl->free_bitmap[type][0] | ctrl->free_bitmap[type][1];
    int cnt = __builtin_popcountll(free_bitmap);
    if (cnt <= 0) {
        WORKER_LOGF("send,free_cnt,%d", cnt);
        return 0;
    }
    uint32_t task_ids[AIC_CNT];
    if (!batch_dequeue(&ctrl->ready_queue[type], task_ids, &cnt)){
        return 0;
    }
    
    int sent = 0;
    for (int i = 0; i < cnt; i++) {
        uint32_t task_id = task_ids[i];
        uint64_t idx = (uint64_t)__builtin_ctzll(free_bitmap);

        uint64_t mask = (uint64_t)0x1 << idx;
        /* Prefer slot 0. This is load-bearing, not cosmetic: on an idle core the AICore examines
         * slot 0 first, so filling slot 0 before slot 1 makes the software fill order agree with
         * the hardware pickup order, and a task queued behind a resident one always lands in the
         * higher slot. Early dispatch Case A depends on the predecessor occupying the lower slot. */
        int slot = (ctrl->free_bitmap[type][0] & mask) != 0 ? 0 : 1;
        // Set executor's tasks and duration
        int core = (int)idx;
        
        if (slot == 1) {
            ctrl->task_id_map2[type][idx] = task_id;
            #ifdef REAL_CHIP
            *ctrl->aicore_spr_2[type][idx] = task_id;
            #endif
        } else {
            ctrl->task_id_map1[type][idx] = task_id;
            #ifdef REAL_CHIP
            *ctrl->aicore_spr_1[type][idx] = task_id;
            #endif
        }
        
        // Clear the free bit for this core/slot combination (mark as busy)
        ctrl->free_bitmap[type][slot] &= ~mask;

        #ifndef REAL_CHIP
        #ifdef SIM_LATENCY
        sim_place((int)ctrl->tid, type, core, slot, task_id);
        #else
        ctrl->msg_bitmap[type][slot] |= mask;   /* fake return: retires next round */
        #endif
        #endif

        WORKER_LOGF("send,task_id,%u,core,%d,slot,%d,type,%d", task_id, core, slot, type);
        sent++;
        free_bitmap &= ~mask;
    }
    return sent;
}

int dispatch(int tid)
{
    int total_sent = 0;
#if defined(SIM_LATENCY) && !defined(REAL_CHIP)
    /* Advance the simulated cores before observing them. Must run in this thread - see the
     * comment on sim_tick(). */
    sim_tick(tid);
#endif
    read_msgq(tid);
    push_2_completed_queue(tid);
    total_sent += send_task(&g_ctrl_t[tid], TASK_TYPE_MIX);
    total_sent += send_task(&g_ctrl_t[tid], TASK_TYPE_VECTOR);
    total_sent += send_task(&g_ctrl_t[tid], TASK_TYPE_CUBE);
    return total_sent;
}

/*
 * Dispatch worker thread entry point Runs the dispatch loop for task distribution
 */
void *dispatch_worker(void *arg)
{
    int tid = (int)(intptr_t)arg;
    int total_sent = 0;
    WORKER_LOGF("dispatch,%d,start", tid);

    for (size_t i = 0; i < EXE_TYPE_CNT; i++)
    {        
        hand_shake(tid, g_ctrl_t[tid].aicore_spr_1[i], i, 0);
        hand_shake(tid, g_ctrl_t[tid].aicore_spr_2[i], i, 64);
    }

    // atomic_store_explicit(&g_is_done, true, memory_order_release);
    // return NULL;

    bool is_done = false;
    while (!is_done) {
        total_sent += dispatch(tid);
        is_done = atomic_load(&g_is_done);
    }
    WORKER_LOGF("dispatch,%d,done", tid);
    return NULL;
}
