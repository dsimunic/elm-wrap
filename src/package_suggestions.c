#include "package_suggestions.h"
#include "install_env.h"
#include "alloc.h"
#include "registry.h"
#include "protocol_v2/solver/v2_registry.h"
#include <stdint.h>
#include <string.h>

static uint32_t osa_distance_bytes(const char *a, const char *b) {
    if (!a || !b) {
        return 0;
    }

    size_t len_a = strlen(a);
    size_t len_b = strlen(b);

    if (len_a == 0) {
        return (uint32_t)len_b;
    }
    if (len_b == 0) {
        return (uint32_t)len_a;
    }

    uint32_t *row_prev = arena_calloc(len_b + 1, sizeof(uint32_t));
    uint32_t *row_curr = arena_calloc(len_b + 1, sizeof(uint32_t));
    uint32_t *row_prev2 = arena_calloc(len_b + 1, sizeof(uint32_t));

    if (!row_prev || !row_curr || !row_prev2) {
        if (row_prev) arena_free(row_prev);
        if (row_curr) arena_free(row_curr);
        if (row_prev2) arena_free(row_prev2);
        return (uint32_t)(len_a + len_b);
    }

    for (size_t j = 0; j <= len_b; j++) {
        row_prev[j] = (uint32_t)j;
    }

    for (size_t i = 1; i <= len_a; i++) {
        row_curr[0] = (uint32_t)i;

        for (size_t j = 1; j <= len_b; j++) {
            uint32_t cost = (a[i - 1] == b[j - 1]) ? 0u : 1u;

            uint32_t deletion = row_prev[j] + 1u;
            uint32_t insertion = row_curr[j - 1] + 1u;
            uint32_t substitution = row_prev[j - 1] + cost;

            uint32_t best = deletion;
            if (insertion < best) best = insertion;
            if (substitution < best) best = substitution;

            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1]) {
                uint32_t transposition = row_prev2[j - 2] + 1u;
                if (transposition < best) {
                    best = transposition;
                }
            }

            row_curr[j] = best;
        }

        uint32_t *tmp = row_prev2;
        row_prev2 = row_prev;
        row_prev = row_curr;
        row_curr = tmp;
    }

    uint32_t distance = row_prev[len_b];

    arena_free(row_prev);
    arena_free(row_prev2);
    arena_free(row_curr);

    return distance;
}

static uint32_t author_distance(const char *given_author, const char *candidate_author) {
    if (!candidate_author) {
        return 0;
    }

    if (strcmp(candidate_author, "elm") == 0 || strcmp(candidate_author, "elm-explorations") == 0) {
        return 0;
    }

    if (!given_author) {
        return 0;
    }

    return osa_distance_bytes(given_author, candidate_author);
}

static void top_suggestions_insert(
    PackageSuggestion *best,
    size_t *count,
    const char *author,
    const char *name,
    uint32_t score
) {
    if (!best || !count) {
        return;
    }

    size_t current = *count;
    size_t pos = current;

    for (size_t i = 0; i < current; i++) {
        if (score < best[i].score) {
            pos = i;
            break;
        }
    }

    if (pos == current && current >= MAX_PACKAGE_SUGGESTIONS) {
        pos = MAX_PACKAGE_SUGGESTIONS - 1;
    }

    size_t new_count = current < MAX_PACKAGE_SUGGESTIONS ? current + 1 : MAX_PACKAGE_SUGGESTIONS;

    if (pos > new_count - 1) {
        pos = new_count - 1;
    }

    for (size_t i = new_count - 1; i > pos; i--) {
        best[i] = best[i - 1];
    }

    best[pos].author = author;
    best[pos].name = name;
    best[pos].score = score;

    *count = new_count;
}

static void consider_candidate(
    PackageSuggestion *best,
    size_t *count,
    const char *given_author,
    const char *given_name,
    const char *cand_author,
    const char *cand_name
) {
    if (!cand_author || !cand_name) {
        return;
    }

    uint32_t project_distance = osa_distance_bytes(given_name, cand_name);
    uint32_t score = author_distance(given_author, cand_author) + project_distance;

    size_t current = *count;
    if (current == MAX_PACKAGE_SUGGESTIONS && score >= best[MAX_PACKAGE_SUGGESTIONS - 1].score) {
        return;
    }

    top_suggestions_insert(best, count, cand_author, cand_name, score);
}

static int v2_entry_has_valid_version(const V2PackageEntry *entry) {
    if (!entry || entry->version_count == 0) {
        return 0;
    }
    for (size_t i = 0; i < entry->version_count; i++) {
        if (entry->versions[i].status == V2_STATUS_VALID) {
            return 1;
        }
    }
    return 0;
}

size_t package_suggest_nearby_from_env(
    const InstallEnv *env,
    const char *given_author,
    const char *given_name,
    PackageSuggestion out_suggestions[MAX_PACKAGE_SUGGESTIONS]
) {
    if (!env || !given_author || !given_name || !out_suggestions) {
        return 0;
    }

    size_t suggestion_count = 0;

    if (env->protocol_mode == PROTOCOL_V2) {
        if (!env->v2_registry || !env->v2_registry->entries) {
            return 0;
        }

        for (size_t i = 0; i < env->v2_registry->entry_count; i++) {
            V2PackageEntry *entry = &env->v2_registry->entries[i];
            if (!v2_entry_has_valid_version(entry)) {
                continue;
            }
            consider_candidate(out_suggestions, &suggestion_count,
                               given_author, given_name,
                               entry->author, entry->name);
        }
    } else {
        if (!env->registry || !env->registry->entries) {
            return 0;
        }

        for (size_t i = 0; i < env->registry->entry_count; i++) {
            RegistryEntry *entry = &env->registry->entries[i];
            consider_candidate(out_suggestions, &suggestion_count,
                               given_author, given_name,
                               entry->author, entry->name);
        }
    }

    return suggestion_count;
}
