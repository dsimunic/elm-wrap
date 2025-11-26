#ifndef ELM_COMPILER_H
#define ELM_COMPILER_H

/* Find and get the path to the Elm compiler binary
 * Searches in this order:
 * 1. ELM_WRAP_ELM_COMPILER_PATH environment variable
 * 2. Search for 'elm' in PATH
 * Returns: heap-allocated string with compiler path, or NULL if not found
 */
char* elm_compiler_get_path(void);

/* Get Elm compiler version string by running compiler with --version
 * Returns: heap-allocated version string (e.g., "0.19.1"), or NULL if failed
 * Note: Only returns version if output matches pattern \d+.\d+.\d+
 */
char* elm_compiler_get_version(void);

#endif /* ELM_COMPILER_H */
