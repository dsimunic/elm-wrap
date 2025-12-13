#include "http_client.h"
#include "alloc.h"
#include "http_constants.h"
#include "log.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#define DEFAULT_TIMEOUT_MS 10000L
#define CONNECT_TEST_TIMEOUT_MS 2000L

struct CurlSession {
    CURL *handle;
    long timeout_ms;
    struct curl_slist *headers;
    char error_buffer[CURL_ERROR_SIZE];
    char *cainfo;
};

typedef struct {
    char *etag;
} HeaderCapture;

static void curl_session_apply_defaults(CurlSession *session) {
    if (!session || !session->handle) return;

    session->error_buffer[0] = '\0';

    curl_easy_setopt(session->handle, CURLOPT_USERAGENT, "Elm/0.19.1 (libcurl)");
    curl_easy_setopt(session->handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(session->handle, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(session->handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(session->handle, CURLOPT_TIMEOUT_MS, session->timeout_ms);
    curl_easy_setopt(session->handle, CURLOPT_ERRORBUFFER, session->error_buffer);

    /* TLS verification defaults */
    curl_easy_setopt(session->handle, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(session->handle, CURLOPT_SSL_VERIFYHOST, 2L);

    if (session->cainfo) {
        curl_easy_setopt(session->handle, CURLOPT_CAINFO, session->cainfo);
    }
}

static void curl_session_prepare_request(CurlSession *session) {
    if (!session || !session->handle) return;
    curl_easy_reset(session->handle);
    curl_session_apply_defaults(session);
}

static size_t header_cb(char *buffer, size_t size, size_t nitems, void *userdata) {
    size_t total = size * nitems;
    HeaderCapture *cap = (HeaderCapture *)userdata;
    if (!buffer || total == 0 || !cap) {
        return total;
    }

    /* Match "ETag:" case-insensitively */
    const char *prefix = "etag:";
    if (total >= 5) {
        char tmp[6];
        size_t copy = (total < 5) ? total : 5;
        for (size_t i = 0; i < copy; i++) {
            tmp[i] = (char)tolower((unsigned char)buffer[i]);
        }
        tmp[copy] = '\0';

        if (strcmp(tmp, prefix) == 0) {
            char *start = buffer + 5;
            char *end = buffer + (long)total;
            while (start < end && (*start == ' ' || *start == '\t')) {
                start++;
            }
            while (end > start && (end[-1] == '\r' || end[-1] == '\n' || end[-1] == ' ' || end[-1] == '\t')) {
                end--;
            }

            size_t vlen = (size_t)(end - start);
            if (vlen > 0) {
                char *val = arena_malloc(vlen + 1);
                if (val) {
                    memcpy(val, start, vlen);
                    val[vlen] = '\0';
                    if (cap->etag) {
                        arena_free(cap->etag);
                    }
                    cap->etag = val;
                }
            }
        }
    }

    return total;
}

/* Write callback for libcurl */
static size_t write_cb(void *contents, size_t size, size_t nmemb, void *userdata) {
    size_t total_size = size * nmemb;
    MemoryBuffer *buf = (MemoryBuffer *)userdata;

    /* Ensure capacity */
    if (buf->len + total_size + 1 > buf->capacity) {
        size_t new_capacity = (buf->capacity == 0) ? 4096 : buf->capacity * 2;
        while (new_capacity < buf->len + total_size + 1) {
            new_capacity *= 2;
        }

        char *new_data = arena_realloc(buf->data, new_capacity);
        if (!new_data) {
            return 0;  /* Signal error to libcurl */
        }

        buf->data = new_data;
        buf->capacity = new_capacity;
    }

    /* Append data */
    memcpy(buf->data + buf->len, contents, total_size);
    buf->len += total_size;
    buf->data[buf->len] = '\0';  /* Null-terminate */

    return total_size;
}

/* Create session */
CurlSession* curl_session_create(void) {
    CurlSession *session = arena_calloc(1, sizeof(CurlSession));
    if (!session) return NULL;

    /* Initialize libcurl globally (safe to call multiple times) */
    curl_global_init(CURL_GLOBAL_DEFAULT);

    session->handle = curl_easy_init();
    if (!session->handle) {
        arena_free(session);
        return NULL;
    }

    session->timeout_ms = DEFAULT_TIMEOUT_MS;
    session->headers = NULL;
    session->cainfo = NULL;
    session->error_buffer[0] = '\0';

    /* Determine CA bundle path once so curl_easy_reset() doesn't lose it */
    const char *ca_bundle = getenv("CURL_CA_BUNDLE");
    if (!ca_bundle || ca_bundle[0] == '\0') {
        ca_bundle = getenv("SSL_CERT_FILE");
    }
    if (ca_bundle && ca_bundle[0] != '\0') {
        session->cainfo = arena_strdup(ca_bundle);
    } else {
        /* Try common CA bundle locations for portability across distros */
        const char *ca_paths[] = {
            "/etc/ssl/certs/ca-certificates.crt",  /* Debian/Ubuntu/Gentoo */
            "/etc/pki/tls/certs/ca-bundle.crt",    /* Fedora/RHEL */
            "/etc/ssl/ca-bundle.pem",              /* OpenSUSE */
            "/etc/ssl/cert.pem",                   /* Alpine/OpenBSD */
            "/usr/local/share/certs/ca-root-nss.crt", /* FreeBSD */
            NULL
        };

        for (int i = 0; ca_paths[i] != NULL; i++) {
            FILE *test = fopen(ca_paths[i], "r");
            if (test) {
                fclose(test);
                session->cainfo = arena_strdup(ca_paths[i]);
                break;
            }
        }
    }

    curl_session_apply_defaults(session);

    return session;
}

void curl_session_free(CurlSession *session) {
    if (!session) return;

    if (session->headers) {
        curl_slist_free_all(session->headers);
    }

    if (session->handle) {
        curl_easy_cleanup(session->handle);
    }

    if (session->cainfo) {
        arena_free(session->cainfo);
    }

    arena_free(session);
}

/* Test connection */
bool curl_session_can_connect(CurlSession *session, const char *test_url) {
    if (!session || !test_url) return false;

    CURL *handle = session->handle;

    curl_session_prepare_request(session);

    curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, (long)CONNECT_TEST_TIMEOUT_MS);
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS, (long)CONNECT_TEST_TIMEOUT_MS);
    curl_easy_setopt(handle, CURLOPT_URL, test_url);
    curl_easy_setopt(handle, CURLOPT_NOBODY, 1L);  /* HEAD request */
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, NULL);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, NULL);

    /* Perform request */
    CURLcode res = curl_easy_perform(handle);

    /* Log error for debugging */
    if (res != CURLE_OK) {
        const char *error_msg = strlen(session->error_buffer) > 0 
            ? session->error_buffer 
            : curl_easy_strerror(res);
        log_debug("Connection test failed: %s (code %d)", error_msg, res);
    }

    return (res == CURLE_OK);
}

/* File write callback for downloads */
static size_t file_write_cb(void *contents, size_t size, size_t nmemb, void *userdata) {
    size_t total_size = size * nmemb;
    FILE *file = (FILE *)userdata;

    size_t written = fwrite(contents, 1, total_size, file);
    return written;
}

/* HTTP file download */
HttpResult http_download_file(CurlSession *session, const char *url, const char *dest_path) {
    if (!session || !url || !dest_path) return HTTP_ERROR_INIT;

    CURL *handle = session->handle;

    curl_session_prepare_request(session);

    /* Open file for writing */
    FILE *file = fopen(dest_path, "wb");
    if (!file) {
        fprintf(stderr, "Error: Failed to open file for writing: %s\n", dest_path);
        return HTTP_ERROR_INIT;
    }

    /* Configure request */
    curl_easy_setopt(handle, CURLOPT_URL, url);
    curl_easy_setopt(handle, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, file_write_cb);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, file);

    /* Perform request */
    CURLcode res = curl_easy_perform(handle);

    /* Close file */
    fclose(file);

    if (res != CURLE_OK) {
        /* Remove partial file on error */
        remove(dest_path);

        if (res == CURLE_OPERATION_TIMEDOUT) {
            return HTTP_ERROR_TIMEOUT;
        }
        return HTTP_ERROR_NETWORK;
    }

    /* Check HTTP response code */
    long response_code;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &response_code);

    if (http_is_success(response_code)) {
        return HTTP_OK;
    } else if (http_is_client_error(response_code)) {
        remove(dest_path);
        return HTTP_ERROR_4XX;
    } else if (http_is_server_error(response_code)) {
        remove(dest_path);
        return HTTP_ERROR_5XX;
    }

    return HTTP_ERROR_NETWORK;
}

/* HTTP HEAD request */
HttpResult http_head(CurlSession *session, const char *url) {
    if (!session || !url) return HTTP_ERROR_INIT;

    CURL *handle = session->handle;

    curl_session_prepare_request(session);

    curl_easy_setopt(handle, CURLOPT_URL, url);
    curl_easy_setopt(handle, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, NULL);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, NULL);

    CURLcode res = curl_easy_perform(handle);

    if (res != CURLE_OK) {
        if (res == CURLE_OPERATION_TIMEDOUT) {
            return HTTP_ERROR_TIMEOUT;
        }
        return HTTP_ERROR_NETWORK;
    }

    long response_code;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &response_code);

    if (http_is_success(response_code)) {
        return HTTP_OK;
    } else if (http_is_client_error(response_code)) {
        return HTTP_ERROR_4XX;
    } else if (http_is_server_error(response_code)) {
        return HTTP_ERROR_5XX;
    }

    return HTTP_ERROR_NETWORK;
}

HttpResult http_head_etag(
    CurlSession *session,
    const char *url,
    const char *if_none_match,
    char **out_etag,
    bool *out_not_modified
) {
    if (out_not_modified) {
        *out_not_modified = false;
    }
    if (out_etag) {
        *out_etag = NULL;
    }

    if (!session || !url) return HTTP_ERROR_INIT;

    CURL *handle = session->handle;
    curl_session_prepare_request(session);

    HeaderCapture cap = {0};

    struct curl_slist *hdrs = NULL;
    char *header_line = NULL;
    if (if_none_match && if_none_match[0] != '\0') {
        size_t hlen = strlen("If-None-Match: ") + strlen(if_none_match) + 1;
        header_line = arena_malloc(hlen);
        if (!header_line) {
            return HTTP_ERROR_MEMORY;
        }
        snprintf(header_line, hlen, "If-None-Match: %s", if_none_match);
        hdrs = curl_slist_append(hdrs, header_line);
        curl_easy_setopt(handle, CURLOPT_HTTPHEADER, hdrs);
    }

    curl_easy_setopt(handle, CURLOPT_URL, url);
    curl_easy_setopt(handle, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, NULL);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, NULL);
    curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(handle, CURLOPT_HEADERDATA, &cap);

    CURLcode res = curl_easy_perform(handle);

    if (hdrs) {
        curl_slist_free_all(hdrs);
    }
    if (header_line) {
        arena_free(header_line);
    }

    if (res != CURLE_OK) {
        if (res == CURLE_OPERATION_TIMEDOUT) {
            return HTTP_ERROR_TIMEOUT;
        }
        return HTTP_ERROR_NETWORK;
    }

    long response_code;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &response_code);
    if (response_code == 304) {
        if (out_not_modified) {
            *out_not_modified = true;
        }
        if (out_etag && cap.etag) {
            *out_etag = cap.etag;
        }
        return HTTP_OK;
    }

    if (out_etag && cap.etag) {
        *out_etag = cap.etag;
    }

    if (http_is_success(response_code)) {
        return HTTP_OK;
    } else if (http_is_client_error(response_code)) {
        return HTTP_ERROR_4XX;
    } else if (http_is_server_error(response_code)) {
        return HTTP_ERROR_5XX;
    }

    return HTTP_ERROR_NETWORK;
}

/* HTTP GET with JSON response */
HttpResult http_get_json(CurlSession *session, const char *url, MemoryBuffer *out) {
    if (!session || !url || !out) return HTTP_ERROR_INIT;

    CURL *handle = session->handle;

    curl_session_prepare_request(session);

    /* Clear output buffer */
    memory_buffer_clear(out);

    /* Configure request */
    curl_easy_setopt(handle, CURLOPT_URL, url);
    curl_easy_setopt(handle, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, out);

    /* Perform request */
    CURLcode res = curl_easy_perform(handle);

    if (res != CURLE_OK) {
        if (res == CURLE_OPERATION_TIMEDOUT) {
            return HTTP_ERROR_TIMEOUT;
        }
        return HTTP_ERROR_NETWORK;
    }

    /* Check HTTP response code */
    long response_code;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &response_code);

    if (http_is_success(response_code)) {
        return HTTP_OK;
    } else if (http_is_client_error(response_code)) {
        return HTTP_ERROR_4XX;
    } else if (http_is_server_error(response_code)) {
        return HTTP_ERROR_5XX;
    }

    return HTTP_ERROR_NETWORK;
}

HttpResult http_get_json_etag(
    CurlSession *session,
    const char *url,
    const char *if_none_match,
    MemoryBuffer *out,
    char **out_etag,
    bool *out_not_modified
) {
    if (out_not_modified) {
        *out_not_modified = false;
    }
    if (out_etag) {
        *out_etag = NULL;
    }

    if (!session || !url || !out) return HTTP_ERROR_INIT;

    CURL *handle = session->handle;
    curl_session_prepare_request(session);

    HeaderCapture cap = {0};

    /* Clear output buffer */
    memory_buffer_clear(out);

    struct curl_slist *hdrs = NULL;
    char *header_line = NULL;
    if (if_none_match && if_none_match[0] != '\0') {
        size_t hlen = strlen("If-None-Match: ") + strlen(if_none_match) + 1;
        header_line = arena_malloc(hlen);
        if (!header_line) {
            return HTTP_ERROR_MEMORY;
        }
        snprintf(header_line, hlen, "If-None-Match: %s", if_none_match);
        hdrs = curl_slist_append(hdrs, header_line);
        curl_easy_setopt(handle, CURLOPT_HTTPHEADER, hdrs);
    }

    curl_easy_setopt(handle, CURLOPT_URL, url);
    curl_easy_setopt(handle, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(handle, CURLOPT_HEADERDATA, &cap);

    CURLcode res = curl_easy_perform(handle);

    if (hdrs) {
        curl_slist_free_all(hdrs);
    }
    if (header_line) {
        arena_free(header_line);
    }

    if (res != CURLE_OK) {
        if (res == CURLE_OPERATION_TIMEDOUT) {
            return HTTP_ERROR_TIMEOUT;
        }
        return HTTP_ERROR_NETWORK;
    }

    long response_code;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &response_code);

    if (out_etag && cap.etag) {
        *out_etag = cap.etag;
    }

    if (response_code == 304) {
        if (out_not_modified) {
            *out_not_modified = true;
        }
        return HTTP_OK;
    }

    if (http_is_success(response_code)) {
        return HTTP_OK;
    } else if (http_is_client_error(response_code)) {
        return HTTP_ERROR_4XX;
    } else if (http_is_server_error(response_code)) {
        return HTTP_ERROR_5XX;
    }

    return HTTP_ERROR_NETWORK;
}

/* Memory buffer operations */
MemoryBuffer* memory_buffer_create(void) {
    MemoryBuffer *buf = arena_calloc(1, sizeof(MemoryBuffer));
    return buf;
}

void memory_buffer_free(MemoryBuffer *buf) {
    if (!buf) return;

    arena_free(buf->data);
    arena_free(buf);
}

void memory_buffer_clear(MemoryBuffer *buf) {
    if (!buf) return;

    buf->len = 0;
    if (buf->data) {
        buf->data[0] = '\0';
    }
}

/* Error reporting */
const char* http_result_to_string(HttpResult result) {
    switch (result) {
        case HTTP_OK: return "Success";
        case HTTP_ERROR_INIT: return "Initialization error";
        case HTTP_ERROR_NETWORK: return "Network error";
        case HTTP_ERROR_TIMEOUT: return "Request timeout";
        case HTTP_ERROR_4XX: return "Client error (4xx)";
        case HTTP_ERROR_5XX: return "Server error (5xx)";
        case HTTP_ERROR_MEMORY: return "Memory allocation error";
        default: return "Unknown error";
    }
}

const char* curl_session_get_error(CurlSession *session) {
    if (!session) return "No session";

    if (session->error_buffer[0] != '\0') {
        return session->error_buffer;
    }

    return "No error details available";
}
