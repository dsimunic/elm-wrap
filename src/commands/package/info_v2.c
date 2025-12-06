#include "info_v2.h"
#include "package_common.h"
#include "../../install.h"
#include "../../install_check.h"
#include "../../elm_json.h"
#include "../../elm_project.h"
#include "../../install_env.h"
#include "../../protocol_v2/install.h"
#include "../../protocol_v2/solver/v2_registry.h"
#include "../../alloc.h"
#include "../../constants.h"
#include "../../log.h"
#include "../../fileutil.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <limits.h>
#include <dirent.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

int cmd_info_v2(const char *author, const char *name, const char *version_arg, InstallEnv *env, V2PackageEntry *entry) {
    const char *version_to_use = NULL;
    bool version_found = false;
    char *allocated_version = NULL;

    if (version_arg) {
        for (size_t i = 0; i < entry->version_count; i++) {
            V2PackageVersion *v = &entry->versions[i];
            if (v->status == V2_STATUS_VALID) {
                char *v_str = arena_malloc(MAX_VERSION_STRING_LENGTH);
                snprintf(v_str, MAX_VERSION_STRING_LENGTH, "%u.%u.%u", v->major, v->minor, v->patch);
                if (strcmp(v_str, version_arg) == 0) {
                    version_to_use = version_arg;
                    version_found = true;
                    arena_free(v_str);
                    break;
                }
                arena_free(v_str);
            }
        }

        if (!version_found) {
            fprintf(stderr, "Error: Version %s not found for package %s/%s\n", version_arg, author, name);
            printf("\nAvailable versions:\n");
            for (size_t i = 0; i < entry->version_count; i++) {
                V2PackageVersion *v = &entry->versions[i];
                if (v->status == V2_STATUS_VALID) {
                    printf("  %u.%u.%u\n", v->major, v->minor, v->patch);
                }
            }
            printf("\n");
            arena_free((char*)author);
            arena_free((char*)name);
            return 1;
        }
    }

    if (!version_found) {
        for (size_t i = 0; i < entry->version_count; i++) {
            V2PackageVersion *v = &entry->versions[i];
            if (v->status == V2_STATUS_VALID) {
                allocated_version = arena_malloc(MAX_VERSION_STRING_LENGTH);
                snprintf(allocated_version, MAX_VERSION_STRING_LENGTH, "%u.%u.%u", v->major, v->minor, v->patch);
                version_to_use = allocated_version;
                version_found = true;
                break;
            }
        }
    }

    if (!version_found || !version_to_use) {
        fprintf(stderr, "Error: Could not determine version for %s/%s\n", author, name);
        arena_free((char*)author);
        arena_free((char*)name);
        return 1;
    }

    /* Check if this is a local-dev package */
    int v_major, v_minor, v_patch;
    bool is_local_dev = false;
    if (sscanf(version_to_use, "%d.%d.%d", &v_major, &v_minor, &v_patch) == 3) {
        is_local_dev = is_local_dev_version(v_major, v_minor, v_patch);
    }

    char latest_buf[32];
    for (size_t i = 0; i < entry->version_count; i++) {
        V2PackageVersion *v = &entry->versions[i];
        if (v->status == V2_STATUS_VALID) {
            snprintf(latest_buf, sizeof(latest_buf), "%u.%u.%u", v->major, v->minor, v->patch);
            break;
        }
    }

    printf("\nPackage: %s/%s\n", author, name);
    if (is_local_dev) {
        printf("Version: %s (local development)\n", version_to_use);
    } else {
        printf("Version: %s\n", version_to_use);
    }
    if (strcmp(version_to_use, latest_buf) != 0) {
        printf("Latest version: %s\n", latest_buf);
    }
    printf("Total versions: %zu\n", entry->version_count);
    printf("\n");

    int result = v2_show_package_dependencies(author, name, version_to_use, env->v2_registry);

    /* Show local development tracking information */
    if (is_local_dev) {
        print_package_tracking_info(author, name, version_to_use);
    }

    if (allocated_version) {
        arena_free(allocated_version);
    }
    arena_free((char*)author);
    arena_free((char*)name);
    return result;
}
