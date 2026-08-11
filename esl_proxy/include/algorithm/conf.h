#ifndef ALGORITHM_CONF_H
#define ALGORITHM_CONF_H

#define RING_SIZE 4096
#define RING_MASK (RING_SIZE - 1)
#define HALF_RING_SIZE 2048
#define NODE_BUFF_SIZE 8192

// TODO: ERROR
#define CON_NODE_CNT 32

/*
 * AIC_OSTD：每核每 type 的在途槽位数（ping-pong 预装载深度）。
 *
 * 注意这不是「每核可并行执行的任务数」——executor 同一时刻只跑一个 slot，
 * 多出来的 slot 只用于预装载：当前任务一跑完，下一个已是 RUNNABLE，
 * 下一拍即可接上，不必等 dispatcher 再跑一轮。因此它的作用是消除
 * 「任务完成 -> dispatcher 回收 free 位 -> 下轮 send_task」这段流水线气泡。
 *
 * 做成旋钮是为了给 ED 归因：ED 的 staged 任务会占住一个槽位且在 GATED
 * 期间不执行，等于把该核的预装载深度减一。加大本值可把这份代价与 ED
 * 本身的收益分离开来。所有依赖它的代码都按本值循环，2 以外的取值合法。
 *
 * ===== 默认取 2 的实测依据 =====
 * case=qwen3_dynamic_manual_scope.h, tier=2, scripts/ed_paired_ab.py 配对 ABBA
 * 200 对（括号内为符号检验：B 更慢的对数占比，纯噪声应约 50%）。
 *
 * 1) 加深槽位本身有成本，且单调恶化——基线(ED 关)以 ostd=2 为 A：
 *      ostd=3  1.02x (54%)   ostd=4  1.09x (60%)
 *      ostd=6  1.28x (75%)   ostd=8  1.29x (83%)
 *    executor 每轮扫描 EXE_TYPE_CNT*AIC_CNT*AIC_OSTD 个 slot_state，线性增长。
 *
 * 2) 加深槽位会把 ED 自己的收益吃掉——A 恒为同 ostd 下的 ED 关：
 *      ostd   ED开(不接SPMD)     ED开(接SPMD)
 *      2      0.90x (43%)        1.15x (70%)
 *      3      1.02x (52%)        1.04x (56%)
 *      4      1.00x (50%)        1.09x (68%)
 *    ostd=2 时 ED 有约 10% 正收益，ostd>=3 退化为噪声。原因是两者攻击的是
 *    同一个气泡（任务完成 -> dispatcher 回收 free 位 -> 下轮才派新任务）：
 *    多一个预装载位不靠投机就把它填掉了，ED 便无处可赚。二者是替代品而非
 *    互补品，所以不能靠加深槽位「腾地方」给 ED。
 *
 * 3) 槽位压力确实被解除了（LAT_TRACE=1, ED 接 SPMD, vector/mix）：
 *    usable_core_mean 32.22 (ostd=2) -> 59.39 (ostd=3)，
 *    即 ostd=2 下约 28 个核两 slot 全满，ostd=3 下几乎恒有空位。
 *    但两种 ostd 的 starve_with_waiters 都是 0——normal 派发从未因缺核卡住，
 *    所以这份「压力」本就不是 makespan 的瓶颈。
 *
 * 结论：全局最优仍是 ostd=2 + ED_SPMD_MAX_BLOCKS=1。ostd=2 组合对 ostd=3 组合
 * 快 1.19x (84%)。
 */
#ifndef AIC_OSTD
#define AIC_OSTD 2
#endif
#define AIC_CNT 60
#define EXE_TYPE_CNT 2

#define CQ_BATCH_SIZE 512
#define PRE_BATCH_SIZE 240
#define RQ_BATCH_SIZE 512
#define DISPATCH_COMPLETE_BATCH 512

#define CUTTER_THREAD_CNT 1
#define DISPATCH_THREAD_CNT 1
#define EXECUTOR_THREAD_CNT 1

/* 1: compile in worker logs; toggle at runtime via g_worker_log or WORKER_LOG env */
#define WORKER_LOG 1

/* 1: compile in main thread logs; output to screen only */
#ifndef MAIN_LOG
#define MAIN_LOG 1
#endif

/* Log output mode: 0=file, 1=stdout, 2=both */
#define LOG_OUTPUT_MODE 2

/* 1: enable aicpu_orchestration_entry execution time logging in nanoseconds */
#define ORCHESTRATION_TIME 1

/* 1: compile post-orchestration DAG dump; runtime via DEP_DUMP=1 env */
#ifndef DEP_DUMP
#define DEP_DUMP 0
#endif

/* 1: skip tensormap lookup/insert and succeed(); all tasks submit with no edges */
#ifndef NO_DEPS
#define NO_DEPS 0
#endif

/* 1: enable early-dispatch; 0: disable (baseline). Override: make ED_ENABLE=0/1 */
#ifndef ED_ENABLE
#define ED_ENABLE 0
#endif

/*
 * ED_ABLATE: 常驻开销 ablation 探针位图，仅用于性能归因，默认 0（全部开启）。
 * 置位后对应记账点被编译掉——只在 ED_UNFIN_THRESHOLD=0（永不 stage）下才等价，
 * 正常跑必须保持 0。用法：make EXTRA_CFLAGS=-DED_ABLATE=1
 */
#ifndef ED_ABLATE
#define ED_ABLATE 0
#endif
#define ED_ABLATE_ROUND    (ED_ABLATE & 1)  /* dispatch 每轮的空队列探测 + try_early_dispatch */
#define ED_ABLATE_SEND     (ED_ABLATE & 2)  /* send_task 内的认领 CAS / record / Hook 0 */
#define ED_ABLATE_CUTTER   (ED_ABLATE & 4)  /* cutter 建图与解依赖内的 ED 记账 */
#define ED_ABLATE_COMPLETE (ED_ABLATE & 8)  /* complete_slot 内的 ED 记账 */
#define ED_ABLATE_GATE     (ED_ABLATE & 16) /* executor 每轮的 GATED 排空（开了必然死锁，仅测开销） */

/*
 * ED_UNFIN_THRESHOLD: stage s-task only when fanin 齐且 unfin_pred_cnt <= N.
 * 一期默认 0xFFFF 等价于「前驱全部 dispatch 即可 stage」。
 */
#ifndef ED_UNFIN_THRESHOLD
#define ED_UNFIN_THRESHOLD 0xFFFF
#endif

/*
 * ED_SPMD_MAX_BLOCKS: 允许进入 ED 的最大 block 数（count 上限）。
 *
 * 语义：count > 本值的 SPMD 任务不进 ED，走常规 ready_queue。
 *   1      = 只接非 SPMD 任务（默认）
 *   2/4/…  = 只接短 SPMD 任务
 *   0xFFFF = 全部接
 *
 * 这是调优旋钮而非纯开关，因为收益与代价方向相反：
 *   - 收益侧：ED 省下的是「一次 ready->runnable 空等」这段固定延迟，与 count 无关；
 *     而 count=N 的任务执行时长是 N 倍，故单任务相对收益约按 1/N 衰减。
 *   - 代价侧：staged 槽位在 GATED 期间被占住但不执行，normal 队列里已就绪的任务
 *     拿不到它。这份代价不随 count 缩小；而 pick_stage_core 的二选恰好优先挑
 *     「两个 slot 全空的核」，也就是 normal 最想要的那批核。
 *
 * ===== 默认取 1（即默认不接 SPMD）的实测依据 =====
 * case=qwen3_dynamic_manual_scope.h，scripts/ed_paired_ab.py 配对 ABBA，40 对，
 * 基准恒为本旋钮=1：
 *
 *   tier   knob=2              knob=4              knob=0xFFFF
 *   2      1.05~1.16x (57%)    1.36x (77%)         1.16~1.28x (87~90%)
 *   4      1.05x (53%)         1.25x (87%)         1.14~1.16x (77~80%)
 *   （括号内为符号检验：B 更慢的对数占比，纯噪声应约 50%）
 *
 * 覆盖率确实随旋钮放大而上升（tier2 下 stage_cnt 76 -> 128），但 makespan 全线
 * 变差：knob=2 勉强打平（符号检验贴近噪声），knob>=4 明确为负。即「提高覆盖率」
 * 本身不是收益，槽位被投机占住的代价更大。故默认保持 1，需要时显式放开并按
 * case/tier 重测。不要用块设计（先跑 N 次 A 再跑 N 次 B）验证：机器慢漂移能整块
 * 压在一个 variant 上，实测同一二进制换位置中位数可差 6%，而效应量本身才 10~15%。
 */
#ifndef ED_SPMD_MAX_BLOCKS
#define ED_SPMD_MAX_BLOCKS 1
#endif
/* 准入判据是 early_dispatch.h 的 ed_count_admitted()，三个调用点共用它保证同口径。 */

/*
 * 模拟执行时长缩放因子：executor 每次派发时把 duration 按该因子缩小；
 * 例如 10000 表示 raw_duration/10000（并且最少执行 1 tick）。
 */
#ifndef EXEC_DURATION_SCALE
#define EXEC_DURATION_SCALE 10000u
#endif

#if EXEC_DURATION_SCALE == 0
#error "EXEC_DURATION_SCALE must be > 0"
#endif

/*
 * 返回 uint32_t：raw_duration 可超过 65535（qwen3 的 GATE/UP/DOWN_PROJ 就是
 * 7-9 万 cycles），若在此截断，EXEC_DURATION_SCALE 较小时缩放结果会回绕。
 */
#define SCALE_EXEC_DURATION(raw_duration)                                         \
    (((uint32_t)(raw_duration) > (uint32_t)(EXEC_DURATION_SCALE))               \
         ? (uint32_t)((uint32_t)(raw_duration) / (uint32_t)(EXEC_DURATION_SCALE)) \
         : (uint32_t)1)

#endif /* ALGORITHM_CONF_H */
