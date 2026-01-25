#include "cache_common.h"
#include "../../alloc.h"
#include "../../constants.h"
#include "../../http_client.h"
#include "../../fileutil.h"
#include "../package/package_common.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/**
 * Download a package from GitHub and install to cache.
 * URL format: https://github.com/AUTHOR/NAME/archive/refs/tags/VERSION.zip
 */
bool cache_download_from_github(InstallEnv *env, const char *author,
                                const char *name, const char *version,
                                bool verbose, char *error_out, size_t error_size) {
    bool result = false;

    if (error_out && error_size > 0) {
        error_out[0] = '\0';
    }

    /* Build GitHub URL */
    char url[MAX_TEMP_PATH_LENGTH];
    snprintf(url, sizeof(url),
             "https://github.com/%s/%s/archive/refs/tags/%s.zip",
             author, name, version);

    /* Create temp directory */
    char temp_dir[MAX_TEMP_PATH_LENGTH];
    snprintf(temp_dir, sizeof(temp_dir), "/tmp/wrap_gh_%s_%s_%s", author, name, version);

    /* Clean up any previous attempt */
    remove_directory_recursive(temp_dir);

    if (mkdir(temp_dir, DIR_PERMISSIONS) != 0) {
        if (error_out) snprintf(error_out, error_size, "mkdir failed");
        return false;
    }

    /* Download zip file */
    char zip_path[MAX_TEMP_PATH_LENGTH];
    snprintf(zip_path, sizeof(zip_path), "%s/package.zip", temp_dir);

    if (verbose) {
        printf("\n    URL: %s\n    ", url);
        fflush(stdout);
    }

    HttpResult http_result = http_download_file(env->curl_session, url, zip_path);
    if (http_result != HTTP_OK) {
        if (error_out) snprintf(error_out, error_size, "download: %s", http_result_to_string(http_result));
        remove_directory_recursive(temp_dir);
        return false;
    }

    /* Extract zip file */
    if (!extract_zip_selective(zip_path, temp_dir)) {
        if (error_out) snprintf(error_out, error_size, "extract failed");
        remove_directory_recursive(temp_dir);
        return false;
    }

    /* Remove zip file */
    unlink(zip_path);

    /* Find elm.json - GitHub extracts to a nested directory like "name-version/" */
    char *elm_json_path = find_package_elm_json(temp_dir);
    if (!elm_json_path) {
        if (error_out) snprintf(error_out, error_size, "no elm.json found");
        remove_directory_recursive(temp_dir);
        return false;
    }

    /* Get the source directory (parent of elm.json) */
    char source_dir[MAX_PATH_LENGTH];
    char *last_slash = strrchr(elm_json_path, '/');
    if (last_slash) {
        size_t dir_len = (size_t)(last_slash - elm_json_path);
        if (dir_len >= sizeof(source_dir)) {
            dir_len = sizeof(source_dir) - 1;
        }
        memcpy(source_dir, elm_json_path, dir_len);
        source_dir[dir_len] = '\0';
    } else {
        snprintf(source_dir, sizeof(source_dir), "%s", temp_dir);
    }

    arena_free(elm_json_path);

    /* Install to cache */
    result = install_from_file(source_dir, env, author, name, version);
    if (!result && error_out) {
        snprintf(error_out, error_size, "install_from_file failed");
    }

    /* Cleanup temp directory */
    remove_directory_recursive(temp_dir);

    return result;
}
