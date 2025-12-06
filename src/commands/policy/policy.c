/**
 * policy.c - Policy command group for viewing and managing rulr rules
 *
 * This command provides utilities for working with rulr (Datalog) policy rules,
 * such as viewing rule source code or compiled rules in canonical format.
 */

#include "policy.h"
#include "../../alloc.h"
#include "../../global_context.h"
#include "../../fileutil.h"
#include "../../rulr/rulr_dl.h"
#include "../../rulr/builtin_rules.h"
#include "../../rulr/frontend/ast.h"
#include "../../rulr/frontend/ast_serialize.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* ============================================================================
 * Usage
 * ========================================================================== */

static void print_policy_usage(void) {
    printf("Usage: %s policy SUBCOMMAND [OPTIONS]\n", global_context_program_name());
    printf("\n");
    printf("Manage and view policy rules.\n");
    printf("\n");
    printf("Subcommands:\n");
    printf("  view RULE          Print rule source to stdout\n");
    printf("  built-in           List all built-in rules\n");
    printf("\n");
    printf("Options:\n");
    printf("  -h, --help         Show this help message\n");
}

static void print_view_usage(void) {
    printf("Usage: %s policy view RULE\n", global_context_program_name());
    printf("\n");
    printf("Print the source of a rule to stdout.\n");
    printf("\n");
    printf("Arguments:\n");
    printf("  RULE               Rule name or path (without extension)\n");
    printf("                     For simple names (no path), looks in built-in rules first\n");
    printf("                     Tries .dlc (compiled) first, falls back to .dl (source)\n");
    printf("                     Can also specify with extension to use exact path\n");
    printf("\n");
    printf("For source (.dl) files, prints the file contents as-is.\n");
    printf("For compiled (.dlc) files, prints in canonical pretty-printed format.\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s policy view no_unused_dependencies\n", global_context_program_name());
    printf("  %s policy view rulr/rules/core_package_files\n", global_context_program_name());
    printf("  %s policy view rulr/rules/core_package_files.dl\n", global_context_program_name());
}

/* ============================================================================
 * Utility functions
 * ========================================================================== */

static int has_extension(const char *path, const char *ext) {
    size_t path_len = strlen(path);
    size_t ext_len = strlen(ext);
    if (ext_len > path_len) return 0;
    return strcmp(path + path_len - ext_len, ext) == 0;
}

/**
 * Check if a name contains a path separator (/ or \).
 */
static int has_path_separator(const char *name) {
    for (const char *p = name; *p; p++) {
        if (*p == '/' || *p == '\\') {
            return 1;
        }
    }
    return 0;
}

/**
 * Print a source (.dl) file to stdout.
 */
static int print_source_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "Error: Failed to open file: %s\n", path);
        return 1;
    }
    
    char buffer[4096];
    size_t n;
    while ((n = fread(buffer, 1, sizeof(buffer), f)) > 0) {
        fwrite(buffer, 1, n, stdout);
    }
    
    fclose(f);
    return 0;
}

/**
 * Print a compiled (.dlc) file in canonical pretty-printed format.
 */
static int print_compiled_file(const char *path) {
    AstProgram ast;
    ast_program_init(&ast);
    
    AstSerializeError err = ast_deserialize_from_file(path, &ast);
    if (err.is_error) {
        fprintf(stderr, "Error: Failed to read %s: %s\n", path, err.message);
        return 1;
    }
    
    /* Extract rule name from path (basename without extension) */
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    size_t name_len = strlen(base);
    if (name_len > 4 && strcmp(base + name_len - 4, ".dlc") == 0) {
        name_len -= 4;
    }
    printf("%% %.*s\n\n", (int)name_len, base);
    
    ast_pretty_print(&ast);
    
    return 0;
}

/**
 * Print a compiled rule from memory.
 */
static int print_compiled_from_memory(const char *name, const void *data, size_t size) {
    AstProgram ast;
    ast_program_init(&ast);
    
    AstSerializeError err = ast_deserialize_from_memory(data, size, &ast);
    if (err.is_error) {
        fprintf(stderr, "Error: Failed to parse built-in rule %s: %s\n", name, err.message);
        return 1;
    }
    
    printf("%% %s (built-in)\n\n", name);
    
    ast_pretty_print(&ast);
    
    return 0;
}

/* ============================================================================
 * View subcommand implementation
 * ========================================================================== */

int cmd_policy_view(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Error: view command requires a rule name or path\n\n");
        print_view_usage();
        return 1;
    }
    
    /* Check for help */
    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        print_view_usage();
        return 0;
    }
    
    const char *name = argv[1];
    
    /* If name already has an extension, use it directly */
    if (has_extension(name, RULR_SOURCE_EXT)) {
        if (!file_exists(name)) {
            fprintf(stderr, "Error: File not found: %s\n", name);
            return 1;
        }
        return print_source_file(name);
    }
    
    if (has_extension(name, RULR_COMPILED_EXT)) {
        if (!file_exists(name)) {
            fprintf(stderr, "Error: File not found: %s\n", name);
            return 1;
        }
        return print_compiled_file(name);
    }
    
    /*
     * For simple names (no path separators), first check built-in rules.
     */
    if (!has_path_separator(name) && builtin_rules_available()) {
        void *data = NULL;
        size_t size = 0;
        if (builtin_rules_extract(name, &data, &size)) {
            int result = print_compiled_from_memory(name, data, size);
            arena_free(data);
            return result;
        }
    }
    
    /* Build paths with extensions */
    size_t name_len = strlen(name);
    size_t src_ext_len = strlen(RULR_SOURCE_EXT);
    size_t cmp_ext_len = strlen(RULR_COMPILED_EXT);
    
    char *compiled_path = arena_malloc(name_len + cmp_ext_len + 1);
    char *source_path = arena_malloc(name_len + src_ext_len + 1);
    
    if (!compiled_path || !source_path) {
        fprintf(stderr, "Error: Out of memory\n");
        return 1;
    }
    
    snprintf(compiled_path, name_len + cmp_ext_len + 1, "%s%s", name, RULR_COMPILED_EXT);
    snprintf(source_path, name_len + src_ext_len + 1, "%s%s", name, RULR_SOURCE_EXT);
    
    /* Try compiled file first */
    if (file_exists(compiled_path)) {
        return print_compiled_file(compiled_path);
    }
    
    /* Fall back to source file */
    if (file_exists(source_path)) {
        return print_source_file(source_path);
    }
    
    /* Neither exists - print error */
    fprintf(stderr, "Error: Rule file not found: %s (tried %s and %s)\n", 
            name, compiled_path, source_path);
    return 1;
}

/* ============================================================================
 * Built-in subcommand implementation
 * ========================================================================== */

static void print_builtin_usage(void) {
    printf("Usage: %s policy built-in\n", global_context_program_name());
    printf("\n");
    printf("List all built-in rules embedded in the binary.\n");
    printf("\n");
    printf("Built-in rules can be used by name without specifying a path.\n");
}

int cmd_policy_builtin(int argc, char *argv[]) {
    /* Check for help */
    if (argc >= 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        print_builtin_usage();
        return 0;
    }
    
    if (!builtin_rules_available()) {
        printf("No built-in rules available.\n");
        printf("(This binary was built without embedded rules.)\n");
        return 0;
    }
    
    int count = builtin_rules_count();
    if (count == 0) {
        printf("No built-in rules available.\n");
        return 0;
    }
    
    printf("Built-in rules (%d):\n", count);
    for (int i = 0; i < count; i++) {
        const char *name = builtin_rules_name(i);
        if (name) {
            printf("  %s\n", name);
        }
    }
    
    return 0;
}

/* ============================================================================
 * Main entry point
 * ========================================================================== */

int cmd_policy(int argc, char *argv[]) {
    if (argc < 2) {
        print_policy_usage();
        return 1;
    }
    
    const char *subcmd = argv[1];
    
    if (strcmp(subcmd, "-h") == 0 || strcmp(subcmd, "--help") == 0) {
        print_policy_usage();
        return 0;
    }
    
    if (strcmp(subcmd, "view") == 0) {
        return cmd_policy_view(argc - 1, argv + 1);
    }
    
    if (strcmp(subcmd, "built-in") == 0) {
        return cmd_policy_builtin(argc - 1, argv + 1);
    }
    
    fprintf(stderr, "Error: Unknown policy subcommand '%s'\n", subcmd);
    fprintf(stderr, "Run '%s policy --help' for usage information.\n", global_context_program_name());
    return 1;
}
