#ifndef ORCH_CONFIG_H
#define ORCH_CONFIG_H

/*
 * Orchestrator configuration shared across alloc / desc / detect.
 *
 * Override via -D, e.g.:
 *   make -f Makefile_orchestrator EXTRA_CFLAGS="-DDESC_THREAD_COUNT=4"
 */

#ifndef DESC_THREAD_COUNT
#define DESC_THREAD_COUNT 8
#endif

#ifndef DETECT_THREAD_COUNT
#define DETECT_THREAD_COUNT DESC_THREAD_COUNT
#endif

#ifndef MAX_TENSOR_NUM
#define MAX_TENSOR_NUM 2048
#endif

/* non-spmd Qwen3 ≈ 3096 tasks; must fit without new_task backpressure. */
#ifndef RING_SIZE
#define RING_SIZE 4096
#endif
#ifndef RING_MASK
#define RING_MASK (RING_SIZE - 1)
#endif

#ifndef QWEN3_SPMD_TIER
#define QWEN3_SPMD_TIER 0
#endif
#if QWEN3_SPMD_TIER < 0 || QWEN3_SPMD_TIER > 4
#error "QWEN3_SPMD_TIER must be 0..4"
#endif

/* Must match DETECT_TMD_NUM_BUCKETS in qwen3_14b_decoder_detect.h */
#ifndef ORCH_TM_NUM_BUCKETS
#define ORCH_TM_NUM_BUCKETS 2048u
#endif

#ifndef ORCH_IO_MAX
#define ORCH_IO_MAX 16
#endif

/* Describe→detect insert 前移开关（类型/填票在 qwen3_14b_decoder_desc.h）。 */
#ifndef ORCH_USE_INSERT_TICKETS
#define ORCH_USE_INSERT_TICKETS 1
#endif

#include "tensor.h"

struct task_tensor_desc {
    uint16_t id;
    uint16_t in_cnt;
    uint16_t out_cnt;
    uint16_t inout_cnt;
    Tensor in_data[ORCH_IO_MAX];
    Tensor out_data[ORCH_IO_MAX];
    Tensor inout_data[ORCH_IO_MAX];
};

#endif /* ORCH_CONFIG_H */
