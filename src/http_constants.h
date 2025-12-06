#ifndef HTTP_CONSTANTS_H
#define HTTP_CONSTANTS_H

/*
 * HTTP status code range constants
 */
#define HTTP_SUCCESS_MIN 200
#define HTTP_SUCCESS_MAX 299
#define HTTP_CLIENT_ERROR_MIN 400
#define HTTP_CLIENT_ERROR_MAX 499
#define HTTP_SERVER_ERROR_MIN 500

/*
 * Helper functions for HTTP status code checking
 */
static inline int http_is_success(long code) {
    return code >= HTTP_SUCCESS_MIN && code <= HTTP_SUCCESS_MAX;
}

static inline int http_is_client_error(long code) {
    return code >= HTTP_CLIENT_ERROR_MIN && code <= HTTP_CLIENT_ERROR_MAX;
}

static inline int http_is_server_error(long code) {
    return code >= HTTP_SERVER_ERROR_MIN;
}

#endif /* HTTP_CONSTANTS_H */
