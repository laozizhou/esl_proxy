/*
 * log.h - Toggleable worker logging
 *
 * Compile-time: 
 *   - WORKER_LOG: set to 1 in conf.h to include worker log calls (file output)
 *   - MAIN_LOG: set to 1 in conf.h to include main thread log calls (screen output)
 * Runtime (WORKER_LOG only): set g_worker_log to 1, or export WORKER_LOG=1 in the environment.
 *
 * Log format: source,log_line,detail
 * Each log entry is CSV formatted for easy analysis.
 *
 * Thread-safe: worker logs are split into separate files per thread.
 * Each thread writes to its own file: <base_filename>_thread_<tid>.csv
 */

#ifndef LOG_H
#define LOG_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#define LOG_MAX_THREADS 16

#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <pthread.h>
#include <time.h>

#ifdef __APPLE__
#include <mach/mach_time.h>
#endif

#include "conf.h"

static inline uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/*
 * 高分辨率单调时钟，专供 ready->runnable 延迟 KPI。
 *
 * 为什么不直接用 get_time_ns()：CLOCK_MONOTONIC 在本机实测粒度是 1000 ns
 * （clock_getres 报 1000 ns，连续采样最小非零间隔也是 1000 ns），而这段
 * 调度延迟常在亚微秒级，会被量化成 0 或 1000，无法分辨。
 * mach_absolute_time 的 timebase 是 125/3，即 41.667 ns/tick，实测最小
 * 间隔 41 ns，够用。
 *
 * 刻意不改 get_time_ns() 本身：makespan 等既有指标都用它，换时钟会让
 * 新老数据不可比。
 */
static inline uint64_t get_time_ns_hires(void) {
#ifdef __APPLE__
    /* mach_timebase_info 结果恒定，首次调用后缓存；并发重复初始化无害。 */
    static mach_timebase_info_data_t tb;
    if (tb.denom == 0) {
        mach_timebase_info(&tb);
    }
    return mach_absolute_time() * (uint64_t)tb.numer / (uint64_t)tb.denom;
#else
    /* Linux 上 CLOCK_MONOTONIC 本身就是 ns 级，无需特殊处理 */
    return get_time_ns();
#endif
}

#if WORKER_LOG
extern int g_worker_log;
extern int g_log_output_mode;

void log_init(const char *base_filename);
void log_close(void);

// Internal function called by WORKER_LOGF macro
void log_write(const char *file, int line, const char *fmt, ...);

// Variadic macro helper - count arguments and dispatch to log_write
#define _LOG_WRITE_0(file, line, fmt) \
    log_write(file, line, fmt)
#define _LOG_WRITE_1(file, line, fmt, arg1) \
    log_write(file, line, fmt, arg1)
#define _LOG_WRITE_2(file, line, fmt, arg1, arg2) \
    log_write(file, line, fmt, arg1, arg2)
#define _LOG_WRITE_3(file, line, fmt, arg1, arg2, arg3) \
    log_write(file, line, fmt, arg1, arg2, arg3)
#define _LOG_WRITE_4(file, line, fmt, arg1, arg2, arg3, arg4) \
    log_write(file, line, fmt, arg1, arg2, arg3, arg4)
#define _LOG_WRITE_5(file, line, fmt, arg1, arg2, arg3, arg4, arg5) \
    log_write(file, line, fmt, arg1, arg2, arg3, arg4, arg5)
#define _LOG_WRITE_GET(_0, _1, _2, _3, _4, _5, NAME, ...) NAME

// WORKER_LOGF - worker thread logging macro
// Uses log_write() which outputs to pto._thread_<id>.csv
#define WORKER_LOGF(...)                                                  \
    do {                                                                   \
        if (g_worker_log) {                                                \
            _LOG_WRITE_GET(__VA_ARGS__, _LOG_WRITE_5, _LOG_WRITE_4,       \
                           _LOG_WRITE_3, _LOG_WRITE_2, _LOG_WRITE_1,       \
                           _LOG_WRITE_0)(__FILE__, __LINE__, __VA_ARGS__); \
        }                                                                  \
    } while (0)

#else
#define WORKER_LOGF(...) ((void)0)
#endif

#if MAIN_LOG
// Internal function called by MAIN_LOGF macro
void main_log_write(int line, const char *fmt, ...);

// Main thread logging function - count arguments and dispatch
#define _MAIN_LOG_WRITE_0(line, fmt) main_log_write(line, fmt)
#define _MAIN_LOG_WRITE_1(line, fmt, arg1) main_log_write(line, fmt, arg1)
#define _MAIN_LOG_WRITE_2(line, fmt, arg1, arg2) main_log_write(line, fmt, arg1, arg2)
#define _MAIN_LOG_WRITE_3(line, fmt, arg1, arg2, arg3) main_log_write(line, fmt, arg1, arg2, arg3)
#define _MAIN_LOG_WRITE_4(line, fmt, arg1, arg2, arg3, arg4) main_log_write(line, fmt, arg1, arg2, arg3, arg4)
#define _MAIN_LOG_WRITE_5(line, fmt, arg1, arg2, arg3, arg4, arg5) main_log_write(line, fmt, arg1, arg2, arg3, arg4, arg5)
#define _MAIN_LOG_WRITE_GET(_0, _1, _2, _3, _4, _5, NAME, ...) NAME

// MAIN_LOGF - main thread logging macro
// Uses main_log_write() which outputs to screen only
#define MAIN_LOGF(...)                                                     \
    do {                                                                   \
        _MAIN_LOG_WRITE_GET(__VA_ARGS__, _MAIN_LOG_WRITE_5,               \
                            _MAIN_LOG_WRITE_4, _MAIN_LOG_WRITE_3,           \
                            _MAIN_LOG_WRITE_2, _MAIN_LOG_WRITE_1,          \
                            _MAIN_LOG_WRITE_0)(__LINE__, __VA_ARGS__);     \
    } while (0)

#else
#define MAIN_LOGF(...) ((void)0)
#endif

#endif /* LOG_H */