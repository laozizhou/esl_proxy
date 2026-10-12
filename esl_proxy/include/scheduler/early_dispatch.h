/*
 * early_dispatch.h - shared state for early dispatch (see doc/early-dispatch-case-b.md)
 *
 * Split of ownership, which is what keeps this lock-free:
 *
 *   g_pred_xor[]          maintained by painter alongside g_predecessor_cnt[]
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

/* XOR of the task's not-yet-completed predecessors. Maintained strictly alongside
 * g_predecessor_cnt[]: both are seeded in add_successors() over the same predecessors, and both
 * are updated on the same line of resolve_dep(). When the count reaches 1 the accumulator holds
 * the one survivor, because every other predecessor has been XORed in once and out once.
 *
 * This replaces two CSR predecessor lists (same-type and cross-type) plus the scan that walked
 * them looking for an unfinished entry. The type split those lists encoded is now a single
 * comparison at publish time, g_early_type[p] vs g_early_type[s].
 *
 * It inherits exactly the ownership rules of g_predecessor_cnt[]: every slot is single-writer.
 * add_successors(tid) only commits tasks tid owns, and resolve_dep(tid) only walks successors that
 * add_successors(tid) itself recorded, so the painter that owns S is the only one that ever
 * touches g_pred_xor[S]. That, not the fact that the two arrays are updated on adjacent lines, is
 * what makes a plain non-atomic uint32_t sound here. See invariant 6 in the ownership note above
 * before letting a second dispatch thread resolve edges into the same slot.
 *
 * The failure modes are not symmetric. A lost decrement shows up loudly as a count that never
 * reaches zero. A lost or doubled XOR yields an unrelated but in-range task id, publishes a hint
 * naming a non-predecessor, and surfaces only as an ordering-oracle failure much later. */
extern uint32_t g_pred_xor[RING_SIZE];

/* Keyed by the PREDECESSOR: g_early_hint[P] == S means "S is waiting only on P". Keying by P is
 * what lets dispatch consume the hint at a point where it already holds (type, core, slot), so
 * the sibling slot is slot ^ 1 and no reverse task_id -> location map is needed. */
extern uint32_t g_early_hint[RING_SIZE];

#ifdef EARLY_DISPATCH_CROSS_TYPE
/* Keyed by the PREDECESSOR, same convention as g_early_hint: g_cross_hint[P] == S means "S is
 * waiting only on P, and P is a different type from S". early_publish_hint() routes into this
 * table rather than g_early_hint when the survivor turns out to be the other type. */
extern uint32_t g_cross_hint[RING_SIZE];
#endif

/* Task type by global id, needed to route a published hint into the same-type or cross-type
 * table once the accumulator has identified the surviving predecessor. */
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
extern uint32_t g_early_plants;
extern uint32_t g_early_skipped_dup;
#ifdef EARLY_DISPATCH_CROSS_TYPE
extern uint32_t g_cross_hints_published;
extern uint32_t g_cross_plants;
#endif

/* Built by painter (it owns test_graph); call once before the worker threads start. */
void early_dispatch_init(void);

/* Called from main() after the joins. Returns 0 if every check passed, non-zero otherwise. */
int early_dispatch_report(void);

#endif /* SCHEDULER_EARLY_DISPATCH_H */
