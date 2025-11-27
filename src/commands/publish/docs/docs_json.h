#ifndef ELM_DOCS_JSON_H
#define ELM_DOCS_JSON_H

#include "elm_docs.h"

/**
 * docs_json.h - JSON output generation for Elm documentation
 *
 * Functions for printing documentation in Elm's docs.json format.
 */

/**
 * Print a JSON-escaped string to stdout.
 * Handles escaping of special characters (quotes, backslashes, control chars).
 */
void print_json_string_internal(const char *str);

/**
 * Print complete Elm documentation as JSON to stdout.
 * 
 * @param docs Array of ElmModuleDocs structures
 * @param docs_count Number of modules in the array
 */
void print_docs_json(ElmModuleDocs *docs, int docs_count);

#endif /* ELM_DOCS_JSON_H */
