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

#ifndef QWEN3_SPMD_TIER
#define QWEN3_SPMD_TIER 0
#endif
#if QWEN3_SPMD_TIER < 0 || QWEN3_SPMD_TIER > 4
#error "QWEN3_SPMD_TIER must be 0..4"
#endif

#endif /* ORCH_CONFIG_H */