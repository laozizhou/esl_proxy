#define _POSIX_C_SOURCE 199309L

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "mem_pool.h"
#include "orch_config.h"
#include "ring_buf.h"

void init_predecessors(void);

void orc_alloc_call(uint64_t orch_args);
int orc_desc_call(uint64_t orch_args, int thread_id, int *created_cnt);
int orc_detect_call(uint64_t orch_args, int thread_id, int total_task_cnt,
    int *created_cnt);

struct worker_thread_arg {
    uint64_t orch_args;
    int thread_id;
    int total_task_cnt;
    int task_count;
    int created_cnt;
    uint64_t elapsed_ns;
};

int desc_thread_count = DESC_THREAD_COUNT;
int desc_batch_size = 128;
int detect_thread_count = DETECT_THREAD_COUNT;
int detect_batch_size = 128;

#define MEM_POOL_BYTES (1024UL * 1024UL * 1024UL)
#define WHEN2FREE_CAP 4096

static uint8_t g_mem_pool_storage[MEM_POOL_BYTES];
static when2free_entry_t g_when2free_entries[WHEN2FREE_CAP];

static void *alloc_thread_func(void *arg)
{
    orc_alloc_call((uint64_t)(uintptr_t)arg);
    return NULL;
}

static void *desc_thread_func(void *arg)
{
    struct worker_thread_arg *targ = (struct worker_thread_arg *)arg;
    uint64_t t0 = get_time_ns();
    targ->task_count =
        orc_desc_call(targ->orch_args, targ->thread_id, &targ->created_cnt);
    targ->elapsed_ns = get_time_ns() - t0;
    return NULL;
}

static void *detect_thread_func(void *arg)
{
    struct worker_thread_arg *targ = (struct worker_thread_arg *)arg;
    uint64_t t0 = get_time_ns();
    targ->task_count = orc_detect_call(targ->orch_args, targ->thread_id,
        targ->total_task_cnt, &targ->created_cnt);
    targ->elapsed_ns = get_time_ns() - t0;
    return NULL;
}

static void print_worker_stats(const char *label, struct worker_thread_arg *args,
    int n)
{
    printf("%s throughput (MTasks/s):\n", label);
    int total_cnt = 0;
    uint64_t max_ns = 0;
    for (int i = 0; i < n; i++) {
        double throughput = (args[i].elapsed_ns == 0)
            ? 0.0
            : (double)args[i].created_cnt / (double)args[i].elapsed_ns * 1000.0;
        double time_240_us = (throughput > 0.0) ? (240.0 / throughput) : 0.0;
        printf("  thread %2d: tasks=%d  created=%d  time=%llu ns  "
               "throughput=%.2f MTasks/s  time_240=%.2f us\n",
            args[i].thread_id, args[i].task_count, args[i].created_cnt,
            (unsigned long long)args[i].elapsed_ns, throughput, time_240_us);
        total_cnt += args[i].created_cnt;
        if (args[i].elapsed_ns > max_ns)
            max_ns = args[i].elapsed_ns;
    }
    printf("%s published=%d  wall_max=%llu ns\n", label, total_cnt,
        (unsigned long long)max_ns);
}

/* Dump predecessor sets for golden compare: one line per task
 *   task_id cnt p0 p1 ... (sorted ascending) */
static int dump_predecessors(const char *path, int total_task_cnt)
{
    FILE *fp = fopen(path, "w");
    if (!fp) {
        perror(path);
        return -1;
    }
    fprintf(fp, "# predecessors dump total_task_cnt=%d\n", total_task_cnt);
    for (int tid = 0; tid < total_task_cnt; tid++) {
        struct predecessor_list *pl = &g_predecessors[tid];
        uint32_t cnt = pl->cnt;
        uint32_t tmp[256];
        if (cnt > 256)
            cnt = 256;
        for (uint32_t i = 0; i < cnt; i++)
            tmp[i] = pl->exp[i];
        /* insertion sort for stable golden */
        for (uint32_t i = 1; i < cnt; i++) {
            uint32_t v = tmp[i];
            uint32_t j = i;
            while (j > 0 && tmp[j - 1] > v) {
                tmp[j] = tmp[j - 1];
                j--;
            }
            tmp[j] = v;
        }
        fprintf(fp, "%d %u", tid, cnt);
        for (uint32_t i = 0; i < cnt; i++)
            fprintf(fp, " %u", tmp[i]);
        fprintf(fp, "\n");
    }
    fclose(fp);
    printf("wrote predecessor dump: %s\n", path);
    return 0;
}

int main(int argc, char *argv[])
{
    const char *dump_path = getenv("DEP_DUMP_PATH");

    if (argc >= 2) {
        desc_thread_count = atoi(argv[1]);
        if (desc_thread_count <= 0) {
            fprintf(stderr,
                "Usage: %s [desc_threads] [detect_threads]\n"
                "  env DEP_DUMP_PATH=file  dump sorted predecessors\n",
                argv[0]);
            return 1;
        }
        detect_thread_count = desc_thread_count;
    }
    if (argc >= 3) {
        detect_thread_count = atoi(argv[2]);
        if (detect_thread_count <= 0) {
            fprintf(stderr, "detect_thread_count must be > 0\n");
            return 1;
        }
    }

    printf("ORCH_USE_INSERT_TICKETS=%d  RING_SIZE=%d  desc=%d detect=%d\n",
#if defined(ORCH_USE_INSERT_TICKETS)
        ORCH_USE_INSERT_TICKETS,
#else
        1,
#endif
        RING_SIZE, desc_thread_count, detect_thread_count);

    mem_pool_init(&g_mem_pool, g_mem_pool_storage, sizeof g_mem_pool_storage);
    mem_pool_init_fifo(&g_mem_pool, g_when2free_entries, WHEN2FREE_CAP);
    ring_buf_init();
    init_predecessors();

    pthread_t alloc_thread;
    pthread_t *desc_threads =
        malloc((size_t)desc_thread_count * sizeof(pthread_t));
    pthread_t *detect_threads =
        malloc((size_t)detect_thread_count * sizeof(pthread_t));
    if (!desc_threads || !detect_threads) {
        fprintf(stderr, "Failed to allocate thread arrays\n");
        free(desc_threads);
        free(detect_threads);
        return 1;
    }

    uint64_t start_ns = get_time_ns();

    pthread_create(&alloc_thread, NULL, alloc_thread_func, (void *)(uintptr_t)0);
    pthread_join(alloc_thread, NULL);

    struct worker_thread_arg *desc_args =
        calloc((size_t)desc_thread_count, sizeof(struct worker_thread_arg));
    if (!desc_args) {
        return 1;
    }
    for (int i = 0; i < desc_thread_count; i++) {
        desc_args[i].thread_id = i;
        pthread_create(&desc_threads[i], NULL, desc_thread_func, &desc_args[i]);
    }
    for (int i = 0; i < desc_thread_count; i++)
        pthread_join(desc_threads[i], NULL);

    int total_task_cnt = desc_args[0].task_count;
    print_worker_stats("desc_thread", desc_args, desc_thread_count);

    struct worker_thread_arg *detect_args =
        calloc((size_t)detect_thread_count, sizeof(struct worker_thread_arg));
    if (!detect_args) {
        return 1;
    }
    for (int i = 0; i < detect_thread_count; i++) {
        detect_args[i].thread_id = i;
        detect_args[i].total_task_cnt = total_task_cnt;
        pthread_create(&detect_threads[i], NULL, detect_thread_func,
            &detect_args[i]);
    }
    for (int i = 0; i < detect_thread_count; i++)
        pthread_join(detect_threads[i], NULL);

    uint64_t end_ns = get_time_ns();
    print_worker_stats("detect_thread", detect_args, detect_thread_count);

    printf("orchestrator total elapsed (1 alloc + %d desc + %d detect): %llu ns\n",
        desc_thread_count, detect_thread_count,
        (unsigned long long)(end_ns - start_ns));
    printf("total_task_cnt=%d\n", total_task_cnt);

    if (dump_path && dump_path[0]) {
        if (dump_predecessors(dump_path, total_task_cnt) != 0)
            return 2;
    }

    free(desc_args);
    free(detect_args);
    free(desc_threads);
    free(detect_threads);
    return 0;
}
