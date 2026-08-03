/*
 * dag_export.c - Orchestrator-only entry point (no Scheduler threads)
 *
 * Runs just aicpu_orchestration_entry() single-threaded, then dumps the
 * resulting dependency graph (g_basic_buf + g_predecessors) to a CSV file,
 * one line per task: task_id,type,duration,pred1;pred2;pred3
 *
 * This intentionally skips cutter/dispatch entirely, so it does not touch
 * the crash we found in cutter.c's ring-buffer handling.
 */
#define _POSIX_C_SOURCE 199309L

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

#include "conf.h"
#include "log.h"
#include "mem_pool.h"

#ifndef ORCH_CASE
#define ORCH_CASE qwen3_dynamic_tensormap.h
#endif

#define INCLUDE(x) #x
#define INCLUDE_FILE(x) INCLUDE(x)
#include INCLUDE_FILE(ORCH_CASE)

void aicpu_orchestration_entry(const uint64_t orch_args);

#define MEM_POOL_BYTES (1024UL * 1024UL * 1024UL)
#define WHEN2FREE_CAP 4096

static uint8_t g_mem_pool_storage[MEM_POOL_BYTES];
static when2free_entry_t g_when2free_entries[WHEN2FREE_CAP];

int main(void) {
    mem_pool_init(&g_mem_pool, g_mem_pool_storage, sizeof g_mem_pool_storage);
    mem_pool_init_fifo(&g_mem_pool, g_when2free_entries, WHEN2FREE_CAP);
    ring_buf_init();
    init_predecessors();

    aicpu_orchestration_entry(0);

    int task_cnt = (int)g_task_id;
    fprintf(stderr, "task_cnt = %d\n", task_cnt);

    FILE *f = fopen("dag_raw.csv", "w");
    if (!f) {
        fprintf(stderr, "failed to open dag_raw.csv for writing\n");
        return 1;
    }
    fprintf(f, "task_id,type,duration,predecessors\n");
    for (int t = 0; t < task_cnt; t++) {
        struct predecessor_list *p = &g_predecessors[t];
        fprintf(f, "%d,%d,%u,", t, g_basic_buf[t].type, g_basic_buf[t].duration);
        for (uint16_t k = 0; k < p->cnt; k++) {
            fprintf(f, "%u%s", p->exp[k], (uint16_t)(k + 1) < p->cnt ? ";" : "");
        }
        fprintf(f, "\n");
    }
    fclose(f);
    fprintf(stderr, "wrote dag_raw.csv\n");

    return 0;
}
