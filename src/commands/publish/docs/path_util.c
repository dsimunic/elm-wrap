/**
 * path_util.c - Shared path utilities for Elm documentation generation
 */

#include "path_util.h"
#include "../../../alloc.h"
#include <string.h>

char *module_name_to_file_path(const char *module_name) {
    size_t len = strlen(module_name);
    char *path = arena_malloc(len + 5);  /* +4 for ".elm" + 1 for null */
    if (!path) return NULL;
    memcpy(path, module_name, len + 1);

    /* Replace dots with slashes */
    for (char *p = path; *p; p++) {
        if (*p == '.') {
            *p = '/';
        }
    }

    memcpy(path + len, ".elm", 5);
    return path;
}
