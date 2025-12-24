#include "shared/log.h"

/* Global log level - defaults to ERROR only */
LogLevel g_log_level = LOG_LEVEL_ERROR;

void log_init(int verbosity) {
    if (verbosity >= 2) {
        g_log_level = LOG_LEVEL_TRACE;
    } else if (verbosity == 1) {
        g_log_level = LOG_LEVEL_DEBUG;
    } else {
        g_log_level = LOG_LEVEL_ERROR;
    }
}

void log_set_level(LogLevel level) {
    g_log_level = level;
}
