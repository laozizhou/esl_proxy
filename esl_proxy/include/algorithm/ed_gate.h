/*
 * ed_gate.h - ED 门铃开闸（GATED -> RUNNABLE）
 *
 * 单独成一个头文件的原因：
 * 1) 该逻辑同时需要 executor.h（g_executors）与 early_dispatch.h（门铃计数、
 *    延迟 KPI），而依赖链是 early_dispatch.h -> queue.h -> executor.h，
 *    executor.h 不能反向包含 early_dispatch.h，否则成环。
 * 2) 保持 static inline 以便 executor 热路径内联；同时测试可直接调用，
 *    不必启动整套运行时。
 */

#ifndef ALGORITHM_ED_GATE_H
#define ALGORITHM_ED_GATE_H

#include <stdbool.h>
#include <stdatomic.h>

#include "conf.h"
#include "early_dispatch.h"
#include "executor.h"
#include "log.h"

#if ED_ENABLE

/*
 * 轮询门铃：仅应在槽位「没有正在执行的任务」时调用，
 * 执行热路径不访问门铃，避免每 tick 写脏与 slot_state 同一缓存行。
 *
 * 返回 true 表示本次调用刚把槽位由 GATED 开闸为 RUNNABLE，调用方可立即开跑。
 *
 * 门铃必须用 acquire 读，不能降级为 relaxed：
 * stager 以 release 发布 GATED、notify 以 release 写门铃，两者构成同步链，
 * 因此一旦读到门铃为 1，紧接着读 slot_state 必然可见 GATED。若用 relaxed，
 * 可能出现「看到门铃却仍看到 EMPTY」，把刚送到的通知误判成残留而清掉；
 * 而 notify_claimed 只允许敲一次门铃，该任务就会永久卡在 GATED。
 */
static inline bool ed_poll_doorbell(int type, int core, int slot)
{
    executor_t *e = &g_executors[type][core];
    if (atomic_load_explicit(&e->doorbell[slot], memory_order_acquire) == 0) {
        return false;
    }

    uint8_t expected = EXE_SLOT_GATED;
    if (!atomic_compare_exchange_strong_explicit(
            &e->slot_state[slot], &expected, EXE_SLOT_RUNNABLE,
            memory_order_acq_rel, memory_order_acquire)) {
        /*
         * 槽位不在门禁态：属于换代残留通知（例如目标槽位已被回收）。
         * 吸收掉，避免下一代任务被这枚旧门铃提前放行。
         */
        atomic_store_explicit(&e->doorbell[slot], 0, memory_order_relaxed);
        return false;
    }

    atomic_store_explicit(&e->doorbell[slot], 0, memory_order_release);
    atomic_fetch_add_explicit(&g_ed_gate_open_cnt, 1, memory_order_relaxed);
    /* KPI 终点（ED 放行路径）：开闸即该任务转为可执行 */
    ed_lat_mark_runnable(e->tasks[slot], ED_LAT_EARLY);
    WORKER_LOGF("gate_open, task=%u, core=%d, slot=%d", e->tasks[slot], core, slot);
    return true;
}

#endif /* ED_ENABLE */

#endif /* ALGORITHM_ED_GATE_H */
