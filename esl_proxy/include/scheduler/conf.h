#ifndef SCHEDULER_CONF_H
#define SCHEDULER_CONF_H

#define RING_SIZE 2048
#define RING_MASK (RING_SIZE - 1)
#define HALF_RING_SIZE 1024
#define NODE_BUFF_SIZE 8192
#define CON_NODE_CNT 32

#define AIC_OSTD 2
#define AIC_CNT 64

#define AIC_CNT_PER_DIE 32
#define DIE_CNT 2
#define EXE_TYPE_CNT 2

#define CQ_BATCH_SIZE 512
#define PRE_BATCH_SIZE 240
#define RQ_BATCH_SIZE 512
#define DISPATCH_COMPLETE_BATCH 512

/* CLC (Makefile_clc) builds against the cases/... _total_graph.h headers, which carry a
 * `#if PAINTER_THREAD_CNT != 1 / #error` guard. Without this #ifndef the value below
 * unconditionally overrides -DPAINTER_THREAD_CNT=1 from the command line and that header
 * can never be compiled. Same fix already applied to LOG_OUTPUT_MODE below.
 * The scheduler build passes no such -D, so it still gets 2. */
#ifndef PAINTER_THREAD_CNT
#define PAINTER_THREAD_CNT 2
#endif
#define DISPATCH_THREAD_CNT 2
#define AIC_CNT_PER_THREAD 32

/* 1: compile in worker logs; toggle at runtime via g_worker_log or WORKER_LOG env */
#define WORKER_LOG 1

/* Log output mode: 0=file, 1=stdout, 2=both */
#ifndef LOG_OUTPUT_MODE
#define LOG_OUTPUT_MODE 2
#endif

#endif /* SCHEDULER_CONF_H */
