#ifndef ALGORITHM_LAT_TRACE_H
#define ALGORITHM_LAT_TRACE_H

/*
 * 逐任务 ready->runnable 延迟分解探针（诊断用，默认关闭）。
 *
 * 既有 [lat] KPI 只按路径聚合成直方图，任务身份在入账时就丢了，因此无法回答
 * “同一个任务在 ED=0 与 ED=1 两种构建下分别等了多久”。这里按 task_id 存四个
 * 时间戳，把 ready->run 拆成三段：
 *   ready->enq   cutter 攒完这一批并写入 ready_queue
 *   enq  ->deq   在 ready_queue 里等 dispatcher 取走（含 free slot 耗尽的空转）
 *   deq  ->run   出队后写 payload 并发布 RUNNABLE
 *
 * ED 放行路径的 run 由门铃开闸打点，与 deq 无因果关系——ED 任务同样会被
 * dispatcher 出队然后 CAS 失败 skip，那次 deq 可能晚于 run。
 *
 * 前提：task_id 在一次运行内不得跨代复用（当前 case task_cnt < RING_SIZE 成立），
 * 否则同一下标会被后一代覆盖。
 *
 * 开关：make LAT_TRACE=1；CSV 输出路径由环境变量 LAT_TRACE_CSV 指定。
 */

#include <stdint.h>

#include "conf.h"

#ifndef LAT_TRACE
#define LAT_TRACE 0
#endif

#if LAT_TRACE

#include <stdatomic.h>

#include "log.h"

#define LAT_TRACE_PATH_NONE   0u
#define LAT_TRACE_PATH_NORMAL 1u
#define LAT_TRACE_PATH_ED     2u

extern _Atomic uint64_t g_lt_ready_ns[RING_SIZE];
extern _Atomic uint64_t g_lt_enq_ns[RING_SIZE];
extern _Atomic uint64_t g_lt_deq_ns[RING_SIZE];
extern _Atomic uint64_t g_lt_run_ns[RING_SIZE];
extern _Atomic uint32_t g_lt_deq_cnt[RING_SIZE];
extern _Atomic uint8_t  g_lt_path[RING_SIZE];

/*
 * dispatcher 侧的槽位供给统计，用来判断 normal 任务是否在等空闲 slot。
 * 按 task type 分开：MIX 的 free_bitmap 是 CUBE 与 VECTOR 的交集，天然比
 * 另两类紧，混在一起统计会把 MIX 的结构性紧张误算成 ED 造成的饥饿。
 */
#define LAT_TRACE_TYPE_CNT 3

extern _Atomic uint64_t g_lt_dispatch_round_cnt;
extern _Atomic uint64_t g_lt_send_call_cnt[LAT_TRACE_TYPE_CNT];
extern _Atomic uint64_t g_lt_send_starve_cnt[LAT_TRACE_TYPE_CNT];
extern _Atomic uint64_t g_lt_starve_with_work_cnt[LAT_TRACE_TYPE_CNT];
extern _Atomic uint64_t g_lt_starve_waiters_sum[LAT_TRACE_TYPE_CNT];
extern _Atomic uint64_t g_lt_free_core_sum[LAT_TRACE_TYPE_CNT];
/*
 * send_task 的可派发核集合是所有 slot 的 free_bitmap[type][*] 之并：
 * PING-PONG 下每核同时只跑一个 slot，其余用作预装载位，故任一 slot 空闲的核
 * 都可派发（每核每轮只发一个任务，所以名额数就是这个并集的核数，记在
 * g_lt_free_core_sum）。
 *
 * slot0/slot1 两个计数器只看头两个 slot（AIC_OSTD 可调，>2 时其余 slot 不单独
 * 统计）。而「全部 slot 空闲」的核——send_task 优先派发的那批——必须单独累加，
 * 不能用容斥反推：|A∩B| = |A|+|B|-|A∪B| 只对恰好两个集合成立，AIC_OSTD>2 时
 * 会算出负数。
 */
extern _Atomic uint64_t g_lt_free_slot0_sum[LAT_TRACE_TYPE_CNT];
extern _Atomic uint64_t g_lt_free_slot1_sum[LAT_TRACE_TYPE_CNT];
extern _Atomic uint64_t g_lt_free_allslot_sum[LAT_TRACE_TYPE_CNT];

/*
 * set_mix 的净效果：它把 free_bitmap[MIX] 覆写为 CUBE 与 VECTOR 的交集，
 * 而 TASK_TYPE_MIX 与 TASK_TYPE_VECTOR 枚举值相同，因此这是对 VECTOR 可用核
 * 集合的原地收缩。这里统计每轮被交集削掉的核数，用来判断 ED 占住的 CUBE 槽位
 * 是否被放大成 VECTOR 的核短缺。
 */
extern _Atomic uint64_t g_lt_setmix_call_cnt;
extern _Atomic uint64_t g_lt_setmix_before_sum;
extern _Atomic uint64_t g_lt_setmix_after_sum;

void lat_trace_init(void);
void lat_trace_dump(void);

/* 依赖刚满足：本代样本从这里开始，顺带清掉上一代残留 */
static inline void lat_trace_ready(uint32_t task_id)
{
    uint16_t idx = (uint16_t)(task_id & RING_MASK);
    atomic_store_explicit(&g_lt_enq_ns[idx], 0, memory_order_relaxed);
    atomic_store_explicit(&g_lt_deq_ns[idx], 0, memory_order_relaxed);
    atomic_store_explicit(&g_lt_run_ns[idx], 0, memory_order_relaxed);
    atomic_store_explicit(&g_lt_deq_cnt[idx], 0, memory_order_relaxed);
    atomic_store_explicit(&g_lt_path[idx], LAT_TRACE_PATH_NONE, memory_order_relaxed);
    atomic_store_explicit(&g_lt_ready_ns[idx], get_time_ns_hires(), memory_order_relaxed);
}

static inline void lat_trace_enq(uint32_t task_id)
{
    uint16_t idx = (uint16_t)(task_id & RING_MASK);
    atomic_store_explicit(&g_lt_enq_ns[idx], get_time_ns_hires(), memory_order_relaxed);
}

/* 只记首次出队时刻，但出队总次数照数——skip 掉的那几次也是被消耗的名额 */
static inline void lat_trace_deq(uint32_t task_id)
{
    uint16_t idx = (uint16_t)(task_id & RING_MASK);
    atomic_fetch_add_explicit(&g_lt_deq_cnt[idx], 1, memory_order_relaxed);
    uint64_t expected = 0;
    atomic_compare_exchange_strong_explicit(&g_lt_deq_ns[idx], &expected,
                                            get_time_ns_hires(),
                                            memory_order_relaxed,
                                            memory_order_relaxed);
}

/* 槽位变 RUNNABLE：与 ed_lat_mark_runnable 同点调用，只认第一次 */
static inline void lat_trace_run(uint32_t task_id, unsigned path)
{
    uint16_t idx = (uint16_t)(task_id & RING_MASK);
    uint8_t expected = LAT_TRACE_PATH_NONE;
    if (!atomic_compare_exchange_strong_explicit(&g_lt_path[idx], &expected,
                                                 (uint8_t)path,
                                                 memory_order_relaxed,
                                                 memory_order_relaxed)) {
        return;
    }
    atomic_store_explicit(&g_lt_run_ns[idx], get_time_ns_hires(), memory_order_relaxed);
}

static inline void lat_trace_dispatch_round(void)
{
    atomic_fetch_add_explicit(&g_lt_dispatch_round_cnt, 1, memory_order_relaxed);
}

static inline void lat_trace_setmix(uint64_t before_bits, uint64_t after_bits)
{
    atomic_fetch_add_explicit(&g_lt_setmix_call_cnt, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_lt_setmix_before_sum,
                              (uint64_t)__builtin_popcountll(before_bits),
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&g_lt_setmix_after_sum,
                              (uint64_t)__builtin_popcountll(after_bits),
                              memory_order_relaxed);
}

/*
 * s0/s1 为头两个 slot 各自的空闲核位图，all 为「全部 slot 都空闲」的核位图；
 * free_cnt 是 send_task 实际可派发的核数（任一 slot 空闲的核数）。
 */
static inline void lat_trace_send_call(int type, uint32_t free_cnt,
                                       uint64_t s0, uint64_t s1, uint64_t all)
{
    atomic_fetch_add_explicit(&g_lt_send_call_cnt[type], 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_lt_free_core_sum[type], free_cnt, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_lt_free_slot0_sum[type],
                              (uint64_t)__builtin_popcountll(s0), memory_order_relaxed);
    atomic_fetch_add_explicit(&g_lt_free_slot1_sum[type],
                              (uint64_t)__builtin_popcountll(s1), memory_order_relaxed);
    atomic_fetch_add_explicit(&g_lt_free_allslot_sum[type],
                              (uint64_t)__builtin_popcountll(all), memory_order_relaxed);
}

/*
 * 无核可发，任务留在 ready_queue 里。waiters 为此刻队列里的待派发任务数：
 * waiters>0 才是真饥饿——队列空时无核可用不构成延迟。
 */
static inline void lat_trace_send_starve(int type, uint64_t waiters)
{
    atomic_fetch_add_explicit(&g_lt_send_starve_cnt[type], 1, memory_order_relaxed);
    if (waiters > 0) {
        atomic_fetch_add_explicit(&g_lt_starve_with_work_cnt[type], 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_lt_starve_waiters_sum[type], waiters, memory_order_relaxed);
    }
}

#else /* !LAT_TRACE */

#define LAT_TRACE_PATH_NORMAL 1u
#define LAT_TRACE_PATH_ED     2u

static inline void lat_trace_init(void) {}
static inline void lat_trace_dump(void) {}
static inline void lat_trace_ready(uint32_t task_id) { (void)task_id; }
static inline void lat_trace_enq(uint32_t task_id) { (void)task_id; }
static inline void lat_trace_deq(uint32_t task_id) { (void)task_id; }
static inline void lat_trace_run(uint32_t task_id, unsigned path)
{
    (void)task_id;
    (void)path;
}
static inline void lat_trace_dispatch_round(void) {}
static inline void lat_trace_setmix(uint64_t before_bits, uint64_t after_bits)
{
    (void)before_bits;
    (void)after_bits;
}
static inline void lat_trace_send_call(int type, uint32_t free_cnt,
                                       uint64_t s0, uint64_t s1, uint64_t all)
{
    (void)type;
    (void)free_cnt;
    (void)s0;
    (void)s1;
    (void)all;
}
static inline void lat_trace_send_starve(int type, uint64_t waiters)
{
    (void)type;
    (void)waiters;
}

#endif /* LAT_TRACE */

#endif /* ALGORITHM_LAT_TRACE_H */
