#include "elm_compiler.h"
#include "alloc.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <limits.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Search for elm binary in PATH */
static char* find_elm_binary_in_path(void) {
    const char *path_env = getenv("PATH");
    if (!path_env) {
        return NULL;
    }

    char *path_copy = arena_strdup(path_env);
    if (!path_copy) {
        return NULL;
    }

    char *dir = strtok(path_copy, ":");
    while (dir != NULL) {
        char *candidate = arena_malloc(PATH_MAX);
        if (!candidate) {
            return NULL;
        }

        snprintf(candidate, PATH_MAX, "%s/elm", dir);

        struct stat st;
        if (stat(candidate, &st) == 0 && (st.st_mode & S_IXUSR)) {
            return candidate;
        }

        dir = strtok(NULL, ":");
    }

    return NULL;
}

char* elm_compiler_get_path(void) {
    const char *compiler_path = getenv("WRAP_ELM_COMPILER_PATH");
    if (compiler_path && compiler_path[0] != '\0') {
        return arena_strdup(compiler_path);
    }

    return find_elm_binary_in_path();
}

char* elm_compiler_get_version(void) {
    char *compiler_path = elm_compiler_get_path();
    if (!compiler_path) {
        return NULL;
    }

    /* Build command: compiler --version */
    size_t cmd_len = strlen(compiler_path) + strlen(" --version 2>&1") + 1;
    char *cmd = arena_malloc(cmd_len);
    if (!cmd) {
        arena_free(compiler_path);
        return NULL;
    }
    snprintf(cmd, cmd_len, "%s --version 2>&1", compiler_path);
    arena_free(compiler_path);

    /* Execute and capture output */
    FILE *fp = popen(cmd, "r");
    arena_free(cmd);
    if (!fp) {
        return NULL;
    }

    char buffer[256];
    char *version = NULL;
    if (fgets(buffer, sizeof(buffer), fp) != NULL) {
        /* Remove trailing newline */
        size_t len = strlen(buffer);
        if (len > 0 && buffer[len - 1] == '\n') {
            buffer[len - 1] = '\0';
            len--;
        }

        /* Check if it matches version pattern: X.Y.Z */
        int major, minor, patch;
        if (sscanf(buffer, "%d.%d.%d", &major, &minor, &patch) == 3) {
            /* Valid version format */
            version = arena_strdup(buffer);
        }
    }

    pclose(fp);
    return version;
}
