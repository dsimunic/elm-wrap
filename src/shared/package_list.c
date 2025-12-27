/**
 * package_list.c - Unified package list printing and sorting
 */

#include "package_list.h"
#include "../alloc.h"
#include "../constants.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int package_list_compare(const void *a, const void *b) {
    const PackageListEntry *pkg_a = (const PackageListEntry *)a;
    const PackageListEntry *pkg_b = (const PackageListEntry *)b;

    /* Compare by author first */
    int author_cmp = strcmp(pkg_a->author, pkg_b->author);
    if (author_cmp != 0) {
        return author_cmp;
    }

    /* If authors are the same, compare by name */
    return strcmp(pkg_a->name, pkg_b->name);
}

int package_name_compare(const void *a, const void *b) {
    const char *name_a = *(const char **)a;
    const char *name_b = *(const char **)b;

    /* Find the slash in each name to split author/name */
    const char *slash_a = strchr(name_a, '/');
    const char *slash_b = strchr(name_b, '/');

    /* If either doesn't have a slash, fall back to strcmp */
    if (!slash_a || !slash_b) {
        return strcmp(name_a, name_b);
    }

    /* Compare authors (part before slash) */
    size_t author_len_a = (size_t)(slash_a - name_a);
    size_t author_len_b = (size_t)(slash_b - name_b);

    size_t min_author_len = author_len_a < author_len_b ? author_len_a : author_len_b;
    int author_cmp = strncmp(name_a, name_b, min_author_len);
    if (author_cmp != 0) {
        return author_cmp;
    }

    /* Authors match up to min length, compare by length */
    if (author_len_a != author_len_b) {
        return author_len_a < author_len_b ? -1 : 1;
    }

    /* Authors are equal, compare names (part after slash) */
    return strcmp(slash_a + 1, slash_b + 1);
}

size_t package_list_max_name_width(const PackageListEntry *entries, int count) {
    size_t max_width = 0;

    for (int i = 0; i < count; i++) {
        if (!entries[i].author || !entries[i].name) {
            continue;
        }
        size_t len = strlen(entries[i].author) + 1 + strlen(entries[i].name);
        if (len > max_width) {
            max_width = len;
        }
    }

    return max_width;
}

void package_list_print(const PackageListEntry *entries, int count,
                        size_t max_width, int indent) {
    if (!entries || count <= 0) {
        return;
    }

    /* Auto-calculate width if not specified */
    if (max_width == 0) {
        max_width = package_list_max_name_width(entries, count);
    }

    char full_name[MAX_PACKAGE_NAME_LENGTH];

    for (int i = 0; i < count; i++) {
        if (!entries[i].author || !entries[i].name) {
            continue;
        }

        snprintf(full_name, sizeof(full_name), "%s/%s",
                 entries[i].author, entries[i].name);

        /* Print indent */
        for (int j = 0; j < indent; j++) {
            putchar(' ');
        }

        /* Print aligned name and version */
        if (entries[i].version) {
            printf("%-*s  %s%s\n",
                   (int)max_width, full_name,
                   entries[i].version,
                   entries[i].annotation ? entries[i].annotation : "");
        } else {
            printf("%s%s\n",
                   full_name,
                   entries[i].annotation ? entries[i].annotation : "");
        }
    }
}

void package_list_print_sorted(const PackageListEntry *entries, int count,
                               size_t max_width, int indent) {
    if (!entries || count <= 0) {
        return;
    }

    /* Create a copy for sorting */
    PackageListEntry *sorted = arena_malloc((size_t)count * sizeof(PackageListEntry));
    if (!sorted) {
        /* Fallback to unsorted print on allocation failure */
        package_list_print(entries, count, max_width, indent);
        return;
    }

    memcpy(sorted, entries, (size_t)count * sizeof(PackageListEntry));
    qsort(sorted, (size_t)count, sizeof(PackageListEntry), package_list_compare);

    package_list_print(sorted, count, max_width, indent);

    arena_free(sorted);
}

void package_names_print_sorted(const char **names, int count, int indent) {
    if (!names || count <= 0) {
        return;
    }

    /* Create a copy for sorting */
    const char **sorted = arena_malloc((size_t)count * sizeof(char *));
    if (!sorted) {
        /* Fallback to unsorted print on allocation failure */
        for (int i = 0; i < count; i++) {
            for (int j = 0; j < indent; j++) {
                putchar(' ');
            }
            printf("%s\n", names[i]);
        }
        return;
    }

    memcpy(sorted, names, (size_t)count * sizeof(char *));
    qsort(sorted, (size_t)count, sizeof(char *), package_name_compare);

    for (int i = 0; i < count; i++) {
        for (int j = 0; j < indent; j++) {
            putchar(' ');
        }
        printf("%s\n", sorted[i]);
    }

    arena_free(sorted);
}
