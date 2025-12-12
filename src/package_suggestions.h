#ifndef PACKAGE_SUGGESTIONS_H
#define PACKAGE_SUGGESTIONS_H

#include <stddef.h>
#include <stdint.h>

struct InstallEnv;

#define MAX_PACKAGE_SUGGESTIONS 4

typedef struct {
    const char *author;
    const char *name;
    uint32_t score;
} PackageSuggestion;

size_t package_suggest_nearby_from_env(
    const struct InstallEnv *env,
    const char *given_author,
    const char *given_name,
    PackageSuggestion out_suggestions[MAX_PACKAGE_SUGGESTIONS]
);

#endif /* PACKAGE_SUGGESTIONS_H */
