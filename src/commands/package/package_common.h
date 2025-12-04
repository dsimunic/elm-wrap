#ifndef PACKAGE_COMMON_H
#define PACKAGE_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include "../../elm_json.h"
#include "../../install_env.h"
#include "../../solver.h"

#define ELM_JSON_PATH "elm.json"

bool parse_package_name(const char *package, char **author, char **name);
Package* find_existing_package(ElmJson *elm_json, const char *author, const char *name);
bool read_package_info_from_elm_json(const char *elm_json_path, char **out_author, char **out_name, char **out_version);
char* find_package_elm_json(const char *pkg_path);
bool install_from_file(const char *source_path, InstallEnv *env, const char *author, const char *name, const char *version);
int compare_package_changes(const void *a, const void *b);

#endif /* PACKAGE_COMMON_H */
