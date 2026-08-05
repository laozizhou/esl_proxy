/*
 * sched_only.c - Scheduler-only entry point (no orchestrator thread)
 *
 * Reads a dag_export.c-format CSV (task_id,type,duration,predecessors) and
 * loads it directly into g_basic_buf / g_predecessors / g_predecessor_ring,
 * bypassing aicpu_orchestration_entry() and add_predecessors() entirely.
 * Then sets g_task_id/g_orch_is_done as if orchestration had already
 * finished instantly, and launches the *unmodified* cutter_worker /
 * dispatch_worker threads from src/algorithm/ against this pre-loaded graph.
 *
 * This deliberately tests the "orchestrator already produced the whole
 * graph, cutter/dispatch must drain it as fast as possible" scenario -- the
 * one case tonight's testing found where a smaller edge count can actually
 * show up as a real, measurable win (see the manual_scope TIER2-4 findings).
 *
 * Usage: ./bin/sched_only path/to/dag.csv
 */
#define _POSIX_C_SOURCE 199309L

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "conf.h"
#include "cutter.h"
#include "dispatch.h"
#include "log.h"
#include "ring_buf.h"

extern atomic_int g_task_id;
extern atomic_bool g_orch_is_done;
extern struct task_desc g_basic_buf[RING_SIZE];
extern struct predecessor_list g_predecessors[RING_SIZE];
extern struct ring_buf g_predecessor_ring;

static void load_csv(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "failed to open %s\n", path);
        exit(1);
    }

    ring_buf_init();

    char line[4096];
    /* skip header */
    if (!fgets(line, sizeof line, f)) {
        fprintf(stderr, "empty csv: %s\n", path);
        exit(1);
    }

    uint32_t ring_idx = 0;
    uint32_t max_task_id = 0;

    while (fgets(line, sizeof line, f)) {
        char *saveptr = NULL;
        char *tok = strtok_r(line, ",", &saveptr);
        if (!tok) continue;
        uint32_t task_id = (uint32_t)strtoul(tok, NULL, 10);

        tok = strtok_r(NULL, ",", &saveptr);
        int type = tok ? atoi(tok) : 0;

        tok = strtok_r(NULL, ",", &saveptr);
        uint32_t duration = tok ? (uint32_t)strtoul(tok, NULL, 10) : 0;

        /* Remainder of the line is the ';'-separated predecessor list (may
         * be empty for tasks with no predecessors). strtok_r already
         * consumed the trailing '\n' as part of the 4th field via the ','
         * delimiter set below -- split on ';' and newline both. */
        char *preds_field = strtok_r(NULL, "\n", &saveptr);

        g_basic_buf[task_id].type = (task_type_t)type;
        g_basic_buf[task_id].duration = duration;

        struct predecessor_list *p = &g_predecessors[task_id];
        p->exp = g_predecessor_ring.head + ring_idx;
        p->cnt = 0;

        if (preds_field && preds_field[0] != '\0') {
            char *psave = NULL;
            char *ptok = strtok_r(preds_field, ";", &psave);
            while (ptok) {
                g_predecessor_ring.head[ring_idx++] = (uint32_t)strtoul(ptok, NULL, 10);
                p->cnt++;
                ptok = strtok_r(NULL, ";", &psave);
            }
        }

        if (task_id > max_task_id) {
            max_task_id = task_id;
        }
    }
    fclose(f);

    atomic_store(&g_predecessor_ring.tail, ring_idx);
    atomic_store(&g_task_id, (int)(max_task_id + 1));
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s path/to/dag.csv\n", argv[0]);
        return 1;
    }

    init_ctrl_t();
    load_csv(argv[1]);

    pthread_t cutter_thread;
    pthread_t dispatch_thread;

    uint64_t start_ns = get_time_ns();

    /* Orchestrator "finished" the instant we loaded the graph. */
    atomic_store(&g_orch_is_done, true);

    pthread_create(&cutter_thread, NULL, cutter_worker, (void *)(intptr_t)0);
    pthread_create(&dispatch_thread, NULL, dispatch_worker, (void *)(intptr_t)0);

    pthread_join(cutter_thread, NULL);
    pthread_join(dispatch_thread, NULL);

    uint64_t end_ns = get_time_ns();
    uint64_t elapsed_ns = end_ns - start_ns;

    MAIN_LOGF("[sched_only] task_cnt = %u", g_task_id);
    MAIN_LOGF("[sched_only] elapsed_time = %llu ns", (unsigned long long)elapsed_ns);
    MAIN_LOGF("[sched_only] task_tp = %f MTasks/s", (float)(g_task_id * 1000.0 / elapsed_ns));

    return 0;
}
