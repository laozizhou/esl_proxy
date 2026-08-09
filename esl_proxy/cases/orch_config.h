#ifndef ORCH_CONFIG_H
#define ORCH_CONFIG_H

/*
 * Orchestrator configuration shared across:
 *   - esl_proxy/cases/qwen3_14b_decoder_desc.h
 *   - esl_proxy/cases/qwen3_14b_decoder_alloc.h
 *   - esl_proxy/src/orchestrator/orchestrator.c
 *
 * Override defaults via -D compiler flags, e.g.:
 *   make -f Makefile_orchestrator CFLAGS="-DDESC_THREAD_COUNT=4 -DMAX_TENSOR_NUM=4096"
 */

#ifndef DESC_THREAD_COUNT
#define DESC_THREAD_COUNT 8
#endif

#ifndef MAX_TENSOR_NUM
#define MAX_TENSOR_NUM 2048
#endif

#define RING_SIZE 2048

#ifndef QWEN3_SPMD_TIER
#define QWEN3_SPMD_TIER 0
#endif
#if QWEN3_SPMD_TIER < 0 || QWEN3_SPMD_TIER > 4
#error "QWEN3_SPMD_TIER must be 0..4"
#endif

#include "tensor.h"

struct task_tensor_desc {
    uint16_t       id;          /* ring-buffer task id */
    uint16_t       out_cnt;  /* number of valid data[] entries */
    uint16_t       in_cnt;  /* number of valid data[] entries */
    uint16_t       inout_cnt;  /* number of valid data[] entries */
    Tensor         in_data[8];    /* tensor addresses (Tensor handles) */
    Tensor         out_data[8];    /* tensor addresses (Tensor handles) */
    Tensor         inout_data[8];    /* tensor addresses (Tensor handles) */
};

#endif /* ORCH_CONFIG_H */