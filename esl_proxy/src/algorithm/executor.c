/*
 * executor.c - Executor Implementation
 *
 * Provides task execution utilities including delay functionality.
 *
 * C11 standard with _Atomic for lock-free concurrency.
 */

#define _POSIX_C_SOURCE 199309L

#include "conf.h"
#include <time.h>
#include "log.h"
#include "executor.h"
#include "dispatch.h"
#include "early_dispatch.h"
#include "ring_buf.h"

#ifndef ED_A11_PROBE
#define ED_A11_PROBE 0
#endif

#ifndef ED_A11_DUMP_PERIOD
#define ED_A11_DUMP_PERIOD 500
#endif

extern _Atomic bool g_is_done;
extern ctrl_t g_ctrl_t[DISPATCH_THREAD_CNT];
extern executor_t g_executors[EXE_TYPE_CNT][AIC_CNT];

void executor_init(void)
{
    for (int exe_type = 0; exe_type < EXE_TYPE_CNT; exe_type++) {
        for (int core = 0; core < AIC_CNT; core++) {
            g_executors[exe_type][core].idx = AIC_OSTD;
            for (int i = 0; i < AIC_OSTD; i++) {
                g_executors[exe_type][core].tasks[i] = 0;
                g_executors[exe_type][core].block_idx[i] = 0;
                g_executors[exe_type][core].duration[i] = 0;
                g_executors[exe_type][core].base[i] = 0;
                atomic_init(&g_executors[exe_type][core].slot_state[i],
                            EXE_SLOT_EMPTY);
#if ED_ENABLE
                atomic_init(&g_executors[exe_type][core].doorbell[i], 0);
#endif
            }
        }
    }
}

/*
 * slot 完成的唯一收口：
 * 1) 先 release 置 EMPTY，让 dispatcher 可安全回收 free bit。
 * 2) 再 release 置 msg_bitmap done bit，触发 dispatcher drain。
 */
static inline void complete_slot(int type, int core, int slot, uint16_t task_id_done)
{
    executor_t *e = &g_executors[type][core];
    e->block_idx[slot] = 0;
    e->idx = AIC_OSTD;
#if ED_ENABLE
    /* Step 2：仅做 tag 校验后清记录，不接 Hook。 */
    ed_task_dispatch_record_clear(task_id_done);
    uint32_t live_tag = atomic_load_explicit(
        &g_ring_task_tag[task_id_done & RING_MASK], memory_order_acquire);
    WORKER_LOGF("slot_free, task=%u, tag=%u", task_id_done, live_tag);
#else
    (void)task_id_done;
#endif
    atomic_store_explicit(&e->slot_state[slot], EXE_SLOT_EMPTY,
                          memory_order_release);
    atomic_fetch_or_explicit(&g_ctrl_t[core % DISPATCH_THREAD_CNT].msg_bitmap[type][slot],
                             ((uint64_t)0x1 << core), memory_order_release);
}

void* executor_worker(void *arg)
{
    (void)arg;
    int total_write_cnt = 0;
#if ED_ENABLE && ED_A11_PROBE
    uint64_t iterations = 0;
#endif
    while (!atomic_load(&g_is_done))
    {
#if ED_ENABLE && ED_A11_PROBE
        iterations++;
        if ((iterations % ED_A11_DUMP_PERIOD) == 0) {
            for (int t = 0; t < EXE_TYPE_CNT; t++) {
                for (int c = 0; c < AIC_CNT; c++) {
                    for (int sl = 0; sl < AIC_OSTD; sl++) {
                        uint16_t task = g_executors[t][c].tasks[sl];
                        uint16_t s_idx = task & RING_MASK;
                        WORKER_LOGF("slot_state_dump, s=%u, unfin=%u, spec=%u, state=%u, doorbell=%u",
                                    task,
                                    (unsigned)atomic_load_explicit(&g_unfin_pred_cnt[s_idx], memory_order_relaxed),
                                    (unsigned)atomic_load_explicit(&g_spec_state[s_idx], memory_order_relaxed),
                                    (unsigned)atomic_load_explicit(&g_executors[t][c].slot_state[sl], memory_order_relaxed),
                                    (unsigned)atomic_load_explicit(&g_executors[t][c].doorbell[sl], memory_order_relaxed));
                    }
                }
            }
        }
#endif
        for (int exe_type = 0; exe_type < EXE_TYPE_CNT; exe_type++) {
            for (int core = 0; core < AIC_CNT; core++) {
                executor_t *e = &g_executors[exe_type][core];
                for (int slot = 0; slot < AIC_OSTD; slot++) {
                    uint8_t state = atomic_load_explicit(&e->slot_state[slot],
                                                         memory_order_acquire);
                    /* EMPTY/GATED 都不执行；只有 RUNNABLE 才 tick。 */
                    if (state != EXE_SLOT_RUNNABLE) {
                        continue;
                    }

#if ED_ENABLE
                    /* Step 2: doorbell 仅做硬件信号占位，清零不影响 gate 判据。 */
                    atomic_store_explicit(&e->doorbell[slot], 0, memory_order_relaxed);
#endif

                    uint16_t task_id = e->tasks[slot];
                    uint32_t block_count = g_basic_buf[task_id & RING_MASK].count;

                    if (block_count > 1) {
                        /*
                         * 保留 SPMD 语义：单次 dispatch 的条目要执行完所有 block，
                         * 不能在首块结束就直接发布 completed。
                         */
                        if (e->duration[slot] > 0) {
                            e->duration[slot]--;
                            continue;
                        }
                        uint16_t next_block = ++e->block_idx[slot];
                        if (next_block < block_count) {
                            uint32_t raw_duration = g_basic_buf[task_id & RING_MASK].duration;
                            e->duration[slot] = SCALE_EXEC_DURATION(raw_duration);
                            continue;
                        }
                        total_write_cnt++;
                        WORKER_LOGF("total,%d,core,%d,type,%d,blocks,%u",
                                    total_write_cnt, core, exe_type, block_count);
                        complete_slot(exe_type, core, slot, task_id);
                    } else {
                        if (e->duration[slot] > 0) {
                            e->duration[slot]--;
                        }
                        if (e->duration[slot] == 0) {
                            total_write_cnt++;
                            WORKER_LOGF("total,%d,core,%d,type,%d", total_write_cnt, core, exe_type);
                            complete_slot(exe_type, core, slot, task_id);
                        }
                    }
                }
            }
        }
    }
    WORKER_LOGF("finished, total_write_cnt=%d", total_write_cnt);
    return NULL;
}
