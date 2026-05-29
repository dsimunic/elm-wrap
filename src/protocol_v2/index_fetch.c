/**
 * index_fetch.c - V2 Registry Index Download
 *
 * Downloads the full registry index file for a specific compiler/version
 * combination from the V2 registry protocol URL.
 *
 * Networking goes through the http_client.h abstraction (libcurl on
 * macOS/Linux, WinHTTP on Windows) so this code path is identical across
 * all three platforms.
 */

#include "index_fetch.h"
#include "../alloc.h"
#include "../constants.h"
#include "../env_defaults.h"
#include "../global_context.h"
#include "../http_client.h"
#include "../shared/log.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>

/* Progress reporting state (timed dots, shared look with the rest of wrap) */
typedef struct {
    struct timeval start_time;
    bool progress_started;
    double last_reported_bytes;
} ProgressState;

/*
 * HttpProgressFn implementation: once the download has been running for more
 * than a second, announce the size (if known) and then emit a dot per 50 KB.
 */
static int index_progress(void *userdata, double total_bytes, double downloaded_bytes) {
    ProgressState *state = (ProgressState *)userdata;
    if (!state) return 0;

    struct timeval now;
    gettimeofday(&now, NULL);

    double elapsed = (now.tv_sec - state->start_time.tv_sec) +
                     (now.tv_usec - state->start_time.tv_usec) / MICROSECONDS_PER_SECOND;

    if (elapsed >= 1.0 && downloaded_bytes > 0) {
        if (!state->progress_started) {
            if (total_bytes > 0) {
                printf("Downloading index (%.1f KB)...", total_bytes / BYTES_PER_KB);
            } else {
                printf("Downloading index...");
            }
            fflush(stdout);
            state->progress_started = true;
            state->last_reported_bytes = downloaded_bytes;
        } else {
            double bytes_per_dot = PROGRESS_BYTES_PER_DOT;
            long dots_now = (long)(downloaded_bytes / bytes_per_dot);
            long dots_last = (long)(state->last_reported_bytes / bytes_per_dot);
            for (long i = dots_last; i < dots_now; i++) {
                printf(".");
                fflush(stdout);
            }
            state->last_reported_bytes = downloaded_bytes;
        }
    }

    return 0;
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

    CurlSession *session = curl_session_create();
    if (!session) {
        log_error("Failed to initialize HTTP client");
        arena_free(base_url);
        arena_free(url);
        arena_free(dest_path);
        return false;
    }

    ProgressState progress_state = {0};
    gettimeofday(&progress_state.start_time, NULL);

    printf("Downloading registry index from %s\n", url);

    long response_code = 0;
    char err_buf[MAX_ERROR_MESSAGE_LENGTH];
    err_buf[0] = '\0';

    /* http_download_file_ex removes the partial file on any failure. */
    HttpResult res = http_download_file_ex(session, url, dest_path,
                                           index_progress, &progress_state,
                                           &response_code, err_buf, sizeof(err_buf));

    /* Finish progress line if started */
    if (progress_state.progress_started) {
        printf(" done\n");
    }

    bool success = false;

    if (res == HTTP_OK) {
        struct stat st;
        if (stat(dest_path, &st) == 0) {
            printf("Downloaded index.dat (%.1f KB)\n", (double)st.st_size / BYTES_PER_KB);
        } else {
            printf("Downloaded index.dat\n");
        }
        success = true;
    } else if (res == HTTP_ERROR_4XX) {
        log_error("Index not found: HTTP %ld (URL: %s)", response_code, url);
    } else if (res == HTTP_ERROR_5XX) {
        log_error("Server error: HTTP %ld", response_code);
    } else {
        log_error("Download failed: %s",
                  err_buf[0] != '\0' ? err_buf : http_result_to_string(res));
    }

    curl_session_free(session);
    arena_free(base_url);
    arena_free(url);
    arena_free(dest_path);

    return success;
}
