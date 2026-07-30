#ifndef ALGORITHM_CONF_H
#define ALGORITHM_CONF_H

#define RING_SIZE 4096
#define RING_MASK (RING_SIZE - 1)
#define HALF_RING_SIZE 2048
#define NODE_BUFF_SIZE 8192

// TODO: ERROR
#define CON_NODE_CNT 32

#define AIC_OSTD 2
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
#define ED_ENABLE 1
#endif

/*
 * ED_UNFIN_THRESHOLD: stage s-task only when fanin 齐且 unfin_pred_cnt <= N.
 * 一期默认 0xFFFF 等价于「前驱全部 dispatch 即可 stage」。
 */
#ifndef ED_UNFIN_THRESHOLD
#define ED_UNFIN_THRESHOLD 0xFFFF
#endif

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
