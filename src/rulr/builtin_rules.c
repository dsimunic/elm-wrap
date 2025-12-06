/**
 * builtin_rules.c - Built-in rules embedded in the command binary
 *
 * Reads pre-compiled rulr rules from a zip archive appended to the executable.
 * Uses miniz to read the zip archive from the tail of the binary.
 */

#include "builtin_rules.h"
#include "alloc.h"
#include "../embedded_archive.h"

#include <stdio.h>
#include <string.h>

/* Static state for the built-in rules subsystem */
static struct {
    bool initialized;
    bool available;
    
    /* Cached rule names (without .dlc extension) */
    int rule_count;
    char **rule_names;
} g_builtin = {0};

bool builtin_rules_init(const char *exe_path) {
    if (g_builtin.initialized) {
        return g_builtin.available;
    }
    
    g_builtin.initialized = true;
    g_builtin.available = false;
    
    if (!embedded_archive_init(exe_path)) {
        return false;
    }
    
    if (!embedded_archive_available()) {
        return false;
    }
    
    g_builtin.available = true;
    
    mz_uint num_files = embedded_archive_file_count();
    
    /* Allocate space for rule names */
    int names_capacity = 16;
    g_builtin.rule_names = arena_malloc(names_capacity * sizeof(char*));
    g_builtin.rule_count = 0;
    
    if (!g_builtin.rule_names) {
        embedded_archive_cleanup();
        g_builtin.available = false;
        return false;
    }
    
    for (mz_uint i = 0; i < num_files; i++) {
        mz_zip_archive_file_stat stat;
        if (!embedded_archive_file_stat(i, &stat)) {
            continue;
        }
        
        /* Skip directories */
        if (embedded_archive_is_directory(i)) {
            continue;
        }
        
        /* Only include .dlc files */
        size_t name_len = strlen(stat.m_filename);
        if (name_len < 4 || strcmp(stat.m_filename + name_len - 4, ".dlc") != 0) {
            continue;
        }
        
        /* Grow array if needed */
        if (g_builtin.rule_count >= names_capacity) {
            names_capacity *= 2;
            g_builtin.rule_names = arena_realloc(g_builtin.rule_names,
                                                  names_capacity * sizeof(char*));
            if (!g_builtin.rule_names) {
                embedded_archive_cleanup();
                g_builtin.available = false;
                return false;
            }
        }
        
        /* Store name without .dlc extension */
        char *name = arena_malloc(name_len - 4 + 1);
        if (!name) {
            continue;
        }
        memcpy(name, stat.m_filename, name_len - 4);
        name[name_len - 4] = '\0';
        
        g_builtin.rule_names[g_builtin.rule_count++] = name;
    }
    
    return true;
}

bool builtin_rules_available(void) {
    return g_builtin.available;
}

bool builtin_rules_has(const char *name) {
    if (!g_builtin.available || !name) {
        return false;
    }
    
    /* Build the filename with .dlc extension */
    size_t name_len = strlen(name);
    char *filename = arena_malloc(name_len + 5);
    if (!filename) {
        return false;
    }
    snprintf(filename, name_len + 5, "%s.dlc", name);
    
    int index = embedded_archive_locate(filename);
    arena_free(filename);
    
    return index >= 0;
}

bool builtin_rules_extract(const char *name, void **data, size_t *size) {
    if (!g_builtin.available || !name || !data || !size) {
        return false;
    }
    
    /* Build the filename with .dlc extension */
    size_t name_len = strlen(name);
    char *filename = arena_malloc(name_len + 5);
    if (!filename) {
        return false;
    }
    snprintf(filename, name_len + 5, "%s.dlc", name);
    
    /* Find the file in the archive */
    int index = embedded_archive_locate(filename);
    if (index < 0) {
        arena_free(filename);
        return false;
    }
    
    bool ok = embedded_archive_extract(filename, data, size);
    arena_free(filename);
    return ok;
}

int builtin_rules_count(void) {
    if (!g_builtin.available) {
        return 0;
    }
    return g_builtin.rule_count;
}

const char *builtin_rules_name(int index) {
    if (!g_builtin.available || index < 0 || index >= g_builtin.rule_count) {
        return NULL;
    }
    return g_builtin.rule_names[index];
}

void builtin_rules_cleanup(void) {
    g_builtin.initialized = false;
    g_builtin.available = false;
    g_builtin.rule_count = 0;
    g_builtin.rule_names = NULL;
}
