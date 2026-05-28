#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include <stdbool.h>
#include <stddef.h>

/* HTTP result codes */
typedef enum {
    HTTP_OK = 0,
    HTTP_ERROR_INIT,
    HTTP_ERROR_NETWORK,
    HTTP_ERROR_TIMEOUT,
    HTTP_ERROR_4XX,
    HTTP_ERROR_5XX,
    HTTP_ERROR_MEMORY
} HttpResult;

/* Memory buffer for HTTP responses */
typedef struct {
    char *data;
    size_t len;
    size_t capacity;
} MemoryBuffer;

/* libcurl session wrapper */
typedef struct CurlSession CurlSession;

/* Session lifecycle */
CurlSession* curl_session_create(void);
void curl_session_free(CurlSession *session);
void curl_session_set_timeout(CurlSession *session, long timeout_ms);

/* Connection test */
bool curl_session_can_connect(CurlSession *session, const char *test_url);

/* HTTP operations */
HttpResult http_get_json(CurlSession *session, const char *url, MemoryBuffer *out);
HttpResult http_download_file(CurlSession *session, const char *url, const char *dest_path);
HttpResult http_head(CurlSession *session, const char *url);

/*
 * Progress callback invoked periodically during a download.
 *
 *   userdata        : opaque pointer passed to http_download_file_ex.
 *   total_bytes     : expected size of the download in bytes, or 0 if unknown.
 *   downloaded_bytes: bytes received so far.
 *
 * Return 0 to continue. Non-zero aborts the transfer.
 */
typedef int (*HttpProgressFn)(void *userdata, double total_bytes, double downloaded_bytes);

/*
 * Download `url` to `dest_path` with optional progress reporting and error
 * detail capture.
 *
 *   progress_fn / progress_userdata: may be NULL to disable progress.
 *   out_response_code              : may be NULL; receives the HTTP status
 *                                    code when curl returned data.
 *   err_buf / err_buf_size         : may be NULL/0; receives a copy of the
 *                                    curl error string on failure.
 */
HttpResult http_download_file_ex(
    CurlSession *session,
    const char *url,
    const char *dest_path,
    HttpProgressFn progress_fn,
    void *progress_userdata,
    long *out_response_code,
    char *err_buf,
    size_t err_buf_size
);

/* Conditional requests / ETag helpers
 *
 * If if_none_match is non-NULL, sends an If-None-Match header.
 * If out_not_modified is non-NULL, it is set to true when HTTP 304 is received.
 * If out_etag is non-NULL, it is set to the response ETag (arena-allocated) when provided.
 */
HttpResult http_get_json_etag(
    CurlSession *session,
    const char *url,
    const char *if_none_match,
    MemoryBuffer *out,
    char **out_etag,
    bool *out_not_modified
);

HttpResult http_head_etag(
    CurlSession *session,
    const char *url,
    const char *if_none_match,
    char **out_etag,
    bool *out_not_modified
);

/* Memory buffer operations */
MemoryBuffer* memory_buffer_create(void);
void memory_buffer_free(MemoryBuffer *buf);
void memory_buffer_clear(MemoryBuffer *buf);

/* Error reporting */
const char* http_result_to_string(HttpResult result);
const char* curl_session_get_error(CurlSession *session);

#endif /* HTTP_CLIENT_H */
