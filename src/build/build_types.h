/**
 * build_types.h - Data structures for the build driver
 *
 * These structures represent the build plan that will be output as JSON.
 */

#ifndef BUILD_TYPES_H
#define BUILD_TYPES_H

#include <stdbool.h>

/* ============================================================================
 * Artifact Status (for package caching)
 * ========================================================================== */

typedef enum {
    ARTIFACT_PRESENT,        /* artifacts.dat exists with valid fingerprint */
    ARTIFACT_STALE,          /* artifacts.dat exists but fingerprint doesn't match */
    ARTIFACT_MISSING         /* artifacts.dat does not exist */
} ArtifactStatus;

/* ============================================================================
 * Foreign Module (import from a package)
 * ========================================================================== */

typedef struct {
    char *name;              /* Module name, e.g., "Html.Attributes" */
    char *package;           /* Package name, e.g., "elm/html" */
} BuildForeignModule;

/* ============================================================================
 * Package in build order
 * ========================================================================== */

typedef struct {
    char *name;              /* "author/name" format */
    char *version;           /* e.g., "1.0.5" */
    char *path;              /* Full path to src/ directory */
    char *package_path;      /* Full path to package root (without /src) */
    char **deps;             /* Array of "author/name" dependency strings */
    int dep_count;
    int dep_capacity;
    ArtifactStatus artifact_status;  /* Whether artifacts.dat exists */
} BuildPackage;

/* ============================================================================
 * Local module
 * ========================================================================== */

typedef struct {
    char *name;              /* Module name, e.g., "Page.Home" */
    char *path;              /* Relative path from project root, e.g., "src/Page/Home.elm" */
    char **deps;             /* Local module dependencies only */
    int dep_count;
    int dep_capacity;
    bool has_main;           /* Has main definition */
    bool cached;             /* Always false in Phase 1 */
    int level;               /* Parallel batch level (-1 = uncomputed) */
} BuildModule;

/* ============================================================================
 * Parallel batch
 * ========================================================================== */

typedef struct {
    int level;
    int count;
    BuildModule **modules;   /* Pointers to modules at this level */
    int modules_capacity;
} BuildBatch;

/* ============================================================================
 * Build problem
 * ========================================================================== */

typedef struct {
    char *module_name;       /* Optional: which module caused the problem */
    char *message;           /* Error description */
} BuildProblem;

/* ============================================================================
 * Complete build plan
 * ========================================================================== */

typedef struct {
    char *root;                          /* Project root directory (absolute) */
    char **src_dirs;                     /* Source directories (absolute paths) */
    int src_dir_count;
    bool use_cached;                     /* Always false in Phase 1 */
    char **roots;                        /* Entry point module names */
    int root_count;

    BuildForeignModule *foreign_modules; /* Imports from packages */
    int foreign_count;
    int foreign_capacity;

    BuildPackage *packages;              /* Packages in build order */
    int package_count;
    int package_capacity;

    BuildModule *modules;                /* Local modules in build order */
    int module_count;
    int module_capacity;

    BuildBatch *batches;                 /* Parallel batches */
    int batch_count;
    int batch_capacity;

    BuildProblem *problems;              /* Errors encountered */
    int problem_count;
    int problem_capacity;

    /* Summary statistics */
    int total_packages;
    int total_modules;
    int modules_to_build;
    int parallel_levels;
    int packages_with_artifacts;   /* Packages with valid artifacts.dat */
    int packages_stale;            /* Packages with stale artifacts.dat */
    int packages_missing;          /* Packages without artifacts.dat */
} BuildPlan;

#endif /* BUILD_TYPES_H */
