#include "http_client.h"
#include "alloc.h"
#include "http_constants.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define DEFAULT_TIMEOUT_MS 10000
#define CONNECT_TEST_TIMEOUT_MS 2000

struct CurlSession {
    CURL *handle;
    long timeout_ms;
    struct curl_slist *headers;
    char error_buffer[CURL_ERROR_SIZE];
};

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
    session->error_buffer[0] = '\0';

    /* Configure session defaults */
    curl_easy_setopt(session->handle, CURLOPT_USERAGENT, "Elm/0.19.1 (libcurl)");
    curl_easy_setopt(session->handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(session->handle, CURLOPT_ACCEPT_ENCODING, "");  /* Accept all supported encodings */
    curl_easy_setopt(session->handle, CURLOPT_NOSIGNAL, 1L);  /* Thread-safe */
    curl_easy_setopt(session->handle, CURLOPT_TIMEOUT_MS, session->timeout_ms);
    curl_easy_setopt(session->handle, CURLOPT_ERRORBUFFER, session->error_buffer);

    /* TLS verification defaults */
    curl_easy_setopt(session->handle, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(session->handle, CURLOPT_SSL_VERIFYHOST, 2L);

    /* Respect environment variables for CA bundle */
    const char *ca_bundle = getenv("CURL_CA_BUNDLE");
    if (!ca_bundle) {
        ca_bundle = getenv("SSL_CERT_FILE");
    }
    if (ca_bundle) {
        curl_easy_setopt(session->handle, CURLOPT_CAINFO, ca_bundle);
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
                curl_easy_setopt(session->handle, CURLOPT_CAINFO, ca_paths[i]);
                break;
            }
        }
    }

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

    arena_free(session);
}

/* Test connection */
bool curl_session_can_connect(CurlSession *session, const char *test_url) {
    if (!session || !test_url) return false;

    CURL *handle = session->handle;

    /* Save current timeout */
    long saved_timeout = session->timeout_ms;

    /* Set short timeout for connectivity test */
    curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, CONNECT_TEST_TIMEOUT_MS);
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS, CONNECT_TEST_TIMEOUT_MS);
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
        fprintf(stderr, "DEBUG: Connection test failed: %s (code %d)\n", error_msg, res);
    }

    /* Restore timeout */
    curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, saved_timeout);
    curl_easy_setopt(handle, CURLOPT_NOBODY, 0L);

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

    curl_easy_setopt(handle, CURLOPT_URL, url);
    curl_easy_setopt(handle, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, NULL);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, NULL);

    CURLcode res = curl_easy_perform(handle);

    /* Reset to GET */
    curl_easy_setopt(handle, CURLOPT_NOBODY, 0L);

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

/* HTTP GET with JSON response */
HttpResult http_get_json(CurlSession *session, const char *url, MemoryBuffer *out) {
    if (!session || !url || !out) return HTTP_ERROR_INIT;

    CURL *handle = session->handle;

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
