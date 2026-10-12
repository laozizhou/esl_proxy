/*
 * dispatch.c - Dispatch Worker Thread Implementation
 *
 * Worker thread entry point for Dispatch.
 * This file is compiled separately as it contains pthread-specific code.
 */
#include <assert.h>
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
uint32_t g_early_plants;
uint32_t g_early_skipped_dup;
#ifdef EARLY_DISPATCH_CROSS_TYPE
uint32_t g_cross_plants;

/* The cross-type wait (wait_target/g_sim_cnt, see sim_tick()) only exists under
 * the SIM_LATENCY arbiter. Without it, place_task()'s fake-return path marks a task complete the
 * round after it is sent regardless of wait_target, so a cross-type S would "finish" without
 * ever actually waiting for P - silently wrong, not just untested. REAL_CHIP has no notify wiring
 * yet either (see cross_dispatch.md future work), so it is excluded the same way. */
#if !(defined(SIM_LATENCY) && !defined(REAL_CHIP))
#error "EARLY_DISPATCH_CROSS_TYPE requires SIM_LATENCY and no REAL_CHIP"
#endif
#endif

/* Planted pairs, kept so early_dispatch_report() can assert start(S) >= retire(P). Same-type-only
 * runs stay in the single digits (doc/early-dispatch-case-b.md section 10), but cross-type plants
 * on the qwen3 DAG have been observed in the 200-300 range per run - keep this comfortably above
 * that, not just above the same-type figure. */
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

#ifdef EARLY_DISPATCH_CROSS_TYPE
/* Placement counter per engine, the dispatch thread's own shadow of the engine retire counter:
 * incremented once per place_task(), never read back from the simulator or from hardware. Every
 * task placed on an engine retires on it exactly once, so "P was the j-th task placed here" and
 * "that engine's retire counter reaches j" are the same event, and j is a pure function of this
 * thread's own placement history. That is what lets a cross-type waiter be armed with an absolute
 * target without ever reading a counter, so there is no read-modify-write window for a retirement
 * to slip through between observing P and arming its successor. The value a waiter is armed with
 * comes from g_place_seq[P], not from g_placed[] at arming time - see plant_pass().
 *
 * It rests on one property: on a given engine, retirement order equals placement order. The 2-slot
 * arbiter provides it (non-preemptive, the phase pointer advances only on retirement, and
 * send_task() prefers slot 0 so software fill order matches hardware pickup order) - the same
 * property same-type early dispatch already depends on, which is why binding the counter to the
 * engine rather than to the slot adds no new assumption. g_place_seq[] records the number handed
 * out so sim_tick() can assert the property on every retirement rather than trusting it. */
static uint32_t g_placed[DISPATCH_THREAD_CNT][EXE_TYPE_CNT][AIC_CNT];
static uint32_t g_place_seq[RING_SIZE];
#endif

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
    uint32_t wait_target;     /* cross-type early dispatch: hold until the paired engine's retire
                               * counter reaches this value. 0 = not waiting - an unsigned counter
                               * is always >= 0, so the comparison simply never holds. uint32_t
                               * keeps sim_slot_t at 12 bytes; placements per engine are bounded
                               * by the task count. */
} sim_slot_t;

static sim_slot_t g_sim[DISPATCH_THREAD_CNT][EXE_TYPE_CNT][AIC_CNT][AIC_OSTD];
static uint8_t g_sim_next[DISPATCH_THREAD_CNT][EXE_TYPE_CNT][AIC_CNT];
static uint64_t g_sim_now[DISPATCH_THREAD_CNT];

/* Retire counter per engine, monotone and never cleared: g_sim_cnt[tid][type][core] counts the
 * tasks that have retired on engine (type, core). A cross-type successor resident on (type, core)
 * compares its target against its paired engine's counter, g_sim_cnt[tid][type ^ 1][core]. The
 * pairing is 1:1 by core index - core i of one type pairs with core i of the other, and
 * EXE_TYPE_CNT==2 makes type^1 always the paired type. A real 1:2 topology breaks both halves.
 *
 * What replaces the one-bit mailbox is the VALUE, not the direction. Nothing ever clears this, so
 * neither failure direction of the mailbox survives: there is no stale 1 to read, and no real 1
 * that can be cleared out from under a waiter. A waiter armed after the signal would have been
 * sent is still correct, because it compares against an absolute value instead of waiting for an
 * edge. None of that depends on who performs the write.
 *
 * The simulator models it as a value each engine owns and the paired engine reads, which is the
 * compact form - but "can an engine READ its pair's register" is listed as unconfirmed in
 * notes/sim_tick.md section 4. It does not have to be read. The retiring engine can instead PUSH
 * its new counter value into the paired engine's SPR unconditionally, exactly as the old mailbox
 * pushed a bit, and the waiter compares against that locally held value. Same primitive the
 * project already has evidence for, same properties above. Do not treat the read form as a
 * hardware requirement of this design.
 *
 * On real hardware the counter is finite-width. Two things follow, not one: the comparison must
 * become a signed difference, (int64_t)(cnt - target) < 0, AND the target must be truncated to the
 * counter's width when it is armed, or the two sides are taken modulo different values and the
 * signed-difference trick fails. It also assumes a waiter is never more than half a counter range
 * behind. None of this bites here; uint32_t over a few hundred placements never wraps.
 *
 * See g_placed[] for where the value a waiter compares against comes from. */
static uint32_t g_sim_cnt[DISPATCH_THREAD_CNT][EXE_TYPE_CNT][AIC_CNT];

/* Retirements of the tick in progress, merged into g_sim_cnt once every engine has been visited.
 * Without this, visibility would depend on the order sim_tick() happens to walk its engines: a
 * type-0 retirement would already be in g_sim_cnt when the type-1 engines are examined later in
 * the SAME call, so a cube->vector successor would start on the very tick its predecessor retired
 * while a vector->cube one waited a tick, and a same-type successor - which shares one engine, and
 * an engine is examined once per tick - always waited a tick. The engine needing real inter-engine
 * signalling would have been modelled as the fastest of the three.
 *
 * Staging makes a tick an atomic step: every engine observes the counters as they stood at the
 * start of the tick, and all updates land at the end, so the walk order stops mattering and every
 * handoff costs exactly one tick. Same reason a cellular automaton computes the next generation
 * from the previous board rather than updating in place. */
static uint32_t g_sim_cnt_pending[DISPATCH_THREAD_CNT][EXE_TYPE_CNT][AIC_CNT];

/* Ordering oracle. Written only by the simulator, i.e. only by the owning dispatch thread, so
 * an assertion over these is sound where reading g_state_buf would be racy and lagging. */
uint64_t g_sim_start[RING_SIZE];
uint64_t g_sim_retire[RING_SIZE];

static inline void sim_place(int tid, int type, int core, int slot, uint32_t task_id,
                             uint64_t wait_target)
{
    sim_slot_t *s = &g_sim[tid][type][core][slot];
    s->task_id = task_id;
    s->remaining = SIM_TICKS;
    s->occupied = 1;
    s->started = 0;
    s->wait_target = wait_target;
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

            /* Cross-type early dispatch: held here until the paired engine has retired as many
             * tasks as this successor was told to wait for. Neither ticked nor marked started
             * while waiting. No latch is needed - the counter never decreases, so once the
             * comparison passes it passes on every later tick too, and re-reading cannot see a
             * cleared or stale value.
             *
             * Counter updates are staged to the end of the tick (see g_sim_cnt_pending), so the
             * value read here is always the one from the start of this tick. A successor therefore
             * starts exactly one tick after its predecessor retired, in every direction and for
             * both the same-type and cross-type paths, and start(S) > retire(P) holds strictly -
             * the ordering oracle keeps a tick of margin instead of resting on equality. */
            if (g_sim_cnt[tid][type ^ 1][core] < s[run].wait_target) {
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
                g_sim_cnt_pending[tid][type][core]++; /* visible to other engines next tick */
                #ifdef EARLY_DISPATCH_CROSS_TYPE
                /* Retirement order equals placement order on this engine. Necessary for the
                 * target scheme - if the arbiter ever retired out of order, every target on this
                 * engine would name a different task's retirement - but not sufficient: a target
                 * can still name the wrong task while this holds, if it was derived from the
                 * wrong sequence number in the first place.
                 *
                 * Note what this can and cannot cover. EARLY_DISPATCH_CROSS_TYPE implies
                 * SIM_LATENCY && !REAL_CHIP (see the #error at the top), so this never executes
                 * against real silicon - it is a regression guard on the arbiter in sim_tick()
                 * just above, in particular the g_sim_next handling, which has needed a fix once
                 * already. The same property on real hardware is an open question for the
                 * hardware owners, not something this assert answers. */
                assert(g_sim_cnt[tid][type][core] + g_sim_cnt_pending[tid][type][core]
                       == g_place_seq[s[run].task_id]);
                #endif
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

    /* End of tick: publish this tick's retirements. Nothing above reads g_sim_cnt_pending except
     * the assert, so no engine can have observed them while the walk was still in progress. */
    for (int type = 0; type < EXE_TYPE_CNT; type++) {
        for (int core = 0; core < AIC_CNT_PER_THREAD; core++) {
            g_sim_cnt[tid][type][core] += g_sim_cnt_pending[tid][type][core];
            g_sim_cnt_pending[tid][type][core] = 0;
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
 * ready path and by early dispatch so the two cannot drift apart.
 *
 * wait_target is forwarded to sim_place() unchanged: non-zero only for a cross-type S placed onto
 * an idle destination, where it names the value the predecessor's engine counter must reach, and 0
 * for everything else. See sim_slot_t for the encoding. */
static inline void place_task(ctrl_t *ctrl, int type, int core, int slot, uint32_t task_id,
                               uint64_t wait_target)
{
    uint64_t mask = (uint64_t)0x1 << core;

    /* The caller picked this slot; it must actually be free. Overwriting a live resident is
     * silent otherwise - free_bitmap's bit is already clear so the clear below is a no-op, and
     * the damage only surfaces later as a retire-order assert or a hang. Cheap to check here,
     * where the culprit is still on the stack. */
    assert((ctrl->free_bitmap[type][slot] & mask) != 0);

    #ifdef EARLY_DISPATCH_CROSS_TYPE
    /* Pre-increment: the first task placed on an engine gets sequence 1, so 0 stays available as
     * "not waiting" in wait_target. */
    g_place_seq[task_id] = ++g_placed[ctrl->tid][type][core];
    #endif

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
    sim_place((int)ctrl->tid, type, core, slot, task_id, wait_target);
    #else
    ctrl->msg_bitmap[type][slot] |= mask;   /* fake return: retires next round */
    #endif
    #endif
}

#ifdef EARLY_DISPATCH
/* For every core holding exactly one task, if that task has a pending hint, plant the waiting
 * successor into the free sibling slot (same type) or onto the paired core (cross type).
 *
 * Called from dispatch() after read_msgq(), after push_2_completed_queue(), and after all three
 * send_task() calls - see the ordering comment on dispatch() for why each position is load-bearing.
 *
 * "Exactly one busy slot" is the observable proxy for "the resident task is executing": with an
 * in-order slot pair, a core holding a single task has nothing ahead of it, so no arbitration rule
 * can let anything else retire before it - this holds regardless of how that task got there,
 * whether freshly placed this same round or resident for many rounds already. */
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
            if (s != EARLY_NONE) {
                /* Consume the hint whether or not the plant happens, so a stale hint cannot be
                 * retried forever against a predecessor that has since retired. Always safe to
                 * consume here: with the sibling slot already free, the only way to fail is a
                 * stale duplicate, never "try again next round". */
                g_early_hint[p] = EARLY_NONE;
                if (g_early_dispatched[s]) {
                    g_early_skipped_dup++;
                } else {
                    place_task(ctrl, type, (int)idx, free_slot, s, 0);
                    early_record_plant(p, s);
                    g_early_plants++;
                    planted++;
                    WORKER_LOGF("early,plant,successor,%u,predecessor,%u,core,%d,slot,%d",
                                s, p, (int)idx, free_slot);
                }
            }

#ifdef EARLY_DISPATCH_CROSS_TYPE
            /* P has nothing ahead of it on its own core (same "exactly one busy slot" proof as
             * above), so a pending cross-type successor is safe to place as soon as its
             * destination - the paired core, same index, opposite type - has a free slot.
             * Same-type and cross-type hints are independent (two different successors can each
             * have P as their last predecessor), so this never branches off the outcome above.
             *
             * Unlike the same-type hint, this one is NOT consumed on a failed destination check:
             * "both destination slots busy right now" is not "stale", so the pair is left for a
             * later round. That retry is opportunistic, not guaranteed - the same-type block above
             * may fill P's sibling slot in this very iteration, which takes P's core to XOR == 0
             * and stops it ever being a candidate again, and P may simply retire first. In either
             * case the hint is dead and cs reaches a unit through ready_queue instead. */
            uint32_t cs = g_cross_hint[p];
            if (cs != EARLY_NONE) {
                if (g_early_dispatched[cs]) {
                    g_cross_hint[p] = EARLY_NONE;
                    g_early_skipped_dup++;
                } else {
                    int ctype = type ^ 1;
                    /* One free slot is enough, same admission rule send_task() uses. No wait cycle
                     * can form between two engines, and that does not depend on how idle the
                     * destination is: every edge of the wait-for graph points strictly backwards
                     * in placement time. A successor's target names a task that was already
                     * resident when the successor was armed, and an engine runs its residents in
                     * placement order, so a task can only be held up by something placed before
                     * it. A cycle would need an edge pointing forward, and there is none.
                     *
                     * This also lowers the average occupancy cost of a plant, though it does not
                     * remove it. Under the old rule every plant parked on a fully idle engine and
                     * the phase pointer selected it at once, so the wait stalled that engine for
                     * essentially a whole task duration. The relaxed rule is a superset - plenty
                     * of plants still land on an empty destination and still stall it - but the
                     * ones that land behind a running task wait for free. Measured stall per plant
                     * falls to roughly 0.5-0.8x. The trade is more head-of-line blocking: a
                     * waiting head stalls its sibling too, and a blocked speculative task can now
                     * take an engine's last free slot, which the old rule structurally prevented. */
                    bool dest_idle = (ctrl->free_bitmap[ctype][0] & mask) != 0
                                   || (ctrl->free_bitmap[ctype][1] & mask) != 0;
                    if (dest_idle) {
                        /* Read P's own recorded sequence rather than the engine's current
                         * g_placed[]: the same-type block above may already have planted a
                         * different successor onto this very engine, which would have bumped
                         * g_placed past P and made cs wait for that successor instead. Two
                         * successors legitimately sharing one P is routine - the hints are
                         * independent, see the comment above - so this is not a corner case.
                         *
                         * Snapshotting here is safe even if P retires before the comparison is
                         * first evaluated: the target is absolute, so an already-satisfied one
                         * simply never blocks. That is the property the old one-bit mailbox
                         * lacked. */
                        uint64_t wait_target = g_place_seq[p];
                        /* Picking the free slot is a correctness requirement of the relaxed
                         * admission test above, not a style match with send_task(). While the test
                         * demanded both slots free, hardcoding slot 0 was safe. Now the common
                         * case is slot 0 busy and slot 1 free, where a hardcoded 0 would overwrite
                         * a live resident: sim_place() replaces the slot, task_id_map1 is
                         * clobbered, and free_bitmap's bit is already clear so nothing flags it.
                         * The overwritten task never retires and its engine's counter stays one
                         * short of every target armed against it afterwards. These two lines and
                         * the || above must never be separated.
                         *
                         * Slot 0 first, like every other placement site: nothing is ever placed
                         * into slot 1 while slot 0 is free, which is what keeps per-engine
                         * retirement order equal to placement order. */
                        int cslot = (ctrl->free_bitmap[ctype][0] & mask) != 0 ? 0 : 1;
                        g_cross_hint[p] = EARLY_NONE;
                        place_task(ctrl, ctype, (int)idx, cslot, cs, wait_target);
                        early_record_plant(p, cs);
                        g_cross_plants++;
                        planted++;
                        WORKER_LOGF("early,cross_plant,successor,%u,predecessor,%u,core,%d,"
                                    "type,%d,slot,%d", cs, p, (int)idx, ctype, cslot);
                    }
                }
            }
#endif
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
         * higher slot. */
        int slot = (ctrl->free_bitmap[type][0] & mask) != 0 ? 0 : 1;
        int core = (int)idx;

        place_task(ctrl, type, core, slot, task_id, 0);

        WORKER_LOGF("send,task_id,%u,core,%d,slot,%d,type,%d", task_id, core, slot, type);
        sent++;
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

    printf("\n[early-dispatch] latency=%s early_dispatch=%s cross_type=%s\n",
#if defined(SIM_LATENCY) && !defined(REAL_CHIP)
           "sim",
#else
           "fake-return",
#endif
#ifdef EARLY_DISPATCH
           "on",
#else
           "off",
#endif
#ifdef EARLY_DISPATCH_CROSS_TYPE
           "on"
#else
           "off"
#endif
    );
    printf("[early-dispatch] dispatched_tasks         = %u\n", dispatched);
    printf("[early-dispatch] same_type_hints_published = %u\n", g_early_hints_published);
    printf("[early-dispatch] same_type_plants          = %u\n", g_early_plants);
#ifdef EARLY_DISPATCH_CROSS_TYPE
    printf("[early-dispatch] cross_type_hints_published = %u\n", g_cross_hints_published);
    printf("[early-dispatch] cross_type_plants          = %u\n", g_cross_plants);
#endif
    printf("[early-dispatch] skipped_duplicate         = %u\n", g_early_skipped_dup);

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
    total_sent += send_task(&g_ctrl_t[tid], TASK_TYPE_MIX);
    total_sent += send_task(&g_ctrl_t[tid], TASK_TYPE_VECTOR);
    total_sent += send_task(&g_ctrl_t[tid], TASK_TYPE_CUBE);
#ifdef EARLY_DISPATCH
    /* AFTER push_2_completed_queue() (place_task() overwrites task_id_map for the slot it fills,
     * and push_2_completed_queue() reads that same map to turn this round's completion bits back
     * into task ids - planting first would make a retiring task's completion get reported under
     * the PLANTED id instead).
     *
     * AFTER all three send_task() calls too, and that ordering is now equally load-bearing: a
     * core showing "exactly one busy slot" is also the state a genuinely-ready task from
     * ready_queue can claim (send_task() admits a core whenever EITHER slot is free). Scanning
     * before send_task() would let an early plant win that race even when a real, immediately-
     * runnable ready_queue task of the same type was waiting for the very same slot - a real cost
     * (the ready_queue task sits idle for nothing) traded for a speculative gain (the planted
     * successor still has to wait on its predecessor regardless). Running the scan last means
     * early dispatch only ever claims what genuine demand did not want this round. This also means
     * a predecessor freshly placed by send_task() onto a fully-idle core is already sitting there
     * as a plain "exactly one busy slot" core by the time this runs, so plant_pass() catches the
     * co-dispatch case on its own - no separate placed-together code path is needed. */
    plant_pass(tid);
#endif
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
