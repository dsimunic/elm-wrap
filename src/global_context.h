#ifndef GLOBAL_CONTEXT_H
#define GLOBAL_CONTEXT_H

#include <stdbool.h>

/**
 * This struct holds global state that is determined at program startup
 * and influences how commands operate throughout the program's lifetime.
 */

/* Protocol mode for package management */
typedef enum {
    PROTOCOL_V1,  /* Emulates existing Elm registry (package.elm-lang.org) */
    PROTOCOL_V2   /* New elm-wrap repository protocol */
} ProtocolMode;

/* Known compiler types */
typedef enum {
    COMPILER_ELM,     /* Standard Elm compiler */
    COMPILER_LAMDERA, /* Lamdera compiler (extended command set) */
    COMPILER_WRAPC,   /* wrapc compiler (minimal command set, make only) */
    COMPILER_UNKNOWN  /* Unknown compiler (treated like Elm) */
} CompilerType;

typedef struct {
    /* Protocol mode: V1 (legacy Elm) or V2 (elm-wrap repositories) */
    ProtocolMode protocol_mode;

    /* Program name (extracted from argv[0]) */
    char *program_name;      /* e.g., "wrap" or user-defined alias */

    /* Compiler information (populated when V2 mode is detected) */
    char *compiler_name;     /* e.g., "elm", "lamdera", "wrapc" */
    char *compiler_version;  /* e.g., "0.19.1" */
    CompilerType compiler_type; /* Detected compiler type */

    /* V2 repository path (only set when protocol_mode == PROTOCOL_V2) */
    char *repository_path;   /* Full path to active repository */
} GlobalContext;

/**
 * Initialize the global context.
 *
 * This function determines the protocol mode by checking for V2 repositories.
 * V2 mode is active when a repository exists for the current compiler and version
 * at the configured repository local path.
 *
 * Detection logic:
 * 1. Get repository root path (WRAP_REPOSITORY_LOCAL_PATH or default)
 * 2. Determine compiler name (from WRAP_ELM_COMPILER_PATH basename, or "elm")
 * 3. Determine compiler version (by running compiler --version)
 * 4. Check if <root>/<compiler>/<version>/ exists as a directory
 *    - If yes: V2 mode (repository created via `repository new`)
 *    - If no: V1 mode (use traditional Elm package management)
 *
 * The program name is extracted from argv[0] (via basename).
 *
 * @param argv0 The argv[0] from main (program path)
 * @return Pointer to arena-allocated GlobalContext, or NULL on failure
 */
GlobalContext *global_context_init(const char *argv0);

/**
 * Get the current global context.
 * 
 * @return Pointer to the global context, or NULL if not initialized
 */
GlobalContext *global_context_get(void);

/**
 * Check if V2 protocol mode is active.
 * 
 * @return true if V2 mode, false if V1 mode or context not initialized
 */
bool global_context_is_v2(void);

/**
 * Get a human-readable string for the current protocol mode.
 * 
 * @return "V1" or "V2"
 */
const char *global_context_mode_string(void);

/**
 * Get the detected compiler type.
 * 
 * @return CompilerType enum value
 */
CompilerType global_context_compiler_type(void);

/**
 * Check if the current compiler is Elm.
 * 
 * @return true if compiler is elm or unknown
 */
bool global_context_is_elm(void);

/**
 * Check if the current compiler is Lamdera.
 * 
 * @return true if compiler is lamdera
 */
bool global_context_is_lamdera(void);

/**
 * Check if the current compiler is wrapc.
 *
 * @return true if compiler is wrapc
 */
bool global_context_is_wrapc(void);

/**
 * Get the program name (from argv[0]).
 *
 * This returns the actual executable name that was used to invoke the program,
 * which allows the binary to work correctly even if renamed or aliased.
 *
 * @return The program name, or "wrap" if not initialized
 */
const char *global_context_program_name(void);

#endif /* GLOBAL_CONTEXT_H */
