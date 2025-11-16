#include "log.h"

/* Global log level - defaults to ERROR only */
LogLevel g_log_level = LOG_LEVEL_ERROR;

void log_init(bool verbose) {
    if (verbose) {
        g_log_level = LOG_LEVEL_DEBUG;
    } else {
        g_log_level = LOG_LEVEL_ERROR;
    }
}

void log_set_level(LogLevel level) {
    g_log_level = level;
}
