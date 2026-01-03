#ifndef CONSTANTS_H
#define CONSTANTS_H

/*
 * Buffer size constants
 */
#define MAX_PATH_LENGTH 4096
#define MAX_MEDIUM_PATH_LENGTH 2048
#define MAX_TEMP_PATH_LENGTH 1024
#define MAX_PACKAGE_NAME_LENGTH 256
#define MAX_VERSION_STRING_LENGTH 32
#define MAX_VERSION_STRING_MEDIUM_LENGTH 64
#define MAX_ERROR_MESSAGE_LENGTH 4096
#define MAX_TEMP_BUFFER_LENGTH 512
#define MAX_RANGE_STRING_LENGTH 128
#define MAX_TERM_STRING_LENGTH 256
#define MAX_KEY_LENGTH 256
#define MAX_RESPONSE_LENGTH 10
#define MAX_CHECK_STRING_LENGTH 512
#define MAX_HASH_FILENAME_LENGTH 32
#define MAX_SEARCH_PATTERN_LENGTH 256
#define MAX_USER_AGENT_LENGTH 64
#define MAX_LATEST_VERSION_LENGTH 32

/*
 * Initial capacity constants for dynamic arrays
 */
#define INITIAL_TINY_CAPACITY 4
#define INITIAL_MINIMAL_CAPACITY 8
#define INITIAL_SMALL_CAPACITY 16
#define INITIAL_MEDIUM_CAPACITY 64
#define INITIAL_LARGE_CAPACITY 256

/* Specific type capacities */
#define INITIAL_MODULE_CAPACITY 16
#define INITIAL_PACKAGE_CAPACITY 64
#define INITIAL_FILE_CAPACITY 256
#define INITIAL_VISITED_CAPACITY 64
#define INITIAL_CONNECTION_CAPACITY 32
#define INITIAL_PLAN_CAPACITY 32
#define INITIAL_REGISTRY_CAPACITY 128

/*
 * File permissions
 */
#define DIR_PERMISSIONS 0755

/*
 * Unit conversion constants
 */
#define BYTES_PER_KB 1024.0
#define BYTES_PER_MB (1024 * 1024)
#define MICROSECONDS_PER_SECOND 1000000.0

/*
 * Download/transfer constants
 */
#define PROGRESS_BYTES_PER_DOT (50 * 1024)

/*
 * Memory allocation constants
 */
#define INITIAL_ARENA_SIZE BYTES_PER_MB

/*
 * String formatting padding
 */
#define PATH_SEPARATOR_PADDING 20
#define PACKAGE_NAME_SEPARATOR_PADDING 10

/*
 * Tree-sitter field names and lengths
 */
#define FIELD_OPERATOR "operator"
#define FIELD_OPERATOR_LEN 8
#define FIELD_ASSOCIATIVITY "associativity"
#define FIELD_ASSOCIATIVITY_LEN 13
#define FIELD_PRECEDENCE "precedence"
#define FIELD_PRECEDENCE_LEN 10

/*
 * PubGrub solver constants
 */
#define PG_DECISION_VERSION_BUFFER 128
#define PG_DEPENDENCY_BUFFER 128

/*
 * Security hardening limits
 *
 * These bounds exist to prevent pathological inputs (or corrupted caches)
 * from causing excessive CPU/memory usage.
 */

/* Maximum size for an elm.json file we will read/parse. */
#define MAX_ELM_JSON_FILE_BYTES (1 * BYTES_PER_MB)

/* Maximum size for an Elm source (.elm) file we will read/parse. */
#define MAX_ELM_SOURCE_FILE_BYTES (4 * BYTES_PER_MB)

/* Maximum size for a docs.json file we will read/parse. */
#define MAX_DOCS_JSON_FILE_BYTES (8 * BYTES_PER_MB)

/* Maximum size for a V1 /since JSON file we will read/parse (debug tooling). */
#define MAX_V1_SINCE_JSON_FILE_BYTES (32 * BYTES_PER_MB)

/* Maximum size for a V2 registry text file we will read/parse. */
#define MAX_V2_REGISTRY_TEXT_FILE_BYTES (64 * BYTES_PER_MB)

/* Maximum size for a single embedded archive entry we will extract into memory. */
#define MAX_EMBEDDED_ARCHIVE_EXTRACT_BYTES (64 * BYTES_PER_MB)

/* Legacy helper bound: file_read_contents() should never read unbounded data. */
#define MAX_FILE_READ_CONTENTS_BYTES (64 * BYTES_PER_MB)

/*
 * Rulr file-size hardening
 *
 * These bounds protect against pathological (or corrupted) rule inputs.
 */
#define MAX_RULR_TEXT_FILE_BYTES (8 * BYTES_PER_MB)
#define MAX_RULR_COMPILED_FILE_BYTES (32 * BYTES_PER_MB)

/* Maximum number of dependency entries we will accept from an elm.json. */
#define MAX_ELM_JSON_DEPENDENCY_ENTRIES 4096

/* Maximum length of a dependency version/constraint string from elm.json. */
#define MAX_ELM_JSON_VERSION_VALUE_LENGTH MAX_RANGE_STRING_LENGTH

/* PubGrub solve-loop budgets (DoS protection). */
#define PG_MAX_DECISIONS 200000
#define PG_MAX_PROPAGATIONS 1000000
#define PG_MAX_CONFLICTS 200000

/* Memory-growth budgets (additional DoS protection). */
#define PG_MAX_PACKAGES 10000
#define PG_MAX_TRAIL_ASSIGNMENTS 200000
#define PG_MAX_INCOMPATIBILITIES 200000

/*
 * Large buffer constants
 */
#define MAX_LARGE_BUFFER_LENGTH 65536  /* 64KB for large records/data */

/*
 * Hash algorithm constants
 */
#define DJB2_HASH_INIT 5381  /* Initial value for djb2 hash algorithm */

/*
 * Local development tracking constants
 */
#define LOCAL_DEV_TRACKING_DIR "_local-dev"  /* Directory under WRAP_HOME for dependency tracking */
#define REGISTRY_LOCAL_DEV_DAT "registry-local-dev.dat"  /* Text registry file for local-dev packages */

/*
 * Lamdera tracking constants
 */
#define LAMDERA_TRACKING_DIR "_lamdera"  /* Directory under WRAP_HOME for Lamdera-specific data */
#define LAMDERA_REGISTRY_DAT "registry.dat"  /* Lamdera registry file name under LAMDERA_TRACKING_DIR */

#endif /* CONSTANTS_H */
