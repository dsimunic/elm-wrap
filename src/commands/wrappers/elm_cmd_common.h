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
 * Download all packages listed in elm.json.
 *
 * For application projects, downloads direct, indirect, test-direct,
 * and test-indirect dependencies.
 *
 * For package projects, resolves version constraints and downloads
 * package dependencies and test dependencies.
 *
 * Returns 0 on success, non-zero on failure.
 */
int download_all_packages(ElmJson *elm_json, InstallEnv *env);

#endif /* ELM_CMD_COMMON_H */
