/**
 * path_util.c - Shared path utilities for Elm documentation generation
 */

#include "path_util.h"
#include "../../../alloc.h"
#include <string.h>

char *module_name_to_file_path(const char *module_name) {
    size_t len = strlen(module_name);
    char *path = arena_malloc(len + 5);  /* +4 for ".elm" + 1 for null */
    strcpy(path, module_name);

    /* Replace dots with slashes */
    for (char *p = path; *p; p++) {
        if (*p == '.') {
            *p = '/';
        }
    }

    strcat(path, ".elm");
    return path;
}
