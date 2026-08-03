#define _POSIX_C_SOURCE 199309L

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "log.h"
#include "orch_config.h"

/* Declared in orc_alloc.c / orc_desc.c (separate translation units to avoid
 * symbol conflicts between qwen3_14b_decoder_alloc.h and
 * qwen3_14b_decoder_desc.h) */
void orc_alloc_call(uint64_t orch_args);
int orc_desc_call(uint64_t orch_args, int thread_id, int *created_cnt);

struct desc_thread_arg {
    uint64_t orch_args;
    int thread_id;
    int task_count;
    int created_cnt;
    uint64_t elapsed_ns;
};

int desc_thread_count = DESC_THREAD_COUNT;
int desc_batch_size = 128;

static void *alloc_thread_func(void *arg)
{
    uint64_t orch_args = (uint64_t)(uintptr_t)arg;
    orc_alloc_call(orch_args);
    return NULL;
}

static void *desc_thread_func(void *arg)
{
    struct desc_thread_arg *targ = (struct desc_thread_arg *)arg;
    uint64_t t0 = get_time_ns();
    targ->task_count = orc_desc_call(targ->orch_args, targ->thread_id, &targ->created_cnt);
    uint64_t t1 = get_time_ns();
    targ->elapsed_ns = t1 - t0;
    return NULL;
}

int main(int argc, char *argv[])
{
    if (argc >= 2) {
        desc_thread_count = atoi(argv[1]);
        desc_batch_size = (240 / desc_thread_count & 127 + 1) * 128;
        if (desc_thread_count <= 0) {
            fprintf(stderr, "Usage: %s [desc_desc_thread_count]  (default %d)\n",
                    argv[0], DESC_THREAD_COUNT);
            return 1;
        }
    }

    pthread_t alloc_thread;
    pthread_t *desc_threads = malloc((size_t)desc_thread_count * sizeof(pthread_t));
    if (!desc_threads) {
        fprintf(stderr, "Failed to allocate thread array\n");
        return 1;
    }

    uint64_t start_ns, end_ns, elapsed_ns;
    start_ns = get_time_ns();

    /* 1 orchestrator_alloc thread */
    pthread_create(&alloc_thread, NULL, alloc_thread_func, (void *)(uintptr_t)0);
    pthread_join(alloc_thread, NULL);

    /* N orchestrator_desc threads in parallel (started after alloc thread finishes) */
    struct desc_thread_arg *desc_args = malloc((size_t)desc_thread_count * sizeof(struct desc_thread_arg));
    if (!desc_args) {
        fprintf(stderr, "Failed to allocate desc thread args\n");
        free(desc_threads);
        return 1;
    }
    for (int i = 0; i < desc_thread_count; i++) {
        desc_args[i].orch_args = 0;
        desc_args[i].thread_id = i;
        pthread_create(&desc_threads[i], NULL, desc_thread_func, &desc_args[i]);
    }

    for (int i = 0; i < desc_thread_count; i++) {
        pthread_join(desc_threads[i], NULL);
    }

    end_ns = get_time_ns();
    elapsed_ns = end_ns - start_ns;
    /* Print per-thread throughput: desc_task_id / execution_time (MTasks/s) */
    printf("desc_thread throughput (MTasks/s):\n");
    int total_cnt = 0;
    for (int i = 0; i < desc_thread_count; i++) {
        /* throughput = tasks / us   (because MTasks/s = tasks / (us * 1e-6) * 1e-6 = tasks / us) */
        double throughput = (double)desc_args[i].task_count / (double)desc_args[i].elapsed_ns * (double)1000.0;
        printf("  thread %2d: tasks=%d  created=%d  time=%llu ns  throughput=%.2f MTasks/s\n",
               desc_args[i].thread_id,
               desc_args[i].task_count,
               desc_args[i].created_cnt,
               (unsigned long long)desc_args[i].elapsed_ns,
               throughput);
        total_cnt += desc_args[i].created_cnt;
    }
    printf("orchestrator total elapsed time (1 alloc + %d desc threads): %llu ns\n",
           desc_thread_count, (unsigned long long)elapsed_ns);
    printf("desc=%d\n", total_cnt);
    free(desc_args);
    free(desc_threads);
    return 0;
}