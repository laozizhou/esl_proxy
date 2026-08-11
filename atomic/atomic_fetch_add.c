/**
 * @file atomic_fetch_add.c
 * @brief 通过 /dev/mem 映射物理地址，多线程 atomic_fetch_add 测试
 *
 * 支持三种测试模式:
 *
 *   Mode 0 - 单地址竞争 (默认):
 *     所有线程 atomic_fetch_add 读写同一个 4B 地址，测试原子操作正确性
 *     及在真实地址竞争下的性能。
 *
 *   Mode 1 - 多线程并行写多条相同 cacheline:
 *     分配 num_entries 个条目，每个条目占 64B（cacheline 对齐/等宽）。
 *     所有线程遍历全部条目，对每个条目的计数器做 atomic_fetch_add。
 *     用于模拟多核同时竞争相同一批 cacheline 的场景。
 *
 *   Mode 2 - 功能验证:
 *     单线程：atomic_store(0)，再 atomic_fetch_add(+6)，打印返回值（应为0），
 *     再 atomic_load 验证最终值（应为6）。快速验证 atomic 指令正确性。
 *
 * Usage:
 *   ./atomic_fetch_add <physical_addr> [num_threads] [iterations] [mode] [num_entries]
 *
 *   physical_addr: 物理地址（十六进制），如 0x100000000
 *   num_threads:   线程数，默认 4
 *   iterations:    mode 0: ops per thread
 *                  mode 1: full-array passes per thread (默认 1000000)
 *   mode:          0 = 单地址竞争 (默认)
 *                  1 = 多条相同 cacheline 并行写
 *                  2 = 功能验证 (atomic_store(0) -> fetch_add(+6))
 *   num_entries:   mode=1 时的条目数，每个条目占 64B (默认 1024)
 *
 *   Mode 0: total_ops = num_threads * iterations
 *   Mode 1: total_ops = num_threads * iterations * num_entries
 *           (每个条目占 64B，4B 计数器 + 60B 填充)
 *   Mode 2: 不需要 num_threads/iterations/num_entries 参数
 *
 * Examples:
 *   sudo ./atomic_fetch_add 0x100000000
 *   sudo ./atomic_fetch_add 0x100000000 8 5000000
 *   sudo ./atomic_fetch_add 0x100000000 8 1000 1 1024
 *   sudo ./atomic_fetch_add 0x100000000 0 0 2
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

#define DEFAULT_NUM_THREADS   4
#define DEFAULT_ITERATIONS    1000000
#define DEFAULT_NUM_ENTRIES   1024
#define CACHELINE_SIZE        64     /* each entry occupies a full cacheline */

/* ============================================================================
 * Types
 * ============================================================================ */

/* 4-byte atomic type */
typedef _Atomic int32_t atomic_int32_t;

/**
 * Each entry in the cacheline array:
 *   - counter: 4-byte atomic (at offset 0)
 *   - padding: 60 bytes to fill the rest of the 64B cacheline
 */
typedef struct {
    atomic_int32_t counter;
    uint8_t        _pad[CACHELINE_SIZE - sizeof(atomic_int32_t)];
} __attribute__((aligned(CACHELINE_SIZE))) entry_t;

/* --- Mode 0: single-address competition --- */
typedef struct {
    int              thread_id;
    int              iterations;
    atomic_int32_t  *target;
    volatile _Atomic int *ready_flag;
} thread_args_t;

/* --- Mode 1: cacheline array --- */
typedef struct {
    int              thread_id;
    int              iterations;       /* number of full passes over the array  */
    int              num_entries;
    entry_t         *entries;          /* array of cacheline-padded entries     */
    volatile _Atomic int *ready_flag;
} cl_thread_args_t;

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
 * Worker threads
 * ============================================================================ */

/** Mode-0 worker: all threads hammer the same 4B address. */
static void *worker(void *arg) {
    thread_args_t *a = (thread_args_t *)arg;

    barrier_wait();

    for (int i = 0; i < a->iterations; i++) {
        atomic_fetch_add_explicit(a->target, 1, memory_order_relaxed);
    }

    atomic_store(a->ready_flag, a->thread_id + 1);

    return NULL;
}

/**
 * Mode-1 worker:
 *   Every thread iterates over the SAME set of entries (ntotal = num_entries).
 *   Outer loop: iterations rounds.
 *   Inner loop: for each entry[j], atomic_fetch_add(&entry[j].counter, 1).
 *   So total ops per thread = iterations * num_entries.
 */
static void *cacheline_worker(void *arg) {
    cl_thread_args_t *a = (cl_thread_args_t *)arg;

    barrier_wait();

    for (int r = 0; r < a->iterations; r++) {
        for (int j = 0; j < a->num_entries; j++) {
            atomic_fetch_add_explicit(&a->entries[j].counter, 1,
                                      memory_order_relaxed);
        }
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

    uintptr_t page_base = phys_addr & ~((uintptr_t)page_size - 1);
    size_t offset       = phys_addr - page_base;
    size_t map_len      = offset + size;

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
 * Mode 0: single-address contention test
 * ============================================================================ */

static int run_single_addr_test(uintptr_t phys_addr,
                                int num_threads, int iterations) {
    void  *mapped_base = NULL;
    size_t mapped_size = 0;

    if (devmem_map(phys_addr, sizeof(atomic_int32_t),
                   &mapped_base, &mapped_size) != 0) {
        return EXIT_FAILURE;
    }

    size_t offset = phys_addr & ((uintptr_t)sysconf(_SC_PAGESIZE) - 1);
    atomic_int32_t *target = (atomic_int32_t *)((uint8_t *)mapped_base + offset);

    int32_t initial = atomic_load(target);

    printf("  Target virt: %p\n", (void *)target);
    printf("  Initial value: %d\n", initial);
    printf("-------------------------------------------------------------\n");

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

    barrier_init(num_threads + 1);

    for (int i = 0; i < num_threads; i++) {
        args[i].thread_id   = i;
        args[i].iterations  = iterations;
        args[i].target      = target;
        args[i].ready_flag  = &ready_flag;
    }

    for (int i = 0; i < num_threads; i++) {
        if (pthread_create(&threads[i], NULL, worker, &args[i]) != 0) {
            fprintf(stderr, "Error: pthread_create failed for thread %d\n", i);
            free(threads);
            free(args);
            munmap(mapped_base, mapped_size);
            return EXIT_FAILURE;
        }
    }

    barrier_wait();
    uint64_t t0 = get_time_ns();

    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    uint64_t t1 = get_time_ns();

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

    free(threads);
    free(args);
    munmap(mapped_base, mapped_size);

    return (final == expected) ? EXIT_SUCCESS : EXIT_FAILURE;
}

/* ============================================================================
 * Mode 1: cacheline array test
 *
 * Every thread iterates over ALL num_entries entries (each 64B cacheline).
 * Each entry is a 4B atomic counter + 60B padding.
 * All threads compete for the same set of cachelines.
 * ============================================================================ */

static int run_cacheline_test(uintptr_t phys_addr,
                              int num_threads, int iterations,
                              int num_entries) {
    size_t total_bytes = (size_t)num_entries * sizeof(entry_t);

    void  *mapped_base = NULL;
    size_t mapped_size = 0;

    if (devmem_map(phys_addr, total_bytes,
                   &mapped_base, &mapped_size) != 0) {
        return EXIT_FAILURE;
    }

    size_t offset = phys_addr & ((uintptr_t)sysconf(_SC_PAGESIZE) - 1);
    entry_t *entries = (entry_t *)((uint8_t *)mapped_base + offset);

    printf("  Entries:     %d (each %zu bytes, cacheline-aligned)\n",
           num_entries, sizeof(entry_t));
    printf("  Total bytes: %zu\n", total_bytes);

    int32_t *initials = malloc((size_t)num_entries * sizeof(int32_t));
    if (!initials) {
        fprintf(stderr, "Error: malloc initials failed\n");
        munmap(mapped_base, mapped_size);
        return EXIT_FAILURE;
    }
    for (int j = 0; j < num_entries; j++) {
        initials[j] = atomic_load(&entries[j].counter);
    }
    printf("  Initial counter[0..3]: [%d, %d, %d, %d]\n",
           initials[0],
           num_entries > 1 ? initials[1] : 0,
           num_entries > 2 ? initials[2] : 0,
           num_entries > 3 ? initials[3] : 0);
    printf("-------------------------------------------------------------\n");

    pthread_t         *threads = malloc((size_t)num_threads * sizeof(pthread_t));
    cl_thread_args_t  *args    = malloc((size_t)num_threads * sizeof(cl_thread_args_t));
    volatile _Atomic int ready_flag = 0;

    if (!threads || !args) {
        fprintf(stderr, "Error: malloc failed\n");
        free(threads);
        free(args);
        free(initials);
        munmap(mapped_base, mapped_size);
        return EXIT_FAILURE;
    }

    barrier_init(num_threads + 1);

    for (int i = 0; i < num_threads; i++) {
        args[i].thread_id   = i;
        args[i].iterations  = iterations;
        args[i].num_entries = num_entries;
        args[i].entries     = entries;
        args[i].ready_flag  = &ready_flag;
    }

    for (int i = 0; i < num_threads; i++) {
        if (pthread_create(&threads[i], NULL, cacheline_worker, &args[i]) != 0) {
            fprintf(stderr, "Error: pthread_create failed for thread %d\n", i);
            free(threads);
            free(args);
            free(initials);
            munmap(mapped_base, mapped_size);
            return EXIT_FAILURE;
        }
    }

    barrier_wait();
    uint64_t t0 = get_time_ns();

    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    uint64_t t1 = get_time_ns();

    unsigned long total_ops = (unsigned long)num_threads * iterations * num_entries;
    double        elapsed_ms  = (double)(t1 - t0) / 1000000.0;
    double        ops_per_sec = (double)total_ops /
                                ((double)(t1 - t0) / 1e9);
    double        ns_per_op   = (double)(t1 - t0) / (double)total_ops;
    int           all_ok      = 1;

    printf("  Entry verification (first %d shown):\n",
           num_entries < 8 ? num_entries : 8);
    int show = num_entries < 8 ? num_entries : 8;
    for (int j = 0; j < num_entries; j++) {
        int32_t final    = atomic_load(&entries[j].counter);
        int     expected = initials[j] + num_threads * iterations;
        int     ok       = (final == expected);

        if (j < show) {
            printf("    entry[%3d]: final=%d expected=%d %s\n",
                   j, final, expected, ok ? "[OK]" : "[MISMATCH]");
        }
        if (!ok) all_ok = 0;
    }
    if (num_entries > show) {
        printf("    ... (remaining %d entries verified silently)\n",
               num_entries - show);
    }

    printf("  Elapsed:     %.2f ms\n", elapsed_ms);
    printf("  Total ops:   %lu\n", total_ops);
    printf("  Ops/sec:     %.2f M\n", ops_per_sec / 1e6);
    printf("  ns/op:       %.2f ns\n", ns_per_op);
    printf("  Overall:     %s\n", all_ok ? "[ALL OK]" : "[HAS MISMATCH]");
    printf("=============================================================\n");

    free(threads);
    free(args);
    free(initials);
    munmap(mapped_base, mapped_size);

    return all_ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

/* ============================================================================
 * Mode 2: functional test
 *
 * Single-threaded sanity check:
 *   1. atomic_store(target, 0)              -- initialise to 0
 *   2. prev = atomic_fetch_add(target, +6)  -- add 6, get old value
 *   3. print prev (expected: 0)
 *   4. final = atomic_load(target)           -- read final value
 *   5. verify final == 6
 * ============================================================================ */

static int run_func_test(uintptr_t phys_addr) {
    void  *mapped_base = NULL;
    size_t mapped_size = 0;

    if (devmem_map(phys_addr, sizeof(atomic_int32_t),
                   &mapped_base, &mapped_size) != 0) {
        return EXIT_FAILURE;
    }

    size_t offset = phys_addr & ((uintptr_t)sysconf(_SC_PAGESIZE) - 1);
    atomic_int32_t *target = (atomic_int32_t *)((uint8_t *)mapped_base + offset);

    printf("  Target virt: %p\n", (void *)target);
    printf("-------------------------------------------------------------\n");

    /* Step 1: initialise to 0 */
    printf("  [1] atomic_store(target, 0)\n");
    atomic_store_explicit(target, 0, memory_order_relaxed);

    /* Step 2: fetch_add +6 */
    printf("  [2] prev = atomic_fetch_add(target, +6)\n");
    int32_t prev = atomic_fetch_add_explicit(target, 6, memory_order_relaxed);

    /* Step 3: print return value */
    printf("  [3] Return value (old value) = %d  (expected 0)  %s\n",
           prev, (prev == 0) ? "[OK]" : "[MISMATCH]");

    /* Step 4: read final value */
    int32_t final = atomic_load_explicit(target, memory_order_relaxed);

    /* Step 5: verify */
    int32_t expected = 6;
    int     ok = (prev == 0 && final == expected);

    printf("  [4] Final value = %d  (expected %d)  %s\n",
           final, expected,
           (final == expected) ? "[OK]" : "[MISMATCH]");
    printf("=============================================================\n");
    printf("  Overall: %s\n", ok ? "[PASS]" : "[FAIL]");
    printf("=============================================================\n");

    munmap(mapped_base, mapped_size);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

/* ============================================================================
 * main
 * ============================================================================ */

int main(int argc, char *argv[]) {
    if (argc < 2 || argc > 6) {
        fprintf(stderr,
                "Usage: %s <physical_addr> [num_threads] [iterations] [mode] [num_entries]\n"
                "  physical_addr: hex physical address (e.g. 0x100000000)\n"
                "  num_threads:   thread count (default %d)\n"
                "  iterations:    mode 0: ops per thread\n"
                "                 mode 1: full-array passes per thread (default %d)\n"
                "  mode:          0 = single-address contention (default)\n"
                "                 1 = same cacheline array, all threads\n"
                "                 2 = functional test (store 0, fetch_add +6)\n"
                "  num_entries:   mode 1 array size, each entry 64B (default %d)\n"
                "\nExamples:\n"
                "  sudo %s 0x100000000\n"
                "  sudo %s 0x100000000 8 5000000\n"
                "  sudo %s 0x100000000 8 1000 1 1024\n"
                "  sudo %s 0x100000000 0 0 2\n",
                argv[0], DEFAULT_NUM_THREADS, DEFAULT_ITERATIONS,
                DEFAULT_NUM_ENTRIES,
                argv[0], argv[0], argv[0], argv[0]);
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

    int mode = 0;
    if (argc >= 5) {
        mode = (int)strtol(argv[4], &endptr, 10);
        if (*endptr != '\0' || mode < 0 || mode > 2) {
            fprintf(stderr, "Error: invalid mode '%s' (must be 0, 1, or 2)\n", argv[4]);
            return EXIT_FAILURE;
        }
    }

    int num_entries = DEFAULT_NUM_ENTRIES;
    if (argc >= 6) {
        num_entries = (int)strtol(argv[5], &endptr, 10);
        if (*endptr != '\0' || num_entries <= 0) {
            fprintf(stderr, "Error: invalid num_entries '%s'\n", argv[5]);
            return EXIT_FAILURE;
        }
    }

    const char *mode_desc[] = {
        "single-address contention (all threads -> same 4B)",
        "same cacheline array (all threads iterate over all entries)",
        "functional test (atomic_store(0) -> fetch_add(+6) -> print retval)",
    };

    printf("=============================================================\n");
    printf("  Multi-Threaded atomic_fetch_add via /dev/mem\n");
    printf("=============================================================\n");
    printf("  Mode:        %d (%s)\n", mode, mode_desc[mode]);

    if (mode == 2) {
        /* Functional test: no threads/iterations needed */
        printf("  Phys addr:   0x%lx\n", phys_addr);
        printf("-------------------------------------------------------------\n");
        return run_func_test(phys_addr);
    }

    printf("  Threads:     %d\n", num_threads);
    printf("  Phys addr:   0x%lx\n", phys_addr);
    printf("  Iterations:  %d / thread\n", iterations);

    unsigned long total_ops = 0;
    if (mode == 0) {
        total_ops = (unsigned long)num_threads * iterations;
    } else {
        total_ops = (unsigned long)num_threads * iterations * num_entries;
        printf("  Entries:     %d (each 64B cacheline)\n", num_entries);
    }
    printf("  Total ops:   %lu\n", total_ops);
    printf("-------------------------------------------------------------\n");

    if (mode == 0) {
        return run_single_addr_test(phys_addr, num_threads, iterations);
    } else {
        return run_cacheline_test(phys_addr, num_threads, iterations, num_entries);
    }
}