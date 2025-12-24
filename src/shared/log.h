#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#include <stdbool.h>

/* Log levels */
typedef enum {
    LOG_LEVEL_ERROR = 0,
    LOG_LEVEL_WARN  = 1,
    LOG_LEVEL_PROGRESS = 2,
    LOG_LEVEL_DEBUG = 3,
    LOG_LEVEL_TRACE = 4  /* Extra verbose debug (-vv) */
} LogLevel;

/* Global log level - default to ERROR only */
extern LogLevel g_log_level;

/* Initialize logging with verbosity level (0=quiet, 1=-v, 2=-vv) */
void log_init(int verbosity);

/* Set log level directly */
void log_set_level(LogLevel level);

/* Logging macros */
#define log_error(fmt, ...) \
    do { \
        if (g_log_level >= LOG_LEVEL_ERROR) { \
            fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__); \
        } \
    } while (0)

#define log_warn(fmt, ...) \
    do { \
        if (g_log_level >= LOG_LEVEL_WARN) { \
            fprintf(stderr, "[WARN] " fmt "\n", ##__VA_ARGS__); \
        } \
    } while (0)

#define log_progress(fmt, ...) \
    do { \
        if (g_log_level >= LOG_LEVEL_PROGRESS) { \
            fprintf(stderr, fmt "\n", ##__VA_ARGS__); \
        } \
    } while (0)

/*
 * In fast mode, debug/trace logging is compiled out entirely.
 * This removes all overhead from hot paths (VM, kernels, etc.).
 */
#ifdef FAST_MODE
    #define log_debug(fmt, ...) ((void)0)
    #define log_trace(fmt, ...) ((void)0)
#else
    #define log_debug(fmt, ...) \
        do { \
            if (g_log_level >= LOG_LEVEL_DEBUG) { \
                fprintf(stderr, "[DEBUG] " fmt "\n", ##__VA_ARGS__); \
                fflush(stderr); \
            } \
        } while (0)

    #define log_trace(fmt, ...) \
        do { \
            if (g_log_level >= LOG_LEVEL_TRACE) { \
                fprintf(stderr, "[TRACE] " fmt "\n", ##__VA_ARGS__); \
                fflush(stderr); \
            } \
        } while (0)
#endif

/* Helper to check if debug is enabled */
#ifdef FAST_MODE
    static inline bool log_is_debug(void) { return false; }
    static inline bool log_is_trace(void) { return false; }
#else
    static inline bool log_is_debug(void) {
        return g_log_level >= LOG_LEVEL_DEBUG;
    }
    static inline bool log_is_trace(void) {
        return g_log_level >= LOG_LEVEL_TRACE;
    }
#endif

/*
 * In fast mode, many position-tracking variables are only used in log_debug calls.
 * Mark them as potentially unused to avoid compiler warnings.
 */
#ifdef FAST_MODE
    #define DBGV_UNUSED __attribute__((unused))
#else
    #define DBGV_UNUSED
#endif

static inline bool log_is_verbose(void) {
    return g_log_level >= LOG_LEVEL_WARN;
}

static inline bool log_is_progress(void) {
    return g_log_level >= LOG_LEVEL_PROGRESS;
}

#endif /* LOG_H */
