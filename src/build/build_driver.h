/**
 * build_driver.h - Build driver public API
 *
 * The build driver generates a JSON build plan for Elm compilation.
 */

#ifndef BUILD_DRIVER_H
#define BUILD_DRIVER_H

#include "build_types.h"
#include "../elm_json.h"
#include "../install_env.h"

/**
 * Generate a complete build plan for an Elm project.
 *
 * @param project_root Absolute path to the project root directory
 * @param elm_json Parsed elm.json structure
 * @param env Installation environment (for registry/solver access)
 * @param entry_files Array of entry point file paths
 * @param entry_count Number of entry point files
 * @return BuildPlan structure (arena-allocated), or NULL on failure
 */
BuildPlan *build_generate_plan(
    const char *project_root,
    ElmJson *elm_json,
    InstallEnv *env,
    const char **entry_files,
    int entry_count
);

/**
 * Convert a build plan to JSON string.
 *
 * @param plan The build plan to serialize
 * @return JSON string (arena-allocated)
 */
char *build_plan_to_json(BuildPlan *plan);

/**
 * Free a build plan and all its contents.
 * Note: With arena allocator, this may be a no-op.
 *
 * @param plan The build plan to free
 */
void build_plan_free(BuildPlan *plan);

/**
 * Add a problem to the build plan.
 *
 * @param plan The build plan
 * @param module_name Optional module name (can be NULL)
 * @param message Error message
 */
void build_add_problem(BuildPlan *plan, const char *module_name, const char *message);

#endif /* BUILD_DRIVER_H */
