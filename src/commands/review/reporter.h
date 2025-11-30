/**
 * reporter.h - Rulr result formatting and reporting utilities
 *
 * This module provides customizable formatters for Datalog query results.
 * It supports tree-based display for file paths, common prefix stripping,
 * and extensible predicate-specific formatting.
 *
 * Usage:
 *   ReporterConfig cfg = reporter_default_config();
 *   cfg.base_path = "/path/to/strip";
 *   reporter_print_file_tree(&cfg, files, count);
 */

#ifndef REVIEW_REPORTER_H
#define REVIEW_REPORTER_H

#include "../../rulr/rulr.h"
#include "../../rulr/runtime/runtime.h"

/* ============================================================================
 * Configuration
 * ========================================================================== */

typedef struct {
    const char *base_path;     /* Base path to strip from file paths (optional) */
    int         use_tree;      /* If true, display as tree; if false, flat list */
    int         use_color;     /* If true, use ANSI colors (NYI) */
    int         max_depth;     /* Max tree depth to show (-1 = unlimited) */
} ReporterConfig;

/**
 * Return a default reporter configuration.
 */
ReporterConfig reporter_default_config(void);

/* ============================================================================
 * File Path Utilities
 * ========================================================================== */

/**
 * Find the longest common directory prefix among an array of paths.
 * Returns arena-allocated string (caller should not free).
 * Returns NULL if paths is empty or no common prefix exists.
 */
char *reporter_find_common_prefix(const char **paths, int count);

/**
 * Strip a base path prefix from a path, returning the relative part.
 * Returns arena-allocated string.
 */
char *reporter_strip_prefix(const char *path, const char *base_path);

/* ============================================================================
 * Tree Printing
 * ========================================================================== */

/**
 * Print a list of file paths as a directory tree.
 * 
 * @param cfg     Reporter configuration
 * @param paths   Array of file paths
 * @param count   Number of paths
 */
void reporter_print_file_tree(const ReporterConfig *cfg, const char **paths, int count);

/**
 * Print a flat list of files with shortened paths.
 * 
 * @param cfg     Reporter configuration  
 * @param paths   Array of file paths
 * @param count   Number of paths
 */
void reporter_print_file_list(const ReporterConfig *cfg, const char **paths, int count);

/* ============================================================================
 * Predicate-specific formatters
 * ========================================================================== */

/**
 * Format and print the redundant_file relation as a tree.
 *
 * @param rulr          The Rulr context
 * @param view          The relation view for redundant_file
 * @param base_path     Package root path to strip from file paths
 */
void reporter_print_redundant_files(
    const Rulr *rulr,
    EngineRelationView view,
    const char *base_path
);

/**
 * Format and print an error relation.
 * Automatically detects file path arguments and formats them nicely.
 *
 * @param rulr          The Rulr context
 * @param view          The relation view for error
 * @param base_path     Optional base path to strip from file paths
 */
void reporter_print_errors(
    const Rulr *rulr,
    EngineRelationView view,
    const char *base_path
);

#endif /* REVIEW_REPORTER_H */
