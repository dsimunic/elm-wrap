/**
 * package_list.h - Unified package list printing and sorting
 *
 * This module provides consistent formatting and sorting for package lists
 * across all commands (info, build check, upgrade, install, etc.).
 *
 * All packages are sorted by author/name alphabetically.
 *
 * When to use this module:
 * - Simple package lists without version change arrows (use print functions)
 * - Any place needing sorted package names (use comparison functions)
 *
 * For complex formatting (upgrade arrows, color coding, constraint display),
 * you may need custom printing, but use the comparison functions for sorting.
 */

#ifndef PACKAGE_LIST_H
#define PACKAGE_LIST_H

#include <stddef.h>

/**
 * A lightweight entry for package list printing.
 * Can be constructed from various source types (Package, PackageChange, etc.).
 */
typedef struct {
    const char *author;       /* Package author (e.g., "elm") */
    const char *name;         /* Package name (e.g., "core") */
    const char *version;      /* Version string (e.g., "1.0.5") or NULL */
    const char *annotation;   /* Optional annotation (e.g., " (indirect)") or NULL */
} PackageListEntry;

/**
 * Comparison function for sorting PackageListEntry arrays.
 * Sorts by author first, then by name (case-sensitive).
 *
 * Use with qsort():
 *   qsort(entries, count, sizeof(PackageListEntry), package_list_compare);
 */
int package_list_compare(const void *a, const void *b);

/**
 * Comparison function for sorting strings in "author/name" format.
 * Parses the slash-separated format and sorts by author, then name.
 *
 * Use with qsort() on char* arrays:
 *   qsort(names, count, sizeof(char*), package_name_compare);
 */
int package_name_compare(const void *a, const void *b);

/**
 * Calculate the maximum display width for package names in a list.
 * Returns the length of the longest "author/name" string.
 *
 * @param entries Array of package entries
 * @param count Number of entries
 * @return Maximum name width
 */
size_t package_list_max_name_width(const PackageListEntry *entries, int count);

/**
 * Print a simple package list with aligned versions.
 * Format: "  author/name    version[annotation]"
 *
 * @param entries Array of package entries (will NOT be modified)
 * @param count Number of entries
 * @param max_width Maximum name width for alignment (0 = auto-calculate)
 * @param indent Number of leading spaces (e.g., 2 for "  ")
 */
void package_list_print(const PackageListEntry *entries, int count,
                        size_t max_width, int indent);

/**
 * Print a sorted package list with aligned versions.
 * Creates a sorted copy internally, does not modify the input.
 * Format: "  author/name    version[annotation]"
 *
 * @param entries Array of package entries
 * @param count Number of entries
 * @param max_width Maximum name width for alignment (0 = auto-calculate)
 * @param indent Number of leading spaces
 */
void package_list_print_sorted(const PackageListEntry *entries, int count,
                               size_t max_width, int indent);

/**
 * Print a simple list of package names (author/name format only).
 * Sorted alphabetically.
 *
 * @param names Array of "author/name" strings
 * @param count Number of names
 * @param indent Number of leading spaces
 */
void package_names_print_sorted(const char **names, int count, int indent);

#endif /* PACKAGE_LIST_H */
