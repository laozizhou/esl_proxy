/**
 * @file atomic_fetch_add.c
 * @brief Multi-threaded atomic_fetch_add on a 4-byte (int32_t) target
 *        at a user-specified address.
 *
 * Usage:
 *   ./atomic_fetch_add <num_threads> <hex_address> <iterations>
 *
 * Example:
 *   ./atomic_fetch_add 4 0x100000000 1000000
 *
 * Each thread performs <iterations> atomic_fetch_add(&target, 1) ops.
 * The target pointer is cast directly from the user-supplied hex address,
 * enabling tests against shared-memory or device-mapped regions.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>

/* ============================================================================
 * Types
 * ============================================================================ */

/* 4-byte atomic type – matches "4B data" requirement */
typedef _Atomic int32_t atomic_int32_t;

typedef struct {
    int           thread_id;
    int           iterations;
    atomic_int32_t *target;
    volatile _Atomic int *ready_flag;   /* signals completion order */
} thread_args_t;

/* ============================================================================
 * Barrier for synchronised start
 * ============================================================================ */

static pthread_mutex_t barrier_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  barrier_cond  = PTHREAD_COND_INITIALIZER;
static int barrier_count = 0;
static int barrier_total = 0;

static void barrier_wait(void) {
    pthread_mutex_lock(&barrier_mutex);
    barrier_count++;
    if (barrier_count == barrier_total) {
        barrier_count = 0;
        pthread_cond_broadcast(&barrier_cond);
    } else {
        pthread_cond_wait(&barrier_cond, &barrier_mutex);
    }
    pthread_mutex_unlock(&barrier_mutex);
}

/* ============================================================================
 * Worker thread – executes atomic_fetch_add in a tight loop
 * ============================================================================ */

static void *worker(void *arg) {
    thread_args_t *a = (thread_args_t *)arg;

    barrier_wait();   /* wait for all threads to be ready */

    for (int i = 0; i < a->iterations; i++) {
        atomic_fetch_add_explicit(a->target, 1, memory_order_relaxed);
    }

    atomic_store(a->ready_flag, a->thread_id + 1);

    return NULL;
}

/* ============================================================================
 * Get time in nanoseconds
 * ============================================================================ */

static inline uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* ============================================================================
 * main
 * ============================================================================ */

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr,
                "Usage: %s <num_threads> <hex_address> <iterations>\n"
                "Example: %s 4 0x100000000 1000000\n",
                argv[0], argv[0]);
        return EXIT_FAILURE;
    }

    /* --- parse arguments ------------------------------------------------ */
    char *endptr;

    int num_threads = (int)strtol(argv[1], &endptr, 10);
    if (*endptr != '\0' || num_threads <= 0) {
        fprintf(stderr, "Error: invalid thread count '%s'\n", argv[1]);
        return EXIT_FAILURE;
    }

    uintptr_t addr = (uintptr_t)strtoull(argv[2], &endptr, 16);
    if (*endptr != '\0') {
        fprintf(stderr, "Error: invalid hex address '%s'\n", argv[2]);
        return EXIT_FAILURE;
    }

    int iterations = (int)strtol(argv[3], &endptr, 10);
    if (*endptr != '\0' || iterations <= 0) {
        fprintf(stderr, "Error: invalid iteration count '%s'\n", argv[3]);
        return EXIT_FAILURE;
    }

    /* --- target pointer ------------------------------------------------ */
    atomic_int32_t *target = (atomic_int32_t *)addr;
    int32_t initial = atomic_load(target);

    printf("=============================================================\n");
    printf("  Multi-Threaded atomic_fetch_add (4-byte)\n");
    printf("=============================================================\n");
    printf("  Threads:    %d\n", num_threads);
    printf("  Address:    0x%lx\n", addr);
    printf("  Iterations: %d / thread\n", iterations);
    printf("  Total ops:  %lu\n", (unsigned long)num_threads * iterations);
    printf("  Initial value: %d\n", initial);
    printf("-------------------------------------------------------------\n");

    /* --- setup threads ------------------------------------------------ */
    pthread_t      *threads = malloc((size_t)num_threads * sizeof(pthread_t));
    thread_args_t  *args   = malloc((size_t)num_threads * sizeof(thread_args_t));
    volatile _Atomic int ready_flag = 0;

    if (!threads || !args) {
        fprintf(stderr, "Error: malloc failed\n");
        free(threads);
        free(args);
        return EXIT_FAILURE;
    }

    barrier_total = num_threads + 1;   /* +1 for main */

    for (int i = 0; i < num_threads; i++) {
        args[i].thread_id   = i;
        args[i].iterations  = iterations;
        args[i].target      = target;
        args[i].ready_flag  = &ready_flag;
    }

    /* --- launch threads & time ---------------------------------------- */
    uint64_t t0, t1;

    for (int i = 0; i < num_threads; i++) {
        if (pthread_create(&threads[i], NULL, worker, &args[i]) != 0) {
            fprintf(stderr, "Error: pthread_create failed for thread %d\n", i);
            free(threads);
            free(args);
            return EXIT_FAILURE;
        }
    }

    barrier_wait();   /* release all threads simultaneously */
    t0 = get_time_ns();

    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    t1 = get_time_ns();

    /* --- report -------------------------------------------------------- */
    int32_t final = atomic_load(target);
    double elapsed_ms = (double)(t1 - t0) / 1000000.0;
    double ops_per_sec = (double)(num_threads * iterations) /
                         ((t1 - t0) / 1e9);
    double ns_per_op = (double)(t1 - t0) / (num_threads * iterations);

    printf("  Final value: %d (expected %d)\n",
           final, initial + num_threads * iterations);
    printf("  Elapsed:     %.2f ms\n", elapsed_ms);
    printf("  Ops/sec:     %.2f M\n", ops_per_sec / 1e6);
    printf("  ns/op:       %.2f ns\n", ns_per_op);
    printf("=============================================================\n");

    free(threads);
    free(args);
    return EXIT_SUCCESS;
}