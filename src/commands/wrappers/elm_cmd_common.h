/**
 * elm_cmd_common.h - Common utilities for Elm command wrappers
 *
 * This module provides shared functionality used by multiple Elm command
 * wrappers (make, reactor, bump, repl, publish, diff) to avoid code duplication.
 */

#ifndef ELM_CMD_COMMON_H
#define ELM_CMD_COMMON_H

#include "../../elm_json.h"
#include "../../install_env.h"

/**
 * Build an environment array for running elm commands.
 * 
 * By default, adds https_proxy=http://1 to force elm into offline mode
 * (since we pre-download all packages). Set WRAP_ALLOW_ELM_ONLINE=1
 * to skip this and allow elm to access the network.
 *
 * Returns NULL if memory allocation fails.
 * The returned array is arena-allocated.
 */
char **build_elm_environment(void);

/**
 * Download all packages listed in elm.json and their transitive dependencies.
 *
 * For application projects, downloads direct, indirect, test-direct,
 * and test-indirect dependencies, plus all their transitive dependencies.
 *
 * For package projects, resolves version constraints and downloads
 * package dependencies and test dependencies, plus all their transitive
 * dependencies.
 *
 * Uses recursive dependency resolution to ensure all packages needed
 * for compilation are available in the cache.
 *
 * Returns 0 on success, non-zero on failure.
 */
int download_all_packages(ElmJson *elm_json, InstallEnv *env);

/**
 * Run a "silent" package build using `elm make --json`, capturing compiler stdout,
 * verifying success via exit code.
 *
 * This is used by commands like `wrap package prepublish` and `wrap package extract`
 * to quickly confirm a package compiles.
 *
 * - `project_dir_abs` must be an absolute path to the package directory.
 * - `elm_json_path_abs` must be an absolute path to that package's elm.json.
 * - `exposed_modules` is typically the exposed-modules list.
 *
 * If `clean_artifacts` is true, it may delete build artifacts (like `elm-stuff`) before
 * and after compilation.
 *
 * Returns true if the compiler exits successfully for all modules compiled.
 * On failure, `*out_compiler_stdout` (if provided) contains the last compiler
 * JSON output captured.
 */
bool elm_cmd_run_silent_package_build(
	const char *project_dir_abs,
	const char *elm_json_path_abs,
	char **exposed_modules,
	int exposed_count,
	bool clean_artifacts,
	char **out_compiler_stdout
);

/*
 * Given the Elm compiler JSON report (from `elm make --report=json`), return the
 * number of unique file paths present in `errors[].path`.
 * Returns 0 if parsing fails or no paths are present.
 */
int elm_cmd_count_compiler_error_files(const char *compiler_json);

/*
 * Parse the Elm compiler JSON report (from `elm make --report=json`) and return
 * a unique list of file paths from `errors[].path`.
 *
 * The returned array and strings are arena-allocated.
 * Returns the number of paths (0 on parse failure or none).
 */
int elm_cmd_get_compiler_error_paths(const char *compiler_json, char ***out_paths);

/*
 * Make an absolute path relative to `base_abs` (if it is under that directory).
 * Returns an arena-allocated string.
 */
char *elm_cmd_path_relative_to_base(const char *abs_path, const char *base_abs);

#endif /* ELM_CMD_COMMON_H */
