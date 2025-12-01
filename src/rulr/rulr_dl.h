#ifndef RULR_DL_H
#define RULR_DL_H

#include "rulr.h"

/* File extensions for rule files - define in one place for easy changes */
#define RULR_SOURCE_EXT ".dl"
#define RULR_COMPILED_EXT ".dlc"

/**
 * Load a source (.dl) rule file.
 */
RulrError rulr_load_dl_file(Rulr *r, const char *path);

/**
 * Load rule and fact files (both source format).
 */
RulrError rulr_load_dl_files(Rulr *r, const char *rule_path, const char *fact_path);

/**
 * Load a compiled (.dlc) rule file.
 */
RulrError rulr_load_compiled_file(Rulr *r, const char *path);

/**
 * Load a rule file by name (without extension).
 * Tries compiled (.dlc) first, falls back to source (.dl).
 * 
 * The name can be:
 * - A base name without extension: "no_unused_dependencies"  
 * - A path without extension: "rulr/rules/core_package_files"
 * - A path with extension: "rulr/rules/core_package_files.dl" (uses exact path)
 * 
 * Returns an error if neither compiled nor source file exists.
 */
RulrError rulr_load_rule_file(Rulr *r, const char *name);

#endif /* RULR_DL_H */
