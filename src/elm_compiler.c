#include "elm_compiler.h"
#include "alloc.h"
#include "constants.h"
#include "commands/package/package_common.h"
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

#ifdef _WIN32
    const char *sep = ";";
    const char *const names[] = { "elm.exe", "elm", NULL };
#else
    const char *sep = ":";
    const char *const names[] = { "elm", NULL };
#endif

    char *dir = strtok(path_copy, sep);
    while (dir != NULL) {
        for (int i = 0; names[i] != NULL; i++) {
            char *candidate = arena_malloc(PATH_MAX);
            if (!candidate) {
                return NULL;
            }

            snprintf(candidate, PATH_MAX, "%s/%s", dir, names[i]);

            struct stat st;
            if (stat(candidate, &st) == 0
#ifdef _WIN32
                && S_ISREG(st.st_mode)   /* Windows has no execute bit */
#else
                && (st.st_mode & S_IXUSR)
#endif
               ) {
                return candidate;
            }
            arena_free(candidate);
        }

        dir = strtok(NULL, sep);
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

    char buffer[MAX_PACKAGE_NAME_LENGTH];
    char *version = NULL;
    if (fgets(buffer, sizeof(buffer), fp) != NULL) {
        /* Remove trailing newline */
        size_t len = strlen(buffer);
        if (len > 0 && buffer[len - 1] == '\n') {
            buffer[len - 1] = '\0';
            len--;
        }

        /* Check if it matches version pattern: X.Y.Z */
        Version parsed_v;
        if (version_parse_safe(buffer, &parsed_v)) {
            /* Valid version format */
            version = arena_strdup(buffer);
        }
    }

    pclose(fp);
    return version;
}
