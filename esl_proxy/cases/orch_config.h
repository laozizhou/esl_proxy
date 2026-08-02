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

// struct Tensor {
//     /* === Cache line 1 (64B) — hot path === */
//     uint64_t buffer_addr;
//     uint64_t buffer_size;
//     uint64_t owner_task_id;
//     uint64_t start_offset;
//     int32_t version;
//     uint32_t ndims;
//     uint8_t dtype;
//     uint8_t manual_dep;
//     uint8_t is_contiguous;
//     uint8_t _pad_cl1;
//     uint32_t shapes[ESL_PROXY_TENSOR_MAX_DIMS];

//     /* === Cache line 2 (64B) — warm path === */
//     uint64_t extent_elem_cache;
//     uint32_t strides[ESL_PROXY_TENSOR_MAX_DIMS];
//     uint8_t _pad_cl2[36];
// } __attribute__((aligned(64)));

// typedef struct Tensor Tensor;
#include "tensor.h"

struct task_tensor_desc {
    uint16_t       id;          /* ring-buffer task id */
    uint16_t       tensor_cnt;  /* number of valid data[] entries */
    Tensor         data[16];    /* tensor addresses (Tensor handles) */
};

#endif /* ORCH_CONFIG_H */