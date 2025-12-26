#ifndef RULR_DL_H
#define RULR_DL_H

#include "rulr.h"

/* File extensions for rule files */
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
 * Load a source file by name (adds .dl extension if needed).
 */
RulrError rulr_load_source_file(Rulr *r, const char *name);

/*
 * For loading compiled (.dlc) files, the host must:
 * 1. Read the .dlc file
 * 2. Check the magic header (AST_MAGIC)
 * 3. Decompress using miniz (see reference/ast_serialize_compress.c)
 * 4. Call ast_deserialize_raw() to get an AstProgram
 * 5. Call rulr_load_program_ast() to load into Rulr
 *
 * See reference/ for helper implementations.
 */

#endif /* RULR_DL_H */
