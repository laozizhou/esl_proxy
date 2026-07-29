/*
 * early_dispatch.h - shared state for early dispatch (see doc/early-dispatch-case-b.md)
 *
 * Split of ownership, which is what keeps this lock-free:
 *
 *   g_early_st_pred[]     built once at init, read-only afterwards
 *   g_early_hint[]        published by painter, consumed (and cleared) by dispatch
 *   g_early_dispatched[]  written and read only by dispatch, at its single send point
 *
 * g_early_hint is the only cross-thread field. It carries a plain task id or the sentinel, so a
 * racing publish/consume can only lose or duplicate a hint - never corrupt one. Losing a hint is
 * harmless because a plant is opportunistic and ready_queue is the fallback; duplicating one is
 * caught by g_early_dispatched. Both arrays are single-writer only while every task flows through
 * one dispatch thread, which holds because send_2_ready_queue() hardcodes target_ctrl = 0. See
 * invariant 6 in the design note before making dispatch genuinely multi-die.
 */

#ifndef SCHEDULER_EARLY_DISPATCH_H
#define SCHEDULER_EARLY_DISPATCH_H

#include <stdint.h>

#include "scheduler/conf.h"

#define EARLY_NONE 0xFFFFFFFFu

/* Unique same-type predecessor of a task, or EARLY_NONE when it does not have exactly one.
 * A static property of the graph: computed once by early_dispatch_init(). */
extern uint32_t g_early_st_pred[RING_SIZE];

/* Keyed by the PREDECESSOR: g_early_hint[P] == S means "S is waiting only on P". Keying by P is
 * what lets dispatch consume the hint at a point where it already holds (type, core, slot), so
 * the sibling slot is slot ^ 1 and no reverse task_id -> location map is needed. */
extern uint32_t g_early_hint[RING_SIZE];

/* Task type by global id, needed to apply the same-type rule while building g_early_st_pred. */
extern uint8_t g_early_type[RING_SIZE];

/* Set at dispatch's single send point, covering both the early and the normal path. This is the
 * only thing preventing a planted task from being dispatched a second time when it later becomes
 * normally ready, which would double-decrement its successors' indegree and release them while a
 * real predecessor is still executing. */
extern uint8_t g_early_dispatched[RING_SIZE];

/* Counters. Reported by early_dispatch_report() with plain printf, deliberately independent of
 * WORKER_LOGF: that is silenced by SCHEDULER_LOG=0, which is the documented performance mode, so
 * otherwise "the feature fired" and "here is the timing" could never come from the same run. */
extern uint32_t g_early_hints_published;
extern uint32_t g_early_plants_b;
extern uint32_t g_early_plants_a;
extern uint32_t g_early_skipped_dup;

/* Built by painter (it owns test_graph); call once before the worker threads start. */
void early_dispatch_init(void);

/* Called from main() after the joins. Returns 0 if every check passed, non-zero otherwise. */
int early_dispatch_report(void);

#endif /* SCHEDULER_EARLY_DISPATCH_H */
