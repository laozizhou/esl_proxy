/*
 * dispatch.c - Dispatch Worker Thread Implementation
 *
 * Worker thread entry point for Dispatch.
 * This file is compiled separately as it contains pthread-specific code.
 */

#include "dispatch.h"
#include "early_dispatch.h"
#include "lat_trace.h"
#include "log.h"
#include "ring_buf.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

#if ED_ENABLE
#define ED_DRAIN_MAX_PER_ROUND 16
#endif

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
        lat_trace_setmix(vector, cube & vector);
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
 *   会让本地 avail 位图前进（idx 已被消耗），下一次 ctzll 拿到错位；同时也会
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
    uint64_t free_s0 = atomic_load_explicit(&ctrl->free_bitmap[type][0],
                                            memory_order_acquire);
    uint64_t free_s1 = atomic_load_explicit(&ctrl->free_bitmap[type][1],
                                            memory_order_acquire);
    /*
     * PING-PONG 语义：executor 每核同时只跑一个 slot，另一个 slot 用作预装载位，
     * 所以「任一 slot 空闲」的核都可以派发，不必等两个 slot 全空。
     */
    /*
     * 本 type 下两个 slot 都空闲的核：优先派这里，走 slot0。
     * 口径是 type 粒度（free_bitmap 第一维即 type），不是「整个物理核空闲」。
     */
    uint64_t avail_both = free_s0 & free_s1;
    /*
     * 半空核：另一个 slot 已被占（通常是 ED staged 的任务）。这些核在旧的 AND
     * 语义下整核不可用，是 ED 开启后 normal 路径饥饿的根源。把它们排在全空核
     * 之后使用，既消除饥饿，又不破坏「先把任务铺到不同核」的负载均衡——过早把
     * 任务塞进忙核的第二个 slot 会把它绑死在该核上，别的核空出来也无法接手。
     * 每核每轮只接一个任务，故名额上限仍是 AIC_CNT。
     */
    uint64_t avail_half = (free_s0 | free_s1) & ~avail_both;
    uint16_t cnt = (uint16_t)__builtin_popcountll(free_s0 | free_s1);
    lat_trace_send_call(type, cnt, free_s0, free_s1);
    if (cnt <= 0) {
#if LAT_TRACE
        /* 只在无核可发这条冷分支上取队列长度，热路径不受影响 */
        lock_q(&ctrl->ready_queue[type]);
        uint64_t waiters = ctrl->ready_queue[type].cnt;
        unlock_q(&ctrl->ready_queue[type]);
        lat_trace_send_starve(type, waiters);
#endif
        WORKER_LOGF("send,free_cnt,%d", cnt);
        return 0;
    }
    uint16_t task_ids[AIC_CNT];
    if (!batch_dequeue(&ctrl->ready_queue[type], task_ids, &cnt)){
        return 0;
    }
    for (uint16_t i = 0; i < cnt; i++) {
        lat_trace_deq(task_ids[i]);
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

        /*
         * skip 判定过后再挑核，避免 skip 的 task 白占 free_bitmap 位。
         * cnt 按可用核数核算、skip 又不消耗名额，故单 dispatcher 下必有余量。
         */
        assert((avail_both | avail_half) != 0);
        uint64_t idx;
        uint64_t mask;
        int slot;
        if (avail_both != 0) {
            idx = (uint64_t)__builtin_ctzll(avail_both);
            mask = (uint64_t)0x1 << idx;
            /* 全空核走 slot0，与 executor 挑选 active slot 的顺序一致 */
            slot = 0;
            avail_both &= ~mask;
        } else {
            idx = (uint64_t)__builtin_ctzll(avail_half);
            mask = (uint64_t)0x1 << idx;
            /* 半空核只剩一个 slot 可用；free_s0/free_s1 是本轮快照，不会被自己改动 */
            slot = (free_s0 & mask) != 0 ? 0 : 1;
            avail_half &= ~mask;
        }

        uint64_t old_free = atomic_fetch_and_explicit(
            &ctrl->free_bitmap[type][slot], ~mask, memory_order_acq_rel);
        assert((old_free & mask) != 0);

        // Set executor's tasks and duration
        int core = (int)idx;
        assert(atomic_load_explicit(&g_executors[exe_type][core].slot_state[slot],
                                    memory_order_acquire) == EXE_SLOT_EMPTY);
        g_executors[exe_type][core].tasks[slot] = task_id;
        g_executors[exe_type][core].block_idx[slot] = 0;
        // Scale down duration for faster simulation (configurable via EXEC_DURATION_SCALE)
        uint32_t raw_duration = g_basic_buf[task_id & RING_MASK].duration;
        g_executors[exe_type][core].duration[slot] = SCALE_EXEC_DURATION(raw_duration);
        
        if (slot == 1) {
            ctrl->task_id_map2[type][idx] = task_id;
        } else {
            ctrl->task_id_map1[type][idx] = task_id;
        }

#if ED_ENABLE
        /*
         * Step 4：record 必须在 RUNNABLE 发布前写；
         * 否则 Hook0/选核可能读到未初始化位置。
         */
        ed_task_dispatch_record_store(task_id, core, slot, type);
#endif

        /*
         * Step 2 normal 发布链：
         * payload + task_id_map 写完后，release 发布 RUNNABLE。
         * executor 只有 acquire 读到 RUNNABLE 才允许读取 payload。
         */
        atomic_store_explicit(&g_executors[exe_type][core].slot_state[slot],
                              EXE_SLOT_RUNNABLE, memory_order_release);
        /* KPI 终点（正常派发路径）；ED 放行路径在 ed_notify_once 内打点 */
        ed_lat_mark_runnable(task_id, ED_LAT_NORMAL);
        lat_trace_run(task_id, LAT_TRACE_PATH_NORMAL);
        WORKER_LOGF("send,task_id,%u,core,%d,slot,%d,type,%d", task_id, core, slot, type);
        sent++;
#if ED_ENABLE
        /* Step 4 Hook 0：成功发布后沿边传播 dispatch_fanin。 */
        propagate_dispatch_fanin(task_id);
#endif
    }
    return sent;
}

#if ED_ENABLE
static inline uint64_t queue_count_snapshot(queue_t *q)
{
    uint64_t cnt;
    lock_q(q);
    cnt = q->cnt;
    unlock_q(q);
    return cnt;
}

/*
 * 只看 normal ready_queue，不含 ED 队列。
 * 用途：ED 是投机派发——staged 任务的前驱可能还没跑完，占住 slot 也不能立刻执行；
 * 而 ready_queue 里的任务前驱已全部完成、拿到 slot 就能跑。后者必须优先用槽位，
 * 所以只有本函数返回 false（normal 侧确实排空、槽位富余）时才允许做 ED。
 */
static inline bool has_pending_normal_work(int tid)
{
    if (queue_count_snapshot(&g_ctrl_t[tid].ready_queue[TASK_TYPE_CUBE]) > 0) {
        return true;
    }
    if (queue_count_snapshot(&g_ctrl_t[tid].ready_queue[TASK_TYPE_VECTOR]) > 0) {
        return true;
    }
    if (queue_count_snapshot(&g_ctrl_t[tid].ready_queue[TASK_TYPE_MIX]) > 0) {
        return true;
    }
    return false;
}

static inline bool has_pending_ready_work(int tid)
{
    if (has_pending_normal_work(tid)) {
        return true;
    }
    if (queue_count_snapshot(&g_ed_ready_queue) > 0) {
        return true;
    }
    return false;
}
#endif

int dispatch(int tid)
{
    int total_sent = 0;
    lat_trace_dispatch_round();
    /* Step 2 固定顺序：drain -> set_mix -> send_task x3 */
    drain_completed_snapshot(tid);
    set_mix(tid);
    total_sent += send_task(&g_ctrl_t[tid], TASK_TYPE_MIX);
    total_sent += send_task(&g_ctrl_t[tid], TASK_TYPE_VECTOR);
    total_sent += send_task(&g_ctrl_t[tid], TASK_TYPE_CUBE);
#if ED_ENABLE
    /*
     * 已 ready 的任务优先：只有 normal 队列彻底排空才动 ED。
     * 队列非空时说明槽位仍供不应求，此时投机占位会挤掉马上就能跑的任务；
     * 顺带也省掉了这一轮的 dequeue / pick_stage_core / re-push 开销。
     */
    if (!has_pending_normal_work(tid)) {
        for (int k = 0; k < ED_DRAIN_MAX_PER_ROUND; k++) {
            if (try_early_dispatch(tid) == 0) {
                break;
            }
            total_sent++;
        }
    }
#endif
    return total_sent;
}

/*
 * Dispatch worker thread entry point
 * Runs the dispatch loop for task distribution
 */
void *dispatch_worker(void *arg)
{
    int tid = (int)(intptr_t)arg;
    /*
     * ===== 本函数末尾三个 [scheduler] 指标的口径 =====
     *
     * 计时窗口：本行 -> 全部任务完成（g_completed_cnt >= g_task_id）。
     * 这是一段墙上时钟（wall clock，即真实流逝时间），窗口内编排线程、
     * cutter 线程、executor 线程都在并发跑，所以它是端到端完工时间
     * （makespan），不是调度代码自身消耗的 CPU 时间。
     *
     * 窗口内混合了四部分耗时，本指标无法把它们分离：
     *   1) 编排线程建图（new_task / add_predecessors）
     *   2) cutter 解依赖（deal_completed_queue -> resolve_dep）
     *   3) dispatcher 派发（drain / send_task / try_early_dispatch）
     *   4) executor 模拟执行：单线程轮询所有槽位，对 RUNNABLE 槽位每轮把
     *      duration 减 1，减到 0 才算完成
     *
     * 主要失真来自第 4 项：它的循环轮数与 SCALE_EXEC_DURATION 的结果成正比，
     * 也就是与 EXEC_DURATION_SCALE 成反比。scale 越小，模拟执行占比越高，
     * 调度侧改动带来的差异会被淹没。因此比较两个调度版本时必须固定 scale，
     * 且保证 executor 内层循环代码路径一致，否则数字不可比。
     *
     * ===== 三个输出的分子/分母 =====
     *   task_cnt = g_completed_cnt              窗口内完成的任务数
     *   duration = end_ns - start_ns            上述墙上时钟窗口，单位 ns
     *   task_tp  = g_completed_cnt * 1000.0 / duration_ns
     *              分子：完成任务数 x 1000；分母：窗口纳秒数
     *              => 单位 MTasks/s，即"每微秒完成多少个任务"
     *              它是系统完工速率，不是"调度器每秒能派发多少任务"
     */
    uint64_t start_ns = get_time_ns();
    
    while (!atomic_load(&g_orch_is_done)) {
        dispatch(tid);
    }
    
    while (atomic_load(&g_completed_cnt) < atomic_load(&g_task_id)) {
        dispatch(tid);
    }

    uint64_t end_ns = get_time_ns();

#if ED_ENABLE
    /*
     * completion 达标后再冲洗一次 ready/ed queue，补齐 send_skip 统计；
     * 不计入 scheduler elapsed，避免把统计收口开销混进性能口径。
     */
    for (int i = 0; i < RING_SIZE; i++) {
        if (!has_pending_ready_work(tid)) {
            break;
        }
        dispatch(tid);
    }
#endif

    atomic_store(&g_is_done, true);
    uint64_t elapsed_ns = end_ns - start_ns;

    /* 口径见函数开头说明：duration/task_tp 是含模拟执行时间的 makespan 派生值 */
    MAIN_LOGF("[scheduler] task_cnt = %u", g_completed_cnt);
    MAIN_LOGF("[scheduler] duration = %llu ns", (unsigned long long)elapsed_ns);
    MAIN_LOGF("[scheduler] task_tp = %f MTasks/s",(float)(g_completed_cnt * 1000.0 / elapsed_ns));
    return NULL;
}
