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
