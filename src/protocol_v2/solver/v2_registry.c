/**
 * v2_registry.c - V2 Registry Index Reader Implementation
 *
 * Parses the V2 registry index format and provides access to package
 * dependency information without needing to read individual elm.json files.
 */

#include "v2_registry.h"
#include "../../alloc.h"
#include "../../log.h"
#include "../../vendor/miniz.h"
#include "../../dyn_array.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Initial capacities */
#define INITIAL_ENTRY_CAPACITY 512
#define INITIAL_VERSION_CAPACITY 8
#define INITIAL_DEP_CAPACITY 16

/**
 * Create an empty V2Registry.
 */
static V2Registry *v2_registry_create(void) {
    V2Registry *registry = arena_calloc(1, sizeof(V2Registry));
    if (!registry) {
        return NULL;
    }

    registry->entry_capacity = INITIAL_ENTRY_CAPACITY;
    registry->entries = arena_malloc(registry->entry_capacity * sizeof(V2PackageEntry));
    if (!registry->entries) {
        arena_free(registry);
        return NULL;
    }

    return registry;
}

/**
 * Parse a version status string to enum.
 */
static V2VersionStatus parse_status(const char *status) {
    if (!status) return V2_STATUS_VALID;
    
    if (strcmp(status, "valid") == 0) return V2_STATUS_VALID;
    if (strcmp(status, "obsolete") == 0) return V2_STATUS_OBSOLETE;
    if (strcmp(status, "missing") == 0) return V2_STATUS_MISSING;
    if (strcmp(status, "missing-deps") == 0) return V2_STATUS_MISSING_DEPS;
    
    return V2_STATUS_VALID;
}

/**
 * Skip whitespace, returning pointer to next non-whitespace char.
 */
static const char *skip_whitespace(const char *s) {
    while (*s && isspace((unsigned char)*s)) {
        s++;
    }
    return s;
}

/**
 * Extract a line from the text, returning the end pointer.
 * Modifies end to point past the newline.
 */
static char *extract_line(const char **pos, const char *end) {
    const char *start = *pos;
    const char *line_end = start;
    
    /* Find end of line */
    while (line_end < end && *line_end != '\n' && *line_end != '\r') {
        line_end++;
    }
    
    /* Calculate length and create copy */
    size_t len = line_end - start;
    char *line = arena_malloc(len + 1);
    if (!line) {
        return NULL;
    }
    memcpy(line, start, len);
    line[len] = '\0';
    
    /* Skip past newline */
    if (line_end < end) {
        if (*line_end == '\r') line_end++;
        if (line_end < end && *line_end == '\n') line_end++;
    }
    
    *pos = line_end;
    return line;
}

/**
 * Parse the format header line: "format N"
 */
static bool parse_format_line(const char *line, int *format_version) {
    return sscanf(line, "format %d", format_version) == 1;
}

/**
 * Parse the compiler line: "elm 0.19.1"
 */
static bool parse_compiler_line(const char *line, char **compiler_name, char **compiler_version) {
    /* Find the space between compiler name and version */
    const char *space = strchr(line, ' ');
    if (!space) {
        return false;
    }
    
    size_t name_len = space - line;
    *compiler_name = arena_malloc(name_len + 1);
    if (!*compiler_name) {
        return false;
    }
    memcpy(*compiler_name, line, name_len);
    (*compiler_name)[name_len] = '\0';
    
    /* Skip space and copy version */
    space = skip_whitespace(space);
    *compiler_version = arena_strdup(space);
    
    return *compiler_version != NULL;
}

/**
 * Parse a package line: "package: author/name"
 */
static bool parse_package_line(const char *line, char **author, char **name) {
    /* Line should be "package: author/name" */
    if (strncmp(line, "package: ", 9) != 0) {
        return false;
    }
    
    const char *pkg_name = line + 9;
    const char *slash = strchr(pkg_name, '/');
    if (!slash) {
        return false;
    }
    
    size_t author_len = slash - pkg_name;
    *author = arena_malloc(author_len + 1);
    if (!*author) {
        return false;
    }
    memcpy(*author, pkg_name, author_len);
    (*author)[author_len] = '\0';
    
    *name = arena_strdup(slash + 1);
    return *name != NULL;
}

/**
 * Parse a version line: "    version: X.Y.Z"
 */
static bool parse_version_line(const char *line, uint16_t *major, uint16_t *minor, uint16_t *patch) {
    /* Skip leading spaces and "version: " */
    line = skip_whitespace(line);
    if (strncmp(line, "version: ", 9) != 0) {
        return false;
    }
    
    line += 9;
    int maj, min, pat;
    if (sscanf(line, "%d.%d.%d", &maj, &min, &pat) != 3) {
        return false;
    }
    
    *major = (uint16_t)maj;
    *minor = (uint16_t)min;
    *patch = (uint16_t)pat;
    return true;
}

/**
 * Parse a status line: "    status: valid"
 */
static bool parse_status_line(const char *line, V2VersionStatus *status) {
    line = skip_whitespace(line);
    if (strncmp(line, "status: ", 8) != 0) {
        return false;
    }
    
    *status = parse_status(line + 8);
    return true;
}

/**
 * Parse a license line: "    license: MIT"
 */
static bool parse_license_line(const char *line, char **license) {
    line = skip_whitespace(line);
    if (strncmp(line, "license: ", 9) != 0) {
        return false;
    }
    
    *license = arena_strdup(line + 9);
    return *license != NULL;
}

/**
 * Parse a dependency line: "        author/name  1.0.0 <= v < 2.0.0"
 */
static bool parse_dependency_line(const char *line, char **package_name, char **constraint) {
    line = skip_whitespace(line);
    
    /* Find the double-space separator between package name and constraint */
    const char *sep = strstr(line, "  ");
    if (!sep) {
        return false;
    }
    
    size_t name_len = sep - line;
    *package_name = arena_malloc(name_len + 1);
    if (!*package_name) {
        return false;
    }
    memcpy(*package_name, line, name_len);
    (*package_name)[name_len] = '\0';
    
    /* Skip spaces to get constraint */
    sep = skip_whitespace(sep);
    *constraint = arena_strdup(sep);
    
    return *constraint != NULL;
}

/**
 * Add a package entry to the registry (with dynamic expansion).
 */
static V2PackageEntry *v2_registry_add_entry(V2Registry *registry, const char *author, const char *name) {
    if (registry->entry_count >= registry->entry_capacity) {
        size_t new_capacity = registry->entry_capacity * 2;
        V2PackageEntry *new_entries = arena_realloc(registry->entries, 
                                                     new_capacity * sizeof(V2PackageEntry));
        if (!new_entries) {
            return NULL;
        }
        registry->entries = new_entries;
        registry->entry_capacity = new_capacity;
    }
    
    V2PackageEntry *entry = &registry->entries[registry->entry_count];
    memset(entry, 0, sizeof(V2PackageEntry));
    
    entry->author = arena_strdup(author);
    entry->name = arena_strdup(name);
    if (!entry->author || !entry->name) {
        arena_free(entry->author);
        arena_free(entry->name);
        return NULL;
    }
    
    registry->entry_count++;
    return entry;
}

/**
 * Add a version to a package entry (with dynamic expansion).
 */
static V2PackageVersion *v2_package_add_version(V2PackageEntry *entry) {
    /* Calculate current capacity based on allocation size */
    if (entry->versions == NULL) {
        entry->versions = arena_malloc(INITIAL_VERSION_CAPACITY * sizeof(V2PackageVersion));
        if (!entry->versions) {
            return NULL;
        }
    } else {
        /* Check if we need to grow - we track count but not capacity explicitly
         * Use power of 2 growth. Current capacity is smallest power of 2 >= count */
        size_t count = entry->version_count;
        size_t cap = INITIAL_VERSION_CAPACITY;
        while (cap < count) {
            cap *= 2;
        }
        
        /* We need to grow when count reaches current capacity */
        if (count >= cap) {
            /* Need to grow */
            size_t new_cap = cap * 2;
            V2PackageVersion *new_versions = arena_realloc(entry->versions,
                                                           new_cap * sizeof(V2PackageVersion));
            if (!new_versions) {
                return NULL;
            }
            entry->versions = new_versions;
        }
    }
    
    V2PackageVersion *version = &entry->versions[entry->version_count];
    memset(version, 0, sizeof(V2PackageVersion));
    entry->version_count++;
    return version;
}

/**
 * Add a dependency to a version (with dynamic expansion).
 */
static bool v2_version_add_dependency(V2PackageVersion *version, 
                                       const char *package_name, 
                                       const char *constraint) {
    if (version->dependencies == NULL) {
        version->dependencies = arena_malloc(INITIAL_DEP_CAPACITY * sizeof(V2Dependency));
        if (!version->dependencies) {
            return false;
        }
    } else {
        /* Calculate capacity and check if we need to grow.
         * Current capacity is smallest power of 2 >= count */
        size_t count = version->dependency_count;
        size_t cap = INITIAL_DEP_CAPACITY;
        while (cap < count) {
            cap *= 2;
        }
        
        /* We need to grow when count reaches current capacity */
        if (count >= cap) {
            size_t new_cap = cap * 2;
            V2Dependency *new_deps = arena_realloc(version->dependencies,
                                                    new_cap * sizeof(V2Dependency));
            if (!new_deps) {
                return false;
            }
            version->dependencies = new_deps;
        }
    }
    
    V2Dependency *dep = &version->dependencies[version->dependency_count];
    dep->package_name = arena_strdup(package_name);
    dep->constraint = arena_strdup(constraint);
    
    if (!dep->package_name || !dep->constraint) {
        arena_free(dep->package_name);
        arena_free(dep->constraint);
        return false;
    }
    
    version->dependency_count++;
    return true;
}

V2Registry *v2_registry_parse(const char *data, size_t size) {
    if (!data || size == 0) {
        log_error("v2_registry_parse: empty data");
        return NULL;
    }
    
    V2Registry *registry = v2_registry_create();
    if (!registry) {
        log_error("v2_registry_parse: failed to create registry");
        return NULL;
    }
    
    const char *pos = data;
    const char *end = data + size;
    
    /* State machine for parsing */
    V2PackageEntry *current_entry = NULL;
    V2PackageVersion *current_version = NULL;
    bool in_dependencies = false;
    int line_number = 0;
    
    while (pos < end) {
        char *line = extract_line(&pos, end);
        if (!line) {
            break;
        }
        line_number++;
        
        /* Skip empty lines */
        if (line[0] == '\0') {
            arena_free(line);
            continue;
        }
        
        /* Check what kind of line this is based on indentation */
        int indent_spaces = 0;
        const char *p = line;
        while (*p == ' ') {
            indent_spaces++;
            p++;
        }
        
        if (indent_spaces == 0) {
            /* Top-level line */
            in_dependencies = false;
            
            if (strncmp(line, "format ", 7) == 0) {
                if (!parse_format_line(line, &registry->format_version)) {
                    log_error("v2_registry_parse: invalid format line at line %d", line_number);
                    arena_free(line);
                    v2_registry_free(registry);
                    return NULL;
                }
                
                if (registry->format_version != 2) {
                    log_error("v2_registry_parse: unsupported format version %d", 
                              registry->format_version);
                    arena_free(line);
                    v2_registry_free(registry);
                    return NULL;
                }
            }
            else if (strncmp(line, "package:", 8) == 0) {
                char *author = NULL;
                char *name = NULL;
                if (!parse_package_line(line, &author, &name)) {
                    log_error("v2_registry_parse: invalid package line at line %d: %s", 
                              line_number, line);
                    arena_free(line);
                    v2_registry_free(registry);
                    return NULL;
                }
                
                current_entry = v2_registry_add_entry(registry, author, name);
                current_version = NULL;
                arena_free(author);
                arena_free(name);
                
                if (!current_entry) {
                    log_error("v2_registry_parse: failed to add entry at line %d", line_number);
                    arena_free(line);
                    v2_registry_free(registry);
                    return NULL;
                }
            }
            else {
                /* Could be compiler line (e.g., "elm 0.19.1") */
                const char *space = strchr(line, ' ');
                if (space && !registry->compiler_name) {
                    parse_compiler_line(line, &registry->compiler_name, &registry->compiler_version);
                }
            }
        }
        else if (indent_spaces == 4) {
            /* Version-level line */
            in_dependencies = false;
            
            if (strncmp(p, "version:", 8) == 0) {
                if (!current_entry) {
                    log_error("v2_registry_parse: version without package at line %d", line_number);
                    arena_free(line);
                    v2_registry_free(registry);
                    return NULL;
                }
                
                current_version = v2_package_add_version(current_entry);
                if (!current_version) {
                    log_error("v2_registry_parse: failed to add version at line %d", line_number);
                    arena_free(line);
                    v2_registry_free(registry);
                    return NULL;
                }
                
                if (!parse_version_line(line, &current_version->major, 
                                        &current_version->minor, 
                                        &current_version->patch)) {
                    log_error("v2_registry_parse: invalid version at line %d: %s", 
                              line_number, line);
                    arena_free(line);
                    v2_registry_free(registry);
                    return NULL;
                }
            }
            else if (strncmp(p, "status:", 7) == 0) {
                if (current_version) {
                    parse_status_line(line, &current_version->status);
                }
            }
            else if (strncmp(p, "license:", 8) == 0) {
                if (current_version) {
                    parse_license_line(line, &current_version->license);
                }
            }
            else if (strncmp(p, "dependencies:", 13) == 0) {
                in_dependencies = true;
            }
        }
        else if (indent_spaces == 8 && in_dependencies && current_version) {
            /* Dependency line */
            char *pkg_name = NULL;
            char *constraint = NULL;
            
            if (parse_dependency_line(line, &pkg_name, &constraint)) {
                if (!v2_version_add_dependency(current_version, pkg_name, constraint)) {
                    log_error("v2_registry_parse: failed to add dependency at line %d", line_number);
                }
                
                arena_free(pkg_name);
                arena_free(constraint);
            }
        }
        
        arena_free(line);
    }
    
    log_debug("v2_registry_parse: loaded %zu packages", registry->entry_count);
    return registry;
}

V2Registry *v2_registry_load_from_text(const char *text_path) {
    if (!text_path) {
        log_error("v2_registry_load_from_text: NULL path");
        return NULL;
    }
    
    FILE *f = fopen(text_path, "rb");
    if (!f) {
        log_error("v2_registry_load_from_text: failed to open %s", text_path);
        return NULL;
    }
    
    /* Get file size */
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (file_size <= 0) {
        log_error("v2_registry_load_from_text: empty or invalid file %s", text_path);
        fclose(f);
        return NULL;
    }
    
    /* Read entire file */
    char *data = arena_malloc((size_t)file_size + 1);
    if (!data) {
        log_error("v2_registry_load_from_text: out of memory");
        fclose(f);
        return NULL;
    }
    
    size_t read_size = fread(data, 1, (size_t)file_size, f);
    fclose(f);
    
    data[read_size] = '\0';
    
    V2Registry *registry = v2_registry_parse(data, read_size);
    arena_free(data);
    
    return registry;
}

V2Registry *v2_registry_load_from_zip(const char *zip_path) {
    if (!zip_path) {
        log_error("v2_registry_load_from_zip: NULL path");
        return NULL;
    }
    
    /* Initialize zip archive */
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    
    if (!mz_zip_reader_init_file(&zip, zip_path, 0)) {
        log_error("v2_registry_load_from_zip: failed to open zip file %s", zip_path);
        return NULL;
    }
    
    /* Get number of files in archive */
    mz_uint num_files = mz_zip_reader_get_num_files(&zip);
    if (num_files == 0) {
        log_error("v2_registry_load_from_zip: empty zip archive %s", zip_path);
        mz_zip_reader_end(&zip);
        return NULL;
    }
    
    /* Extract the first (and typically only) file */
    size_t uncompressed_size = 0;
    void *data = mz_zip_reader_extract_to_heap(&zip, 0, &uncompressed_size, 0);
    mz_zip_reader_end(&zip);
    
    if (!data) {
        log_error("v2_registry_load_from_zip: failed to extract from %s", zip_path);
        return NULL;
    }
    
    /* Parse the extracted text */
    V2Registry *registry = v2_registry_parse((const char *)data, uncompressed_size);
    
    /* miniz uses malloc internally, so use free here */
    free(data);
    
    return registry;
}

void v2_registry_free(V2Registry *registry) {
    if (!registry) {
        return;
    }
    
    for (size_t i = 0; i < registry->entry_count; i++) {
        V2PackageEntry *entry = &registry->entries[i];
        
        arena_free(entry->author);
        arena_free(entry->name);
        
        for (size_t j = 0; j < entry->version_count; j++) {
            V2PackageVersion *version = &entry->versions[j];
            
            arena_free(version->license);
            
            for (size_t k = 0; k < version->dependency_count; k++) {
                arena_free(version->dependencies[k].package_name);
                arena_free(version->dependencies[k].constraint);
            }
            arena_free(version->dependencies);
        }
        arena_free(entry->versions);
    }
    
    arena_free(registry->entries);
    arena_free(registry->compiler_name);
    arena_free(registry->compiler_version);
    arena_free(registry);
}

V2PackageEntry *v2_registry_find(V2Registry *registry, const char *author, const char *name) {
    if (!registry || !author || !name) {
        return NULL;
    }
    
    for (size_t i = 0; i < registry->entry_count; i++) {
        V2PackageEntry *entry = &registry->entries[i];
        if (strcmp(entry->author, author) == 0 && strcmp(entry->name, name) == 0) {
            return entry;
        }
    }
    
    return NULL;
}

V2PackageVersion *v2_registry_find_version(V2Registry *registry,
                                           const char *author, const char *name,
                                           uint16_t major, uint16_t minor, uint16_t patch) {
    V2PackageEntry *entry = v2_registry_find(registry, author, name);
    if (!entry) {
        return NULL;
    }
    
    for (size_t i = 0; i < entry->version_count; i++) {
        V2PackageVersion *v = &entry->versions[i];
        if (v->major == major && v->minor == minor && v->patch == patch) {
            return v;
        }
    }
    
    return NULL;
}
