/*
 * executor.h - Executor Type Definition
 *
 * Defines the executor type used by Dispatch for task execution.
 * The executor runs tasks in its 2-slot PING PONG cache.
 *
 * Trust the Caller (Principle X): No input validation, undefined on invalid input.
 * C11 standard with _Atomic for lock-free concurrency.
 */

#ifndef EXECUTOR_H
#define EXECUTOR_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdatomic.h>

#include "conf.h"

/*
 * Executor slot 发布状态（Step 2+ normal dispatch / Step 7 ed gate 使用）。
 * EMPTY：无任务；GATED：已 stage 等 notify；RUNNABLE：executor 可 tick。
 */
typedef enum {
    EXE_SLOT_EMPTY    = 0,
    EXE_SLOT_GATED    = 1,
    EXE_SLOT_RUNNABLE = 2,
} exe_slot_state_t;

/*
 * Executor
 */
typedef struct executor {
    uint8_t idx;
    uint16_t tasks[AIC_OSTD];
    uint16_t block_idx[AIC_OSTD];
    uint16_t duration[AIC_OSTD];
    uint64_t base[AIC_OSTD];
    /* 跨线程 payload 发布边界；ED_ENABLE=0 时 normal dispatch 也依赖此字段 */
    _Atomic uint8_t slot_state[AIC_OSTD];
#if ED_ENABLE
    /* 硬件 doorbell 模型占位；仅 notify_claimed CAS 胜者写 1 */
    _Atomic uint8_t doorbell[AIC_OSTD];
#endif
} executor_t;

/*
 * executor_init - Initialize all executors
 */
void executor_init(void);

/*
 * executor_worker - Worker thread for executor timing
 */
void* executor_worker(void *arg);

/*
 * Global executor array - EXE_TYPE_CNT x AIC_CNT
 */
extern executor_t g_executors[EXE_TYPE_CNT][AIC_CNT];

#endif /* EXECUTOR_H */
