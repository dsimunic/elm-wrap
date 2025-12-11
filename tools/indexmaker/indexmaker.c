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
#include "../../src/log.h"
#include "../../src/exit_codes.h"
#include "../../src/commands/package/package_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define INITIAL_LINE_CAPACITY 1024

static void print_usage(const char *prog) {
    fprintf(stderr, "Usage: %s INPUT PATH\n", prog);
    fprintf(stderr, "       %s - PATH  (read from stdin)\n", prog);
    fprintf(stderr, "\n");
    fprintf(stderr, "Input format: One package per line in format 'author/package@version'\n");
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

    /* Trim trailing */
    char *end = start + strlen(start) - 1;
    while (end >= start && (*end == ' ' || *end == '\t' || *end == '\r')) {
        *end = '\0';
        end--;
    }

    /* Move trimmed string to beginning if needed */
    if (start != str) {
        memmove(str, start, strlen(start) + 1);
    }
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
        char *author = NULL;
        char *name = NULL;
        Version version;

        if (!parse_package_with_version(line, &author, &name, &version)) {
            fprintf(stderr, "Warning: Line %zu: Invalid package specification '%s'\n", 
                    line_num, line);
            fprintf(stderr, "         Expected format: author/package@version\n");
            continue;
        }

        /* Ensure package exists in registry */
        RegistryEntry *entry = registry_find(registry, author, name);
        if (!entry) {
            if (!registry_add_entry(registry, author, name)) {
                fprintf(stderr, "Error: Failed to add entry for %s/%s\n", author, name);
                arena_free(author);
                arena_free(name);
                continue;
            }
            entry = registry_find(registry, author, name);
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

    /* Clean up line buffer */
    if (line_buffer) {
        arena_free(line_buffer);
    }

    /* Close input */
    if (input != stdin) {
        fclose(input);
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
