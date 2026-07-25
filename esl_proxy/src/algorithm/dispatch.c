/*
 * dispatch.c - Dispatch Worker Thread Implementation
 *
 * Worker thread entry point for Dispatch.
 * This file is compiled separately as it contains pthread-specific code.
 */

#include "dispatch.h"
#include "early_dispatch.h"
#include "log.h"
#include "ring_buf.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

extern atomic_int g_task_id;
extern atomic_bool g_orch_is_done;
extern atomic_int g_completed_cnt;
extern atomic_bool g_is_done;
extern ctrl_t g_ctrl_t[DISPATCH_THREAD_CNT];
extern struct task_desc g_basic_buf[RING_SIZE];
extern executor_t g_executors[EXE_TYPE_CNT][AIC_CNT];

void init_ctrl_t(void)
{
    for (int tid = 0; tid < DISPATCH_THREAD_CNT; tid++) {
        g_ctrl_t[tid].tid = (uint16_t)tid;

        for (int i = 0; i < TASK_TYPE_CNT; i++) {
            for (int j = 0; j < AIC_OSTD; j++) {
                atomic_store_explicit(&g_ctrl_t[tid].free_bitmap[i][j],
                                      (uint64_t)((1ULL << AIC_CNT) - 1),
                                      memory_order_relaxed);
            }
        }

        for (int i = 0; i < EXE_TYPE_CNT; i++) {
            for (int j = 0; j < AIC_OSTD; j++) {
                atomic_store_explicit(&g_ctrl_t[tid].msg_bitmap[i][j], 0,
                                      memory_order_relaxed);
            }
        }

        for (int i = 0; i < EXE_TYPE_CNT; i++) {
            for (int j = 0; j < AIC_CNT; j++) {
                g_ctrl_t[tid].task_id_map1[i][j] = 0;
                g_ctrl_t[tid].task_id_map2[i][j] = 0;
            }
        }

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

static inline void set_mix(int tid)
{
    /*
     * Step 2：每轮都基于当前 free 集合重算 MIX 可用核。
     * 这里是 MIX 语义的唯一写入点，禁止和 drain/send_task 交叉写 msg_bitmap。
     */
    for (int j = 0; j < AIC_OSTD; j++) {
        uint64_t cube = atomic_load_explicit(
            &g_ctrl_t[tid].free_bitmap[TASK_TYPE_CUBE][j], memory_order_acquire);
        uint64_t vector = atomic_load_explicit(
            &g_ctrl_t[tid].free_bitmap[TASK_TYPE_VECTOR][j], memory_order_acquire);
        atomic_store_explicit(&g_ctrl_t[tid].free_bitmap[TASK_TYPE_MIX][j],
                              cube & vector, memory_order_release);
    }
}

/*
 * 从同一 done 快照里完成三件事：
 * 1) task_id_map 映射 completed queue
 * 2) 恢复 free bit
 * 3) 递增完成计数
 *
 * 核心约束：每个 (exe_type, slot) 只允许一次 exchange 消费 msg_bitmap。
 */
static inline void drain_completed_snapshot(int tid)
{
    uint16_t task_id[EXE_TYPE_CNT * AIC_OSTD * AIC_CNT];
    uint16_t complete_cnt = 0;

    for (int i = 0; i < EXE_TYPE_CNT; i++) {
        for (int j = 0; j < AIC_OSTD; j++) {
            uint64_t done = atomic_exchange_explicit(&g_ctrl_t[tid].msg_bitmap[i][j],
                                                     0, memory_order_acquire);
            uint64_t bits = done;
            while (bits) {
                uint64_t idx = (uint64_t)__builtin_ctzll(bits);
                /*
                 * EMPTY -> msg 发布顺序由 executor 保证。dispatcher 在消费 done
                 * 快照前读取 slot_state，防止恢复 free 位时仍有 RUNNABLE 任务。
                 */
                assert(atomic_load_explicit(&g_executors[i][idx].slot_state[j],
                                            memory_order_acquire) == EXE_SLOT_EMPTY);
                task_id[complete_cnt] = (j == 0) ? g_ctrl_t[tid].task_id_map1[i][idx]
                                                 : g_ctrl_t[tid].task_id_map2[i][idx];
                WORKER_LOGF("completed,complete_cnt,%u,task_id,%u,core,%llu,bitmap,%llu",
                            complete_cnt,
                            task_id[complete_cnt],
                            (unsigned long long)idx,
                            (unsigned long long)done);
                complete_cnt++;
                bits &= (bits - 1);
            }
            /*
             * 用完同一快照后再释放 free 位，保证 task_id_map 不会在读取前被新任务覆写。
             */
            atomic_fetch_or_explicit(&g_ctrl_t[tid].free_bitmap[i][j], done,
                                     memory_order_release);
        }
    }
    if (complete_cnt > 0) {
        batch_enqueue(&g_ctrl_t[tid].completed_queue, task_id, complete_cnt);
    }
    atomic_fetch_add_explicit(&g_completed_cnt, complete_cnt, memory_order_relaxed);
}

// TODO: Work Stealing
/*
 * send_task：从 ready_queue 弹出可派发任务，把它们发布到 executor slot。
 *
 * Step 3 关键增量（§3.1 B1 / §6 Step 3）：
 *   normal dispatch 与 executor 都以「一个 slot 条目执行整任务」为语义，因此
 *   normal 路径必须一次性把 g_next_block_idx 从 0 强 CAS 到 count（整任务原子认领），
 *   不允许在 count>1 时逐块 +1。CAS 失败即代表该 task 已被 Hook 1 stager 抢占，
 *   本次 send_task 必须直接 skip：
 *     - g_ed_send_skip_cnt 自增
 *     - continue 不消耗 free_bitmap 位、不写 payload / task_id_map / slot_state
 *
 *   顺序约束：CAS 必须发生在计算 core idx、抢/清 free_bitmap 位之前，否则 skip 分支
 *   会让本地 free_bitmap 前进（idx 已被消耗），下一次 ctzll 拿到错位；同时也会
 *   泄漏一个 free bit（清了 ctrl->free_bitmap 但没写 slot）。
 *
 *   一期约束：count==1 与 count>1 都走 0->count（不是 0->1）；Hook 1 stager 只
 *   会接 count==1 的场景，两侧 CAS 目标一致，只有一个胜者。
 *
 *   count==0 兜底：若 cutter 误把 phantom task（尚未 new_task 的 slot）放进
 *   ready_queue，send_task 必须直接 skip，不能参与 CAS，也不能占 slot。
 */
static inline int send_task(ctrl_t *ctrl, int type)
{
    int exe_type = type;
    uint64_t free_bitmap = atomic_load_explicit(&ctrl->free_bitmap[type][0],
                                                memory_order_acquire) &
                           atomic_load_explicit(&ctrl->free_bitmap[type][1],
                                                memory_order_acquire);
    uint16_t cnt = (uint16_t)__builtin_popcountll(free_bitmap);
    if (cnt <= 0) {
        WORKER_LOGF("send,free_cnt,%d", cnt);
        return 0;
    }
    uint16_t task_ids[AIC_CNT];
    if (!batch_dequeue(&ctrl->ready_queue[type], task_ids, &cnt)){
        return 0;
    }
    
    int sent = 0;
    for (uint16_t i = 0; i < cnt; i++) {
        uint16_t task_id = task_ids[i];

#if ED_ENABLE
        /*
         * Step 3：CAS(g_next_block_idx: 0 -> count) 整任务原子认领。
         * 必须在计算 core idx / 抢 free bit 之前，避免 skip 时白占 slot。
         */
        uint16_t s_idx = task_id & RING_MASK;
        uint16_t count = g_basic_buf[s_idx].count;
        if (count == 0) {
            /* R18：count==0 视为 phantom，直接丢弃，不允许 baseline 派发。 */
            atomic_fetch_add_explicit(&g_ed_send_skip_cnt, 1, memory_order_relaxed);
            WORKER_LOGF("send_skip_zero_count,task_id,%u", task_id);
            continue;
        }
        uint16_t expected = 0;
        if (!atomic_compare_exchange_strong_explicit(
                &g_next_block_idx[s_idx], &expected, count,
                memory_order_acq_rel, memory_order_acquire)) {
            /* 已被 Hook 1 stager（或另一个 dispatcher）抢占：skip，不消耗 slot */
            atomic_fetch_add_explicit(&g_ed_send_skip_cnt, 1,
                                      memory_order_relaxed);
            WORKER_LOGF("send_skip,task_id,%u,nbi_seen,%u,count,%u",
                        task_id, expected, count);
            continue;
        }
#endif

        /* skip 判定过后再计算 idx，避免 skip 的 task 白占 free_bitmap 位。 */
        uint64_t idx = (uint64_t)__builtin_ctzll(free_bitmap);

        uint64_t mask = (uint64_t)0x1 << idx;
        uint64_t free_slot0 = atomic_load_explicit(&ctrl->free_bitmap[type][0],
                                                   memory_order_acquire);
        int slot = (free_slot0 & mask) != 0 ? 0 : 1;

        uint64_t old_free = atomic_fetch_and_explicit(
            &ctrl->free_bitmap[type][slot], ~mask, memory_order_acq_rel);
        assert((old_free & mask) != 0);

        // Set executor's tasks and duration
        int core = (int)idx;
        assert(atomic_load_explicit(&g_executors[exe_type][core].slot_state[slot],
                                    memory_order_acquire) == EXE_SLOT_EMPTY);
        g_executors[exe_type][core].tasks[slot] = task_id;
        g_executors[exe_type][core].block_idx[slot] = 0;
        // Scale down duration for faster simulation (divide by 10000 to handle large durations)
        uint32_t raw_duration = g_basic_buf[task_id & RING_MASK].duration;
        g_executors[exe_type][core].duration[slot] =
            (raw_duration > 10000) ? (uint16_t)(raw_duration / 10000) : 1;
        g_executors[exe_type][core].idx = slot;  // Point to the slot with the new task
        
        if (slot == 1) {
            ctrl->task_id_map2[type][idx] = task_id;
        } else {
            ctrl->task_id_map1[type][idx] = task_id;
        }

        /*
         * Step 2 normal 发布链：
         * payload + task_id_map 写完后，release 发布 RUNNABLE。
         * executor 只有 acquire 读到 RUNNABLE 才允许读取 payload。
         */
        atomic_store_explicit(&g_executors[exe_type][core].slot_state[slot],
                              EXE_SLOT_RUNNABLE, memory_order_release);
        WORKER_LOGF("send,task_id,%u,core,%d,slot,%d,type,%d", task_id, core, slot, type);
        sent++;
        free_bitmap &= ~mask;
    }
    return sent;
}

int dispatch(int tid)
{
    int total_sent = 0;
    /* Step 2 固定顺序：drain -> set_mix -> send_task x3 */
    drain_completed_snapshot(tid);
    set_mix(tid);
    total_sent += send_task(&g_ctrl_t[tid], TASK_TYPE_MIX);
    total_sent += send_task(&g_ctrl_t[tid], TASK_TYPE_VECTOR);
    total_sent += send_task(&g_ctrl_t[tid], TASK_TYPE_CUBE);
    return total_sent;
}

/*
 * Dispatch worker thread entry point
 * Runs the dispatch loop for task distribution
 */
void *dispatch_worker(void *arg)
{
    int tid = (int)(intptr_t)arg;
    uint64_t start_ns = get_time_ns();
    
    while (!atomic_load(&g_orch_is_done)) {
        dispatch(tid);
    }
    
    while (atomic_load(&g_completed_cnt) < atomic_load(&g_task_id)) {
        dispatch(tid);
    }
    
    atomic_store(&g_is_done, true);
    uint64_t end_ns = get_time_ns();
    uint64_t elapsed_ns = end_ns - start_ns;

    MAIN_LOGF("[scheduler] task_cnt = %u", g_completed_cnt);
    MAIN_LOGF("[scheduler] duration = %llu ns", (unsigned long long)elapsed_ns);
    MAIN_LOGF("[scheduler] task_tp = %f MTasks/s",(float)(g_completed_cnt * 1000.0 / elapsed_ns));
    return NULL;
}
