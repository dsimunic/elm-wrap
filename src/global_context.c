/**
 * global_context.c - Global context management
 *
 * This module provides global state that is determined at program startup
 * and influences how commands operate throughout the program's lifetime.
 * 
 * The primary purpose is to detect whether we should use V1 (legacy Elm)
 * or V2 (elm-wrap repository) protocol for package management.
 */

#include "global_context.h"
#include "alloc.h"
#include "env_defaults.h"
#include "elm_compiler.h"
#include "buildinfo.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <libgen.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Global singleton instance */
static GlobalContext *g_context = NULL;

/**
 * Determine the compiler type from the compiler name.
 */
static CompilerType determine_compiler_type(const char *compiler_name) {
    if (!compiler_name) {
        return COMPILER_UNKNOWN;
    }
    if (strcmp(compiler_name, "elm") == 0) {
        return COMPILER_ELM;
    }
    if (strcmp(compiler_name, "lamdera") == 0) {
        return COMPILER_LAMDERA;
    }
    if (strcmp(compiler_name, "wrapc") == 0) {
        return COMPILER_WRAPC;
    }
    return COMPILER_UNKNOWN;
}

/**
 * Get the compiler name from the compiler path.
 * Extracts the basename of the compiler path.
 * Returns "elm" if no custom path is set.
 */
static char *get_compiler_name(void) {
    const char *compiler_path = getenv("WRAP_ELM_COMPILER_PATH");
    if (compiler_path && compiler_path[0] != '\0') {
        /* Make a copy to use basename */
        char *path_copy = arena_strdup(compiler_path);
        if (!path_copy) {
            return arena_strdup("elm");
        }
        char *base = basename(path_copy);
        char *result = arena_strdup(base);
        arena_free(path_copy);
        return result;
    }
    return arena_strdup("elm");
}

/**
 * Check if a directory exists.
 */
static bool directory_exists(const char *path) {
    if (!path || path[0] == '\0') {
        return false;
    }
    
    struct stat st;
    if (stat(path, &st) != 0) {
        return false;
    }
    
    return S_ISDIR(st.st_mode);
}

/**
 * Build the repository path for the given compiler and version.
 * Returns NULL if any component is missing.
 */
static char *build_repository_path(const char *root_path, const char *compiler, const char *version) {
    if (!root_path || !compiler || !version) {
        return NULL;
    }
    
    size_t path_len = strlen(root_path) + 1 + strlen(compiler) + 1 + strlen(version) + 1;
    char *repo_path = arena_malloc(path_len);
    if (!repo_path) {
        return NULL;
    }
    
    snprintf(repo_path, path_len, "%s/%s/%s", root_path, compiler, version);
    return repo_path;
}

GlobalContext *global_context_init(const char *argv0) {
    /* Return existing context if already initialized */
    if (g_context) {
        return g_context;
    }

    /* Allocate context */
    g_context = arena_calloc(1, sizeof(GlobalContext));
    if (!g_context) {
        log_error("Failed to allocate global context");
        return NULL;
    }

    /* Extract and store program name from argv[0] */
    if (argv0 && argv0[0] != '\0') {
        /* Make a copy to use basename (since basename may modify the string) */
        char *argv0_copy = arena_strdup(argv0);
        if (argv0_copy) {
            char *prog_name = basename(argv0_copy);
            g_context->program_name = arena_strdup(prog_name);
        } else {
            g_context->program_name = arena_strdup(build_program_name);
        }
    } else {
        g_context->program_name = arena_strdup(build_program_name);
    }

    /* Default to V1 mode */
    g_context->protocol_mode = PROTOCOL_V1;
    g_context->compiler_name = NULL;
    g_context->compiler_version = NULL;
    g_context->compiler_type = COMPILER_UNKNOWN;
    g_context->repository_path = NULL;
    
    /* Get repository root path */
    char *repo_root = env_get_repository_local_path();
    if (!repo_root || repo_root[0] == '\0') {
        log_debug("No repository root path configured, using V1 mode");
        return g_context;
    }
    
    /* Get compiler name */
    g_context->compiler_name = get_compiler_name();
    if (!g_context->compiler_name) {
        log_debug("Could not determine compiler name, using V1 mode");
        return g_context;
    }
    
    /* Determine compiler type */
    g_context->compiler_type = determine_compiler_type(g_context->compiler_name);
    
    /* Get compiler version */
    g_context->compiler_version = elm_compiler_get_version();
    if (!g_context->compiler_version) {
        log_debug("Could not determine compiler version, using V1 mode");
        return g_context;
    }
    
    /* Build and check repository path */
    char *repo_path = build_repository_path(repo_root, g_context->compiler_name, g_context->compiler_version);
    if (repo_path && directory_exists(repo_path)) {
        /* V2 repository exists! */
        g_context->protocol_mode = PROTOCOL_V2;
        g_context->repository_path = repo_path;
        log_debug("V2 repository detected: %s", repo_path);
    } else {
        log_debug("No V2 repository found at %s, using V1 mode", 
                  repo_path ? repo_path : "(null)");
        if (repo_path) {
            arena_free(repo_path);
        }
    }
    
    return g_context;
}

GlobalContext *global_context_get(void) {
    return g_context;
}

bool global_context_is_v2(void) {
    if (!g_context) {
        return false;
    }
    return g_context->protocol_mode == PROTOCOL_V2;
}

const char *global_context_mode_string(void) {
    if (!g_context) {
        return "V1";
    }
    return g_context->protocol_mode == PROTOCOL_V2 ? "V2" : "V1";
}

CompilerType global_context_compiler_type(void) {
    if (!g_context) {
        return COMPILER_UNKNOWN;
    }
    return g_context->compiler_type;
}

bool global_context_is_elm(void) {
    if (!g_context) {
        return true; /* Default to elm behavior */
    }
    return g_context->compiler_type == COMPILER_ELM || 
           g_context->compiler_type == COMPILER_UNKNOWN;
}

bool global_context_is_lamdera(void) {
    if (!g_context) {
        return false;
    }
    return g_context->compiler_type == COMPILER_LAMDERA;
}

bool global_context_is_wrapc(void) {
    if (!g_context) {
        return false;
    }
    return g_context->compiler_type == COMPILER_WRAPC;
}

const char *global_context_program_name(void) {
    if (!g_context || !g_context->program_name) {
        return build_program_name;
    }
    return g_context->program_name;
}
