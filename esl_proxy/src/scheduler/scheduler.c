#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#ifdef __linux__
#include <sched.h>
#endif

#include "scheduler/conf.h"
#include "scheduler/painter.h"
#include "scheduler/dispatch.h"
// #include "common/log.h"
#include <string.h>

uint64_t get_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* Global variable definitions needed by dispatch.c and painter.c */
atomic_bool g_is_done = false;
atomic_int g_min_uncomplete_task = 0;

#define TASK_EXEC_RECORDS_MAX 8192

typedef struct {
    uint32_t task_id;
    uint32_t task_type;      /* TASK_TYPE_CUBE=0, TASK_TYPE_VECTOR=1 */
    uint32_t core_id;        /* local core index within dispatcher (0..AIC_CNT_PER_THREAD-1) */
    uint64_t start_time_ns;
    uint64_t end_time_ns;
} task_exec_record_t;

/* Task execution records — see dispatch.h for the struct definition */
task_exec_record_t g_task_exec_records[TASK_EXEC_RECORDS_MAX];

static void handle_signal(int sig)
{
    (void)sig;
    atomic_store_explicit(&g_is_done, true, memory_order_release);
}

void *worker(void *arg)
{
    int tid = (int)(intptr_t)arg;
    printf("%d,\n", tid);
    return NULL;
}

int main(void) {
    pthread_t dispatch_threads[DISPATCH_THREAD_CNT];
    pthread_t painter_threads[PAINTER_THREAD_CNT];

    // log_init("scheduler");

    // /* SCHEDULER_LOG env var controls worker logging at runtime:
    //  *   unset or "1" -> enabled (default)
    //  *   "0" or "off" -> disabled for performance testing */
    // char *log_env = getenv("SCHEDULER_LOG");
    // g_worker_log = (!log_env || (strcmp(log_env, "0") != 0 && strcmp(log_env, "off") != 0)) ? 1 : 0;

    buf_init();
    init_state_buf();
    init_ctrl_t();
    // WORKER_LOGF("painter_cnt,%d,dispatcher_cnt,%d", PAINTER_THREAD_CNT, DISPATCH_THREAD_CNT);
    /* Register signal handlers for graceful shutdown on Ctrl+C */
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    for (int i = 0; i < PAINTER_THREAD_CNT; i++) {
        pthread_create(&painter_threads[i], NULL, painter, (void *)(intptr_t)i);
#ifdef __linux__
        cpu_set_t mask;
        CPU_ZERO(&mask);
        CPU_SET(2*i, &mask);
        pthread_setaffinity_np(painter_threads[i], sizeof(cpu_set_t), &mask);
#endif
    }

    for (int i = 0; i < DISPATCH_THREAD_CNT; i++) {
        pthread_create(&dispatch_threads[i], NULL, dispatch_worker, (void *)(intptr_t)i);
#ifdef __linux__
        cpu_set_t mask;
        CPU_ZERO(&mask);
        CPU_SET((2*i + 1), &mask);
        pthread_setaffinity_np(dispatch_threads[i], sizeof(cpu_set_t), &mask);
#endif
    }

    for (int i = 0; i < PAINTER_THREAD_CNT; i++) {
        pthread_join(painter_threads[i], NULL);
    }

    for (int i = 0; i < DISPATCH_THREAD_CNT; i++) {
        pthread_join(dispatch_threads[i], NULL);
    }

    /* ---- dump task execution records to file ---- */
    {
        const char *path = "task_exec_records.csv";
        FILE *f = fopen(path, "w");
        if (f != NULL) {
            fprintf(f, "task_id,task_type,core_id,start_time_ns,end_time_ns,duration_ns\n");
            for (uint32_t i = 0; i < total_task_cnt; i++) {
                const task_exec_record_t *r = &g_task_exec_records[i];
                fprintf(f, "%u,%u,%u,%llu,%llu,%llu\n",
                        r->task_id, r->task_type, r->core_id,
                        (unsigned long long)r->start_time_ns,
                        (unsigned long long)r->end_time_ns,
                        (unsigned long long)(r->end_time_ns - r->start_time_ns));
            }
            fclose(f);
            printf("[scheduler] wrote %u records to %s\n", total_task_cnt, path);
        } else {
            fprintf(stderr, "[scheduler] failed to open %s for writing\n", path);
        }
    }

    // WORKER_LOGF("scheduler_duration,%lld/ns", duration);
    // log_close();
    return 0;
}
