#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#include <stdbool.h>

/* Log levels */
typedef enum {
    LOG_LEVEL_ERROR = 0,
    LOG_LEVEL_WARN  = 1,
    LOG_LEVEL_PROGRESS = 2,
    LOG_LEVEL_DEBUG = 3
} LogLevel;

/* Global log level - default to ERROR only */
extern LogLevel g_log_level;

/* Initialize logging with verbose flag */
void log_init(bool verbose);

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

#define log_debug(fmt, ...) \
    do { \
        if (g_log_level >= LOG_LEVEL_DEBUG) { \
            fprintf(stderr, "[DEBUG] " fmt "\n", ##__VA_ARGS__); \
        } \
    } while (0)

/* Helper to check if debug is enabled */
static inline bool log_is_debug(void) {
    return g_log_level >= LOG_LEVEL_DEBUG;
}

static inline bool log_is_verbose(void) {
    return g_log_level >= LOG_LEVEL_WARN;
}

static inline bool log_is_progress(void) {
    return g_log_level >= LOG_LEVEL_PROGRESS;
}

#endif /* LOG_H */
