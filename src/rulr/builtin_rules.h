/**
 * builtin_rules.h - Built-in rules embedded in the elm-wrap binary
 *
 * This module provides access to pre-compiled rulr rules that are embedded
 * in the elm-wrap binary as a zip archive appended to the executable.
 *
 * The zip archive contains .dlc (compiled Datalog) files that can be loaded
 * directly without needing external rule files.
 */

#ifndef BUILTIN_RULES_H
#define BUILTIN_RULES_H

#include <stdbool.h>
#include <stddef.h>

/**
 * Initialize the built-in rules subsystem.
 * Must be called before any other builtin_rules functions.
 * 
 * @param exe_path Path to the currently running executable
 * @return true if initialization succeeded (zip archive found and valid),
 *         false otherwise (no embedded rules, which is not an error)
 */
bool builtin_rules_init(const char *exe_path);

/**
 * Check if built-in rules are available.
 * 
 * @return true if the binary has embedded rules
 */
bool builtin_rules_available(void);

/**
 * Check if a rule with the given name exists in the built-in rules.
 * 
 * @param name Rule name (without .dlc extension)
 * @return true if the rule exists
 */
bool builtin_rules_has(const char *name);

/**
 * Extract a built-in rule to memory.
 * 
 * @param name Rule name (without .dlc extension)
 * @param data Output: pointer to the extracted data (caller must free with arena_free)
 * @param size Output: size of the extracted data
 * @return true if extraction succeeded, false otherwise
 */
bool builtin_rules_extract(const char *name, void **data, size_t *size);

/**
 * Get the number of built-in rules.
 * 
 * @return Number of rules, or 0 if not available
 */
int builtin_rules_count(void);

/**
 * Get the name of a built-in rule by index.
 * 
 * @param index Index of the rule (0 to count-1)
 * @return Rule name (without extension), or NULL if index is out of range
 */
const char *builtin_rules_name(int index);

/**
 * Clean up the built-in rules subsystem.
 */
void builtin_rules_cleanup(void);

#endif /* BUILTIN_RULES_H */
