/*
 * indexmaker - Generate registry.dat files from package specifications
 *
 * Takes package specifications in the format "author/package@version" (one per line)
 * and generates a registry.dat file compatible with the Elm compiler.
 *
 * Usage:
 *   indexmaker <input-file> <output-file>
 *   cat input.txt | indexmaker - output.dat
 *   indexmaker - output.dat < input.txt
 */

#include "../../src/alloc.h"
#include "../../src/constants.h"
#include "../../src/registry.h"
#include "../../src/protocol_v2/solver/v2_registry.h"
#include "../../src/log.h"
#include "../../src/exit_codes.h"
#include "../../src/commands/package/package_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define INITIAL_LINE_CAPACITY MAX_TEMP_PATH_LENGTH

static void print_usage(const char *prog) {
    fprintf(stderr, "Usage: %s INPUT PATH\n", prog);
    fprintf(stderr, "       %s - PATH  (read from stdin)\n", prog);
    fprintf(stderr, "\n");
    fprintf(stderr, "Input format:\n");
    fprintf(stderr, "  - V1: One package per line in format 'author/package@version'\n");
    fprintf(stderr, "  - V2: Registry text format starting with 'format 2' header\n");
    fprintf(stderr, "Example:\n");
    fprintf(stderr, "  elm/core@1.0.5\n");
    fprintf(stderr, "  elm/html@1.0.0\n");
    fprintf(stderr, "  author/package@2.3.4\n");
}

/* Read a line from file, dynamically allocating buffer as needed */
static char* read_line(FILE *f, size_t *line_capacity, char **line_buffer) {
    if (!*line_buffer) {
        *line_capacity = INITIAL_LINE_CAPACITY;
        *line_buffer = arena_malloc(*line_capacity);
        if (!*line_buffer) {
            return NULL;
        }
    }

    size_t pos = 0;
    int ch;

    while ((ch = fgetc(f)) != EOF) {
        /* Grow buffer if needed */
        if (pos + 1 >= *line_capacity) {
            *line_capacity *= 2;
            *line_buffer = arena_realloc(*line_buffer, *line_capacity);
            if (!*line_buffer) {
                return NULL;
            }
        }

        if (ch == '\n') {
            break;
        }

        (*line_buffer)[pos++] = (char)ch;
    }

    /* EOF with no characters read */
    if (ch == EOF && pos == 0) {
        return NULL;
    }

    (*line_buffer)[pos] = '\0';
    return *line_buffer;
}

/* Trim leading and trailing whitespace in place */
static void trim_whitespace(char *str) {
    if (!str) return;

    /* Trim leading */
    char *start = str;
    while (*start == ' ' || *start == '\t' || *start == '\r') {
        start++;
    }

    size_t len = strlen(start);
    if (len == 0) {
        if (start != str) {
            str[0] = '\0';
        }
        return;
    }

    /* Trim trailing */
    char *end = start + len - 1;
    while (end >= start && (*end == ' ' || *end == '\t' || *end == '\r')) {
        *end = '\0';
        end--;
    }

    /* Move trimmed string to beginning if needed */
    if (start != str) {
        memmove(str, start, strlen(start) + 1);
    }
}

static char *read_stream_to_buffer_with_prefix(
    FILE *f,
    const char *prefix,
    size_t prefix_len,
    size_t *out_size
) {
    if (!f || !prefix || !out_size) return NULL;

    size_t capacity = prefix_len + MAX_TEMP_BUFFER_LENGTH;
    char *buffer = arena_malloc(capacity);
    if (!buffer) return NULL;

    memcpy(buffer, prefix, prefix_len);
    size_t size = prefix_len;

    char tmp[MAX_TEMP_BUFFER_LENGTH];
    size_t nread;

    while ((nread = fread(tmp, 1, sizeof(tmp), f)) > 0) {
        if (size + nread > capacity) {
            while (size + nread > capacity) {
                capacity *= 2;
            }
            buffer = arena_realloc(buffer, capacity);
            if (!buffer) return NULL;
        }

        memcpy(buffer + size, tmp, nread);
        size += nread;
    }

    if (ferror(f)) {
        return NULL;
    }

    *out_size = size;
    return buffer;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        print_usage(argv[0]);
        return EXIT_GENERAL_ERROR;
    }

    const char *input_path = argv[1];
    const char *output_path = argv[2];

    /* Initialize arena allocator */
    alloc_init();

    /* Open input file (or stdin) */
    FILE *input = NULL;
    if (strcmp(input_path, "-") == 0) {
        input = stdin;
    } else {
        input = fopen(input_path, "r");
        if (!input) {
            fprintf(stderr, "Error: Failed to open input file '%s': %s\n", 
                    input_path, strerror(errno));
            alloc_shutdown();
            return EXIT_GENERAL_ERROR;
        }
    }

    /* Create registry */
    Registry *registry = registry_create();
    if (!registry) {
        fprintf(stderr, "Error: Failed to create registry\n");
        if (input != stdin) fclose(input);
        alloc_shutdown();
        return EXIT_GENERAL_ERROR;
    }

    /* Read and parse input */
    size_t line_capacity = 0;
    char *line_buffer = NULL;
    size_t line_num = 0;
    size_t packages_added = 0;

    char *first_line = read_line(input, &line_capacity, &line_buffer);
    if (!first_line) {
        fprintf(stderr, "Error: No input data\n");
        if (input != stdin) fclose(input);
        alloc_shutdown();
        return EXIT_GENERAL_ERROR;
    }

    line_num = 1;
    trim_whitespace(first_line);

    bool is_v2 = (strcmp(first_line, "format 2") == 0);

    if (is_v2) {
        V2Registry *v2_registry = NULL;

        if (strcmp(input_path, "-") == 0) {
            const char header[] = "format 2\n";
            size_t v2_size = 0;
            char *v2_text = read_stream_to_buffer_with_prefix(
                input,
                header,
                sizeof(header) - 1,
                &v2_size
            );
            if (!v2_text) {
                fprintf(stderr, "Error: Failed to read V2 registry from stdin\n");
                alloc_shutdown();
                return EXIT_GENERAL_ERROR;
            }

            v2_registry = v2_registry_parse(v2_text, v2_size);
        } else {
            fclose(input);
            input = NULL;
            v2_registry = v2_registry_load_from_text(input_path);
        }

        if (!v2_registry) {
            fprintf(stderr, "Error: Failed to parse V2 registry input\n");
            alloc_shutdown();
            return EXIT_GENERAL_ERROR;
        }

        for (size_t i = 0; i < v2_registry->entry_count; i++) {
            V2PackageEntry *entry = &v2_registry->entries[i];
            for (size_t j = 0; j < entry->version_count; j++) {
                V2PackageVersion *v2v = &entry->versions[j];
                if (v2v->status != V2_STATUS_VALID) {
                    continue;
                }

                Version version = {
                    .major = v2v->major,
                    .minor = v2v->minor,
                    .patch = v2v->patch
                };

                if (!registry_add_version(registry, entry->author, entry->name, version)) {
                    fprintf(stderr, "Warning: Failed to add version %u.%u.%u to %s/%s\n",
                            version.major, version.minor, version.patch, entry->author, entry->name);
                    continue;
                }

                packages_added++;
            }
        }

        v2_registry_free(v2_registry);
    } else {
        /* V1 path: first line must be a package spec */
        if (first_line[0] == '\0' || first_line[0] == '#') {
            fprintf(stderr, "Error: Line 1: Expected package specification 'author/package@version'\n");
            if (input != stdin) fclose(input);
            alloc_shutdown();
            return EXIT_GENERAL_ERROR;
        }

        char *author = NULL;
        char *name = NULL;
        Version version;

        if (!parse_package_with_version(first_line, &author, &name, &version)) {
            fprintf(stderr, "Error: Line 1: Invalid package specification '%s'\n", first_line);
            fprintf(stderr, "       Expected format: author/package@version\n");
            if (input != stdin) fclose(input);
            alloc_shutdown();
            return EXIT_GENERAL_ERROR;
        }

        if (!registry_add_version(registry, author, name, version)) {
            fprintf(stderr, "Error: Failed to add version %u.%u.%u to %s/%s\n",
                    version.major, version.minor, version.patch, author, name);
            arena_free(author);
            arena_free(name);
            if (input != stdin) fclose(input);
            alloc_shutdown();
            return EXIT_GENERAL_ERROR;
        }

        packages_added++;
        arena_free(author);
        arena_free(name);

        while (1) {
            char *line = read_line(input, &line_capacity, &line_buffer);
            if (!line) break;

            line_num++;
            trim_whitespace(line);

            /* Skip empty lines and comments */
            if (line[0] == '\0' || line[0] == '#') {
                continue;
            }

            /* Parse package specification: author/package@version */
            author = NULL;
            name = NULL;

            if (!parse_package_with_version(line, &author, &name, &version)) {
                fprintf(stderr, "Warning: Line %zu: Invalid package specification '%s'\n",
                        line_num, line);
                fprintf(stderr, "         Expected format: author/package@version\n");
                continue;
            }

            /* Add version to package */
            if (!registry_add_version(registry, author, name, version)) {
                fprintf(stderr, "Warning: Failed to add version %u.%u.%u to %s/%s\n",
                        version.major, version.minor, version.patch, author, name);
            } else {
                packages_added++;
            }

            arena_free(author);
            arena_free(name);
        }

        /* Close input */
        if (input != stdin) {
            fclose(input);
        }
    }

    /* Clean up line buffer */
    if (line_buffer) {
        arena_free(line_buffer);
    }

    if (packages_added == 0) {
        fprintf(stderr, "Error: No valid packages found in input\n");
        alloc_shutdown();
        return EXIT_GENERAL_ERROR;
    }

    /* Sort registry entries for consistent output */
    registry_sort_entries(registry);

    /* Write registry.dat file */
    printf("Writing registry with %zu packages (%zu total versions) to %s\n",
           registry->entry_count, registry->total_versions, output_path);

    if (!registry_dat_write(registry, output_path)) {
        fprintf(stderr, "Error: Failed to write registry to %s\n", output_path);
        alloc_shutdown();
        return EXIT_GENERAL_ERROR;
    }

    printf("Successfully wrote %s\n", output_path);

    /* Cleanup */
    alloc_shutdown();
    return EXIT_SUCCESS;
}
