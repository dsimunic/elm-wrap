/**
 * index_fetch.c - V2 Registry Index Download
 *
 * Downloads the full registry index file for a specific compiler/version
 * combination from the V2 registry protocol URL.
 */

#include "index_fetch.h"
#include "../alloc.h"
#include "../buildinfo.h"
#include "../constants.h"
#include "../env_defaults.h"
#include "../global_context.h"
#include "../http_client.h"
#include "../http_constants.h"
#include "../log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <sys/time.h>
#include <errno.h>

/* Progress reporting state */
typedef struct {
    struct timeval start_time;
    bool progress_started;
    curl_off_t last_reported_bytes;
} ProgressState;

/* Progress callback for curl */
static int progress_callback(void *clientp,
                            curl_off_t dltotal, curl_off_t dlnow,
                            curl_off_t ultotal, curl_off_t ulnow) {
    (void)ultotal;
    (void)ulnow;

    ProgressState *state = (ProgressState *)clientp;
    if (!state) return 0;

    struct timeval now;
    gettimeofday(&now, NULL);

    double elapsed = (now.tv_sec - state->start_time.tv_sec) +
                     (now.tv_usec - state->start_time.tv_usec) / MICROSECONDS_PER_SECOND;

    /* If download is taking longer than 1 second, show progress */
    if (elapsed >= 1.0 && dlnow > 0) {
        if (!state->progress_started) {
            /* First progress report - announce file size if known */
            if (dltotal > 0) {
                printf("Downloading index (%.1f KB)...", (double)dltotal / BYTES_PER_KB);
            } else {
                printf("Downloading index...");
            }
            fflush(stdout);
            state->progress_started = true;
            state->last_reported_bytes = dlnow;
        } else {
            /* Show dots for progress (every 50KB) */
            curl_off_t bytes_per_dot = PROGRESS_BYTES_PER_DOT;
            curl_off_t dots_now = dlnow / bytes_per_dot;
            curl_off_t dots_last = state->last_reported_bytes / bytes_per_dot;
            for (curl_off_t i = dots_last; i < dots_now; i++) {
                printf(".");
                fflush(stdout);
            }
            state->last_reported_bytes = dlnow;
        }
    }

    return 0;
}

/* File write callback */
static size_t file_write_cb(void *contents, size_t size, size_t nmemb, void *userdata) {
    size_t total_size = size * nmemb;
    FILE *file = (FILE *)userdata;
    return fwrite(contents, 1, total_size, file);
}

bool v2_index_fetch(const char *repo_path, const char *compiler, const char *version) {
    if (!repo_path || !compiler || !version) {
        log_error("Invalid arguments to v2_index_fetch");
        return false;
    }

    /* Skip registry index download if WRAP_SKIP_REGISTRY_UPDATE=1 */
    if (global_context_skip_registry_update()) {
        log_progress("Skipping registry index download (WRAP_SKIP_REGISTRY_UPDATE=1)");
        return true;
    }

    /* Get base URL from environment/defaults */
    char *base_url = env_get_registry_v2_full_index_url();
    if (!base_url || base_url[0] == '\0') {
        log_error("WRAP_REGISTRY_V2_FULL_INDEX_URL is not configured");
        return false;
    }

    /* Strip trailing slash from base URL if present */
    size_t base_len = strlen(base_url);
    while (base_len > 0 && base_url[base_len - 1] == '/') {
        base_url[base_len - 1] = '\0';
        base_len--;
    }

    /* Build the full URL: <base_url>/index_<compiler>_<version> */
    size_t url_len = strlen(base_url) + 1 + strlen("index_") + strlen(compiler) + 1 + strlen(version) + 1;
    char *url = arena_malloc(url_len);
    if (!url) {
        log_error("Out of memory allocating URL");
        arena_free(base_url);
        return false;
    }
    snprintf(url, url_len, "%s/index_%s_%s", base_url, compiler, version);

    /* Build destination path: <repo_path>/index.dat */
    size_t dest_len = strlen(repo_path) + strlen("/index.dat") + 1;
    char *dest_path = arena_malloc(dest_len);
    if (!dest_path) {
        log_error("Out of memory allocating destination path");
        arena_free(base_url);
        arena_free(url);
        return false;
    }
    snprintf(dest_path, dest_len, "%s/index.dat", repo_path);

    /* Initialize curl */
    curl_global_init(CURL_GLOBAL_DEFAULT);
    CURL *curl = curl_easy_init();
    if (!curl) {
        log_error("Failed to initialize curl");
        arena_free(base_url);
        arena_free(url);
        arena_free(dest_path);
        return false;
    }

    /* Open destination file */
    FILE *file = fopen(dest_path, "wb");
    if (!file) {
        log_error("Failed to open %s for writing: %s", dest_path, strerror(errno));
        curl_easy_cleanup(curl);
        arena_free(base_url);
        arena_free(url);
        arena_free(dest_path);
        return false;
    }

    /* Initialize progress state */
    ProgressState progress_state = {0};
    gettimeofday(&progress_state.start_time, NULL);
    progress_state.progress_started = false;
    progress_state.last_reported_bytes = 0;

    /* Configure curl */
    char error_buffer[CURL_ERROR_SIZE] = {0};
    char user_agent[MAX_USER_AGENT_LENGTH];
    /* We'll stick to `elm-wrap` user agent identifier regardless of the actual command binary's name */
    snprintf(user_agent, sizeof(user_agent), "elm-wrap/%s", build_base_version);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, file_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    /* Enable progress callback */
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progress_state);

    /* Perform the download */
    printf("Downloading registry index from %s\n", url);

    CURLcode res = curl_easy_perform(curl);

    /* Close file */
    fclose(file);

    /* Finish progress line if started */
    if (progress_state.progress_started) {
        printf(" done\n");
    }

    bool success = false;

    if (res != CURLE_OK) {
        const char *err_msg = (error_buffer[0] != '\0') ? error_buffer : curl_easy_strerror(res);
        log_error("Download failed: %s", err_msg);
        /* Remove partial file */
        remove(dest_path);
    } else {
        /* Check HTTP response code */
        long response_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

        if (http_is_success(response_code)) {
            /* Get downloaded file size */
            curl_off_t dl_size = 0;
            curl_easy_getinfo(curl, CURLINFO_SIZE_DOWNLOAD_T, &dl_size);
            printf("Downloaded index.dat (%.1f KB)\n", (double)dl_size / BYTES_PER_KB);
            success = true;
        } else if (http_is_client_error(response_code)) {
            log_error("Index not found: HTTP %ld (URL: %s)", response_code, url);
            remove(dest_path);
        } else {
            log_error("Server error: HTTP %ld", response_code);
            remove(dest_path);
        }
    }

    /* Cleanup */
    curl_easy_cleanup(curl);
    arena_free(base_url);
    arena_free(url);
    arena_free(dest_path);

    return success;
}
