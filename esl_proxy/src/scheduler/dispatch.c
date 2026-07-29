/*
 * dispatch.c - Dispatch Worker Thread Implementation
 *
 * Worker thread entry point for Dispatch.
 * This file is compiled separately as it contains pthread-specific code.
 */
#include <stdint.h>
#include <stdio.h>

#include "scheduler/dispatch.h"
#include "scheduler/early_dispatch.h"
#include "common/task.h"
#include "common/log.h"
#include "platform/a6.h"

extern atomic_bool g_is_done;

ctrl_t g_ctrl_t[DISPATCH_THREAD_CNT];

/* Owned by dispatch: set at the single send point, covering both the early and the normal path.
 * Kept live even when EARLY_DISPATCH is off so that "every task is dispatched exactly once" can
 * be asserted identically in both configurations. */
uint8_t g_early_dispatched[RING_SIZE];
uint32_t g_early_plants_b;
uint32_t g_early_plants_a;
uint32_t g_early_skipped_dup;

/* Planted pairs, kept so early_dispatch_report() can assert start(S) >= retire(P). The bound is
 * generous: the qwen3 DAG admits at most 6 plants (doc/early-dispatch-case-b.md section 10). */
#define EARLY_MAX_PLANTS 512
static uint32_t g_early_pair[EARLY_MAX_PLANTS][2];
static int g_early_pair_n;

static inline void early_record_plant(uint32_t p, uint32_t s)
{
    if (g_early_pair_n < EARLY_MAX_PLANTS) {
        g_early_pair[g_early_pair_n][0] = p;
        g_early_pair[g_early_pair_n][1] = s;
        g_early_pair_n++;
    }
}

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
                if (!s[0].occupied && !s[1].occupied) {
                    /* That retirement emptied the core, so the phase pointer resets to slot 0 NOW,
                     * not on the next arbitration cycle. The distinction is load-bearing for Case
                     * A and the ordering assertion caught it: dispatch can observe a core as fully
                     * idle and co-dispatch into both slots within the same round the last task
                     * retired. If the pointer were still addressing the sibling at that moment,
                     * the successor in the higher slot would be examined first and would run
                     * before its own predecessor. See doc/early-dispatch-case-a.md section 3. */
                    g_sim_next[tid][type][core] = 0;
                }
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

/* Single placement primitive: write the task into (type, core, slot), record the reverse map used
 * to translate completion bits back into task ids, and mark the slot busy. Shared by the normal
 * ready path and by early dispatch so the two cannot drift apart. */
static inline void place_task(ctrl_t *ctrl, int type, int core, int slot, uint32_t task_id)
{
    uint64_t mask = (uint64_t)0x1 << core;

    if (slot == 1) {
        ctrl->task_id_map2[type][core] = task_id;
        #ifdef REAL_CHIP
        *ctrl->aicore_spr_2[type][core] = task_id;
        #endif
    } else {
        ctrl->task_id_map1[type][core] = task_id;
        #ifdef REAL_CHIP
        *ctrl->aicore_spr_1[type][core] = task_id;
        #endif
    }

    ctrl->free_bitmap[type][slot] &= ~mask;
    g_early_dispatched[task_id] = 1;

    #ifndef REAL_CHIP
    #ifdef SIM_LATENCY
    sim_place((int)ctrl->tid, type, core, slot, task_id);
    #else
    ctrl->msg_bitmap[type][slot] |= mask;   /* fake return: retires next round */
    #endif
    #endif
}

#ifdef EARLY_DISPATCH
/* Case B: for every core holding exactly one task, if that task has a pending hint, plant the
 * waiting successor into the free sibling slot.
 *
 * Called from dispatch() after read_msgq() and after push_2_completed_queue(), and that position
 * is load-bearing in both directions. After read_msgq() so free_bitmap reflects this round's
 * completions, and after push_2_completed_queue() because place_task() overwrites the task_id_map
 * entry for the slot it fills while push_2_completed_queue() still needs that entry to translate
 * this round's completion bits back into task ids.
 *
 * "Exactly one busy slot" is the observable proxy for "the resident task is executing": with an
 * in-order slot pair, a core holding a single task has nothing ahead of it. When that task
 * retires the sibling slot holds exactly one candidate, so no arbitration rule can reorder them -
 * which is why Case B needs only non-preemption and not the idle-core slot-order property. */
static int plant_pass(int tid)
{
    ctrl_t *ctrl = &g_ctrl_t[tid];
    int planted = 0;

    for (int type = 0; type < EXE_TYPE_CNT; type++) {
        /* XOR: exactly one of the two slot bits is free, hence exactly one slot is busy. */
        uint64_t cand = ctrl->free_bitmap[type][0] ^ ctrl->free_bitmap[type][1];
        while (cand) {
            uint64_t idx = (uint64_t)__builtin_ctzll(cand);
            uint64_t mask = (uint64_t)0x1 << idx;
            cand &= cand - 1;

            int free_slot = (ctrl->free_bitmap[type][0] & mask) != 0 ? 0 : 1;
            uint32_t p = free_slot == 1 ? ctrl->task_id_map1[type][idx]
                                        : ctrl->task_id_map2[type][idx];
            uint32_t s = g_early_hint[p];
            if (s == EARLY_NONE) {
                continue;
            }
            /* Consume the hint whether or not the plant happens, so a stale hint cannot be
             * retried forever against a predecessor that has since retired. */
            g_early_hint[p] = EARLY_NONE;
            if (g_early_dispatched[s]) {
                g_early_skipped_dup++;
                continue;
            }
            place_task(ctrl, type, (int)idx, free_slot, s);
            early_record_plant(p, s);
            g_early_plants_b++;
            planted++;
            WORKER_LOGF("early,plant_b,successor,%u,predecessor,%u,core,%d,slot,%d",
                        s, p, (int)idx, free_slot);
        }
    }
    return planted;
}
#endif /* EARLY_DISPATCH */

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

        /* A task that was already planted early will also arrive here once its indegree reaches
         * zero. Skipping it is the single choke point that keeps "dispatched exactly once" true:
         * a second dispatch would report the task complete twice, and resolve_dep() has no
         * idempotence guard, so every successor's indegree would be double-decremented and
         * released while a real predecessor is still executing. Note the core is NOT consumed. */
        if (g_early_dispatched[task_id]) {
            g_early_skipped_dup++;
            WORKER_LOGF("early,skip_dup,task_id,%u,type,%d", task_id, type);
            continue;
        }

        uint64_t idx = (uint64_t)__builtin_ctzll(free_bitmap);
        uint64_t mask = (uint64_t)0x1 << idx;
        /* Prefer slot 0. This is load-bearing, not cosmetic: on an idle core the AICore examines
         * slot 0 first, so filling slot 0 before slot 1 makes the software fill order agree with
         * the hardware pickup order, and a task queued behind a resident one always lands in the
         * higher slot. Early dispatch Case A depends on the predecessor occupying the lower slot. */
        int slot = (ctrl->free_bitmap[type][0] & mask) != 0 ? 0 : 1;
        int core = (int)idx;

#ifdef EARLY_DISPATCH_CASE_A
        /* Case A: this task is about to be placed, and if the core is FULLY idle its sibling slot
         * is free too, so a successor waiting only on this task can ride along in the same round.
         *
         * Readiness of the predecessor is implied by construction here - it came out of
         * ready_queue, so its indegree is 0. That is the whole reason the hint is consumed at this
         * point rather than re-derived: "not yet dispatched" is NOT the same as "ready" (in chain
         * X -> A -> B with X unfinished, both A and B have one unfinished predecessor and A is
         * undispatched, but A must not run yet).
         *
         * Ordering rests on the predecessor taking the LOWER slot: an idle core examines slot 0
         * first, so writing P to slot 0 and S to slot 1 is correct both under a phase pointer that
         * resets to 0 on idle and under a plain lowest-index-first arbiter. Since slot is chosen
         * with a slot-0 preference above, a fully idle core always yields slot == 0 here. */
        int idle = (ctrl->free_bitmap[type][0] & mask) != 0 && (ctrl->free_bitmap[type][1] & mask) != 0;
        uint32_t hint_s = EARLY_NONE;
        if (idle && slot == 0) {
            hint_s = g_early_hint[task_id];
            if (hint_s != EARLY_NONE) {
                g_early_hint[task_id] = EARLY_NONE;   /* consume once, win or lose */
                if (g_early_dispatched[hint_s]) {
                    g_early_skipped_dup++;
                    hint_s = EARLY_NONE;
                }
            }
        }
#endif

        place_task(ctrl, type, core, slot, task_id);

        WORKER_LOGF("send,task_id,%u,core,%d,slot,%d,type,%d", task_id, core, slot, type);
        sent++;

#ifdef EARLY_DISPATCH_CASE_A
        if (hint_s != EARLY_NONE) {
            /* Second write, higher slot, after the predecessor's - the order matters and under
             * REAL_CHIP these are MMIO stores that must not be reordered. */
            place_task(ctrl, type, core, 1, hint_s);
            early_record_plant(task_id, hint_s);
            g_early_plants_a++;
            WORKER_LOGF("early,plant_a,successor,%u,predecessor,%u,core,%d,slot,1",
                        hint_s, task_id, core);
        }
#endif
        free_bitmap &= ~mask;
    }
    return sent;
}

/* Acceptance output. Deliberately plain printf from main() rather than WORKER_LOGF: that macro is
 * silenced by SCHEDULER_LOG=0, which is the documented performance mode, so counters and timing
 * could otherwise never come from the same run. Returns 0 when every check passed. */
int early_dispatch_report(void)
{
    int failures = 0;
    uint32_t dispatched = 0;

    for (uint32_t i = 0; i < RING_SIZE; i++) {
        if (g_early_dispatched[i]) {
            dispatched++;
        }
    }

    printf("\n[early-dispatch] mode=%s%s latency=%s\n",
#ifdef EARLY_DISPATCH
           "on",
#else
           "off",
#endif
#ifdef EARLY_DISPATCH_CASE_A
           "+caseA",
#else
           "",
#endif
#if defined(SIM_LATENCY) && !defined(REAL_CHIP)
           "sim"
#else
           "fake-return"
#endif
    );
    printf("[early-dispatch] dispatched_tasks   = %u\n", dispatched);
    printf("[early-dispatch] hints_published    = %u\n", g_early_hints_published);
    printf("[early-dispatch] plants_case_b      = %u\n", g_early_plants_b);
    printf("[early-dispatch] plants_case_a      = %u\n", g_early_plants_a);
    printf("[early-dispatch] skipped_duplicate  = %u\n", g_early_skipped_dup);

    /* Ordering oracle: a planted successor must not start before its predecessor retired. The
     * timestamps come from the simulator, which is single-writer inside the owning dispatch
     * thread; asserting against g_state_buf instead would be both racy and lagging. */
#if defined(SIM_LATENCY) && !defined(REAL_CHIP)
    for (int i = 0; i < g_early_pair_n; i++) {
        uint32_t p = g_early_pair[i][0];
        uint32_t s = g_early_pair[i][1];
        if (g_sim_start[s] == 0 || g_sim_retire[p] == 0) {
            printf("[early-dispatch] FAIL pair(P=%u,S=%u) never ran: start(S)=%llu retire(P)=%llu\n",
                   p, s, (unsigned long long)g_sim_start[s], (unsigned long long)g_sim_retire[p]);
            failures++;
        } else if (g_sim_start[s] < g_sim_retire[p]) {
            printf("[early-dispatch] FAIL pair(P=%u,S=%u) out of order: "
                   "start(S)=%llu < retire(P)=%llu\n",
                   p, s, (unsigned long long)g_sim_start[s],
                   (unsigned long long)g_sim_retire[p]);
            failures++;
        }
    }
    printf("[early-dispatch] ordering_pairs     = %d checked, %d failed\n",
           g_early_pair_n, failures);
#else
    printf("[early-dispatch] ordering_pairs     = not checked (needs SIM_LATENCY)\n");
#endif

    return failures;
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
#ifdef EARLY_DISPATCH
    /* AFTER push_2_completed_queue(), and that ordering is load-bearing. place_task() overwrites
     * task_id_map for the slot it fills, and push_2_completed_queue() reads that same map to turn
     * completion bits back into task ids. Planting first therefore made a retiring task's
     * completion be reported under the PLANTED id: the real task was never reported complete and
     * the DAG hung, while the planted task was reported complete before it had run. Once
     * completions are drained the map entry for a freed slot is dead and safe to overwrite. */
    plant_pass(tid);
#endif
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
