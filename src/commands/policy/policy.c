/**
 * policy.c - Policy command group for viewing and managing rulr rules
 *
 * This command provides utilities for working with rulr (Datalog) policy rules,
 * such as viewing rule source code or compiled rules in canonical format.
 */

#include "policy.h"
#include "../../alloc.h"
#include "../../progname.h"
#include "../../rulr/rulr_dl.h"
#include "../../rulr/frontend/ast.h"
#include "../../rulr/frontend/ast_serialize.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* ============================================================================
 * Usage
 * ========================================================================== */

static void print_policy_usage(void) {
    printf("Usage: %s policy SUBCOMMAND [OPTIONS]\n", program_name);
    printf("\n");
    printf("Manage and view rulr policy rules.\n");
    printf("\n");
    printf("Subcommands:\n");
    printf("  view <RULE>        Print rule source to stdout\n");
    printf("\n");
    printf("Options:\n");
    printf("  -h, --help         Show this help message\n");
}

static void print_view_usage(void) {
    printf("Usage: %s policy view <RULE>\n", program_name);
    printf("\n");
    printf("Print the source of a rulr rule to stdout.\n");
    printf("\n");
    printf("Arguments:\n");
    printf("  <RULE>             Rule name or path (without extension)\n");
    printf("                     Tries .dlc (compiled) first, falls back to .dl (source)\n");
    printf("                     Can also specify with extension to use exact path\n");
    printf("\n");
    printf("For source (.dl) files, prints the file contents as-is.\n");
    printf("For compiled (.dlc) files, prints in canonical pretty-printed format.\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s policy view no_unused_dependencies\n", program_name);
    printf("  %s policy view rulr/rules/core_package_files\n", program_name);
    printf("  %s policy view rulr/rules/core_package_files.dl\n", program_name);
}

/* ============================================================================
 * Utility functions
 * ========================================================================== */

static int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static int has_extension(const char *path, const char *ext) {
    size_t path_len = strlen(path);
    size_t ext_len = strlen(ext);
    if (ext_len > path_len) return 0;
    return strcmp(path + path_len - ext_len, ext) == 0;
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
    
    fprintf(stderr, "Error: Unknown policy subcommand '%s'\n", subcmd);
    fprintf(stderr, "Run '%s policy --help' for usage information.\n", program_name);
    return 1;
}
