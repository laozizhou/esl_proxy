/**
 * @file atomic_fetch_add.c
 * @brief 通过 /dev/mem 映射物理地址，多线程 atomic_fetch_add 读写同一 4B 数据
 *
 * Usage:
 *   ./atomic_fetch_add <physical_addr> [num_threads] [iterations]
 *
 *   physical_addr: 物理地址（十六进制），如 0x100000000
 *   num_threads:   线程数，默认 4
 *   iterations:    每个线程的循环次数，默认 1000000
 *
 * Example:
 *   sudo ./atomic_fetch_add 0x100000000
 *   sudo ./atomic_fetch_add 0x100000000 8 5000000
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

/* ============================================================================
 * Defaults
 * ============================================================================ */

#define DEFAULT_NUM_THREADS  4
#define DEFAULT_ITERATIONS   1000000

/* ============================================================================
 * Types
 * ============================================================================ */

/* 4-byte atomic type */
typedef _Atomic int32_t atomic_int32_t;

typedef struct {
    int              thread_id;
    int              iterations;
    atomic_int32_t  *target;
    volatile _Atomic int *ready_flag;
} thread_args_t;

/* ============================================================================
 * Barrier for synchronised start
 * ============================================================================ */

static pthread_mutex_t barrier_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  barrier_cond  = PTHREAD_COND_INITIALIZER;
static int barrier_count = 0;
static int barrier_total = 0;

static void barrier_init(int total) {
    barrier_count = 0;
    barrier_total = total;
}

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
 * /dev/mem mapping
 * ============================================================================ */

/**
 * Map a physical address via /dev/mem.
 *
 * @param phys_addr   Physical address to map (byte-granular).
 * @param size        Number of bytes to map (will be page-aligned internally).
 * @param mapped_base Output: start of the mapped virtual region.
 * @param mapped_size Output: actual mapped size (page-aligned).
 * @return 0 on success, -1 on error.
 */
static int devmem_map(uintptr_t phys_addr, size_t size,
                      void **mapped_base, size_t *mapped_size) {
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        fprintf(stderr, "Error: cannot open /dev/mem: %s\n", strerror(errno));
        fprintf(stderr, "  (Are you running as root?)\n");
        return -1;
    }

    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size < 0) {
        fprintf(stderr, "Error: sysconf(_SC_PAGESIZE) failed\n");
        close(fd);
        return -1;
    }

    /* page-align the base address and adjust the size accordingly */
    uintptr_t page_base = phys_addr & ~((uintptr_t)page_size - 1);
    size_t offset       = phys_addr - page_base;
    size_t map_len      = offset + size;

    /* round up to page boundary */
    map_len = (map_len + (size_t)page_size - 1) & ~((size_t)page_size - 1);

    void *virt = mmap(NULL, map_len, PROT_READ | PROT_WRITE,
                      MAP_SHARED, fd, (off_t)page_base);
    close(fd);

    if (virt == MAP_FAILED) {
        fprintf(stderr, "Error: mmap /dev/mem (phys=0x%lx, len=%zu) failed: %s\n",
                phys_addr, map_len, strerror(errno));
        return -1;
    }

    *mapped_base = virt;
    *mapped_size = map_len;

    printf("  /dev/mem mapped: phys=0x%lx -> virt=%p, offset=0x%zx, size=%zu\n",
           phys_addr, virt, offset, map_len);

    return 0;
}

/* ============================================================================
 * main
 * ============================================================================ */

int main(int argc, char *argv[]) {
    if (argc < 2 || argc > 4) {
        fprintf(stderr,
                "Usage: %s <physical_addr> [num_threads] [iterations]\n"
                "  physical_addr: hex physical address (e.g. 0x100000000)\n"
                "  num_threads:   thread count (default %d)\n"
                "  iterations:    iterations per thread (default %d)\n"
                "\nExample:\n"
                "  sudo %s 0x100000000\n"
                "  sudo %s 0x100000000 8 5000000\n",
                argv[0], DEFAULT_NUM_THREADS, DEFAULT_ITERATIONS,
                argv[0], argv[0]);
        return EXIT_FAILURE;
    }

    /* --- parse arguments ------------------------------------------------ */
    char *endptr;

    uintptr_t phys_addr = (uintptr_t)strtoull(argv[1], &endptr, 16);
    if (*endptr != '\0') {
        fprintf(stderr, "Error: invalid physical address '%s'\n", argv[1]);
        return EXIT_FAILURE;
    }

    int num_threads = DEFAULT_NUM_THREADS;
    if (argc >= 3) {
        num_threads = (int)strtol(argv[2], &endptr, 10);
        if (*endptr != '\0' || num_threads <= 0) {
            fprintf(stderr, "Error: invalid thread count '%s'\n", argv[2]);
            return EXIT_FAILURE;
        }
    }

    int iterations = DEFAULT_ITERATIONS;
    if (argc >= 4) {
        iterations = (int)strtol(argv[3], &endptr, 10);
        if (*endptr != '\0' || iterations <= 0) {
            fprintf(stderr, "Error: invalid iteration count '%s'\n", argv[3]);
            return EXIT_FAILURE;
        }
    }

    printf("=============================================================\n");
    printf("  Multi-Threaded atomic_fetch_add (4-byte) via /dev/mem\n");
    printf("=============================================================\n");
    printf("  Threads:     %d\n", num_threads);
    printf("  Phys addr:   0x%lx\n", phys_addr);
    printf("  Iterations:  %d / thread\n", iterations);
    printf("  Total ops:   %lu\n", (unsigned long)num_threads * iterations);
    printf("-------------------------------------------------------------\n");

    /* --- map /dev/mem -------------------------------------------------- */
    void  *mapped_base = NULL;
    size_t mapped_size = 0;

    if (devmem_map(phys_addr, sizeof(atomic_int32_t),
                   &mapped_base, &mapped_size) != 0) {
        return EXIT_FAILURE;
    }

    /* target pointer = mapped base + offset within the page */
    size_t offset = phys_addr & ((uintptr_t)sysconf(_SC_PAGESIZE) - 1);
    atomic_int32_t *target = (atomic_int32_t *)((uint8_t *)mapped_base + offset);

    int32_t initial = atomic_load(target);

    printf("  Target virt: %p\n", (void *)target);
    printf("  Initial value: %d\n", initial);
    printf("-------------------------------------------------------------\n");

    /* --- setup threads ------------------------------------------------ */
    pthread_t      *threads = malloc((size_t)num_threads * sizeof(pthread_t));
    thread_args_t  *args    = malloc((size_t)num_threads * sizeof(thread_args_t));
    volatile _Atomic int ready_flag = 0;

    if (!threads || !args) {
        fprintf(stderr, "Error: malloc failed\n");
        free(threads);
        free(args);
        munmap(mapped_base, mapped_size);
        return EXIT_FAILURE;
    }

    barrier_init(num_threads + 1);   /* +1 for main */

    for (int i = 0; i < num_threads; i++) {
        args[i].thread_id   = i;
        args[i].iterations  = iterations;
        args[i].target      = target;
        args[i].ready_flag  = &ready_flag;
    }

    /* --- launch threads & time ---------------------------------------- */
    for (int i = 0; i < num_threads; i++) {
        if (pthread_create(&threads[i], NULL, worker, &args[i]) != 0) {
            fprintf(stderr, "Error: pthread_create failed for thread %d\n", i);
            free(threads);
            free(args);
            munmap(mapped_base, mapped_size);
            return EXIT_FAILURE;
        }
    }

    barrier_wait();   /* release all threads simultaneously */
    uint64_t t0 = get_time_ns();

    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    uint64_t t1 = get_time_ns();

    /* --- report -------------------------------------------------------- */
    int32_t final       = atomic_load(target);
    double  elapsed_ms  = (double)(t1 - t0) / 1000000.0;
    double  ops_per_sec = (double)(num_threads * iterations) /
                          ((double)(t1 - t0) / 1e9);
    double  ns_per_op   = (double)(t1 - t0) / (double)(num_threads * iterations);
    int     expected     = initial + num_threads * iterations;

    printf("  Final value: %d (expected %d) %s\n",
           final, expected,
           (final == expected) ? "[OK]" : "[MISMATCH]");
    printf("  Elapsed:     %.2f ms\n", elapsed_ms);
    printf("  Ops/sec:     %.2f M\n", ops_per_sec / 1e6);
    printf("  ns/op:       %.2f ns\n", ns_per_op);
    printf("=============================================================\n");

    /* --- cleanup ------------------------------------------------------- */
    free(threads);
    free(args);
    munmap(mapped_base, mapped_size);

    return (final == expected) ? EXIT_SUCCESS : EXIT_FAILURE;
}