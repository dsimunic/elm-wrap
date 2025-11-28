/**
 * import_tree.h - Import dependency tree analysis for Elm packages
 *
 * Provides functions for building import dependency trees and detecting
 * redundant files in Elm packages.
 */

#ifndef IMPORT_TREE_H
#define IMPORT_TREE_H

#include <stdbool.h>

/**
 * Result of analyzing a package's import tree
 */
typedef struct {
    /* Files reachable from exposed modules */
    char **included_files;
    int included_count;
    int included_capacity;
    
    /* Files in src/ but not reachable from exposed modules */
    char **redundant_files;
    int redundant_count;
    int redundant_capacity;
    
    /* Total .elm files in src/ */
    int total_files;
    
    /* Package info */
    char *package_dir;
    char *src_dir;
    
    /* Exposed modules */
    char **exposed_modules;
    int exposed_count;
} ImportTreeAnalysis;

/**
 * Analyze a package directory and build import tree
 * 
 * @param package_dir Path to package directory (must contain elm.json)
 * @return ImportTreeAnalysis* or NULL on error
 */
ImportTreeAnalysis *import_tree_analyze(const char *package_dir);

/**
 * Free resources used by an ImportTreeAnalysis
 */
void import_tree_free(ImportTreeAnalysis *analysis);

/**
 * Check if a file path is in the included files list
 */
bool import_tree_is_included(ImportTreeAnalysis *analysis, const char *file_path);

/**
 * Print import tree to stdout (with tree formatting)
 * 
 * @param analysis The analysis result
 * @param show_external Whether to show external package imports
 */
void import_tree_print(ImportTreeAnalysis *analysis, bool show_external);

/**
 * Print just the redundant files summary
 */
void import_tree_print_redundant(ImportTreeAnalysis *analysis);

/**
 * Get count of redundant files
 */
int import_tree_redundant_count(ImportTreeAnalysis *analysis);

#endif /* IMPORT_TREE_H */
