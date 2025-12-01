/**
 * elm_project.h - Elm project file utilities
 *
 * This module provides utilities for working with Elm project files,
 * including elm.json parsing and source file collection.
 */

#ifndef ELM_PROJECT_H
#define ELM_PROJECT_H

/**
 * Parse exposed-modules from elm.json.
 * 
 * Handles both flat array format: ["Module1", "Module2"]
 * and categorized object format: { "Category": ["Module1", "Module2"], ... }
 *
 * @param elm_json_path Path to elm.json file
 * @param count Output parameter for number of modules found
 * @return Arena-allocated array of module names, or NULL on error
 */
char **elm_parse_exposed_modules(const char *elm_json_path, int *count);

/**
 * Parse source-directories from elm.json.
 *
 * @param elm_json_path Path to elm.json file
 * @param count Output parameter for number of directories found
 * @return Arena-allocated array of directory paths, or NULL on error
 */
char **elm_parse_source_directories(const char *elm_json_path, int *count);

/**
 * Convert an Elm module name to a file path.
 * 
 * E.g., "Html.Events" with src_dir="src" becomes "src/Html/Events.elm"
 *
 * @param module_name Module name with dot separators
 * @param src_dir Source directory path
 * @return Arena-allocated file path
 */
char *elm_module_name_to_path(const char *module_name, const char *src_dir);

/**
 * Recursively collect all .elm files in a directory.
 *
 * @param dir_path Directory to scan
 * @param files Pointer to array of file paths (will be reallocated as needed)
 * @param count Pointer to current count of files
 * @param capacity Pointer to current capacity of files array
 */
void elm_collect_elm_files(const char *dir_path, char ***files, int *count, int *capacity);

/**
 * Recursively collect all files in a directory (not just .elm).
 *
 * @param dir_path Directory to scan
 * @param files Pointer to array of file paths (will be reallocated as needed)
 * @param count Pointer to current count of files
 * @param capacity Pointer to current capacity of files array
 */
void elm_collect_all_files(const char *dir_path, char ***files, int *count, int *capacity);

/**
 * Check if a file path is in a list.
 *
 * @param file File path to search for
 * @param list Array of file paths
 * @param count Number of elements in list
 * @return 1 if found, 0 otherwise
 */
int elm_is_file_in_list(const char *file, char **list, int count);

#endif /* ELM_PROJECT_H */
