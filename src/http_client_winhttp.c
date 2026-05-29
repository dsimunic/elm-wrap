/**
 * http_client_winhttp.c - Windows networking backend (WinHTTP)
 *
 * Implements the http_client.h contract using the native WinHTTP API instead
 * of libcurl. This keeps wrap.exe dependency-free (no curl DLL to ship) and
 * uses Schannel + the Windows certificate store for TLS automatically.
 *
 * The libcurl backend lives in http_client.c; the Makefile compiles exactly
 * one of the two into http_client.o depending on the target OS. The public
 * interface (and the curl_session_* names) are identical on every platform.
 *
 * Proxy/offline behaviour is intentionally NOT replicated here: WinHTTP uses
 * the system proxy configuration, and we do not honour the https_proxy env
 * var the way libcurl does (by request, this is fine for elm-wrap).
 */

#ifdef _WIN32

#include "http_client.h"
#include "alloc.h"
#include "constants.h"
#include "http_constants.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>

#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define DEFAULT_TIMEOUT_MS 10000L
#define CONNECT_TEST_TIMEOUT_MS 2000L
#define WINHTTP_READ_CHUNK 16384
#define MEMBUF_INITIAL_CAPACITY 4096

struct CurlSession {
    HINTERNET hSession;
    long timeout_ms;
    char error_buffer[MAX_ERROR_MESSAGE_LENGTH];
};

/* ---- small UTF-8 <-> UTF-16 helpers (arena-allocated) ------------------- */

static wchar_t *utf8_to_wide(const char *s) {
    if (!s) return NULL;
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = arena_malloc((size_t)n * sizeof(wchar_t));
    if (!w) return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0) {
        arena_free(w);
        return NULL;
    }
    return w;
}

/* wlen in wchar_t units, or -1 for NUL-terminated input. */
static char *wide_to_utf8(const wchar_t *w, int wlen) {
    if (!w) return NULL;
    int n = WideCharToMultiByte(CP_UTF8, 0, w, wlen, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    char *s = arena_malloc((size_t)n + 1);
    if (!s) return NULL;
    int got = WideCharToMultiByte(CP_UTF8, 0, w, wlen, s, n, NULL, NULL);
    if (got <= 0) {
        arena_free(s);
        return NULL;
    }
    s[got] = '\0';
    return s;
}

/* Format a Windows/WinHTTP error code into session->error_buffer and err_buf. */
static void set_error(CurlSession *session, char *err_buf, size_t err_buf_size,
                      DWORD code, const char *ctx) {
    char msg[MAX_ERROR_MESSAGE_LENGTH];
    wchar_t wbuf[512];
    HMODULE winhttp = GetModuleHandleW(L"winhttp.dll");
    DWORD n = 0;

    if (winhttp) {
        n = FormatMessageW(FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS,
                           winhttp, code, 0, wbuf, (DWORD)(sizeof(wbuf) / sizeof(wbuf[0])), NULL);
    }
    if (n == 0) {
        n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                           NULL, code, 0, wbuf, (DWORD)(sizeof(wbuf) / sizeof(wbuf[0])), NULL);
    }

    if (n == 0) {
        snprintf(msg, sizeof(msg), "%s: error %lu",
                 ctx ? ctx : "request", (unsigned long)code);
    } else {
        while (n > 0 && (wbuf[n - 1] == L'\r' || wbuf[n - 1] == L'\n' || wbuf[n - 1] == L' ')) {
            wbuf[--n] = 0;
        }
        char *u = wide_to_utf8(wbuf, -1);
        snprintf(msg, sizeof(msg), "%s: %s", ctx ? ctx : "request", u ? u : "");
        if (u) arena_free(u);
    }

    if (session) {
        snprintf(session->error_buffer, sizeof(session->error_buffer), "%s", msg);
    }
    if (err_buf && err_buf_size > 0) {
        snprintf(err_buf, err_buf_size, "%s", msg);
    }
}

/* Query a string response header, returned as an arena-allocated UTF-8 string. */
static char *query_header_str(HINTERNET hRequest, DWORD level) {
    DWORD len = 0;
    WinHttpQueryHeaders(hRequest, level, WINHTTP_HEADER_NAME_BY_INDEX,
                        NULL, &len, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || len == 0) {
        return NULL;
    }
    wchar_t *wbuf = arena_malloc(len + sizeof(wchar_t));
    if (!wbuf) return NULL;
    if (!WinHttpQueryHeaders(hRequest, level, WINHTTP_HEADER_NAME_BY_INDEX,
                             wbuf, &len, WINHTTP_NO_HEADER_INDEX)) {
        arena_free(wbuf);
        return NULL;
    }
    char *u = wide_to_utf8(wbuf, -1);
    arena_free(wbuf);
    return u;
}

/*
 * Core request driver. Performs one HTTP request and routes the body to a
 * MemoryBuffer and/or a FILE, optionally reporting progress. Any combination
 * of out_* pointers may be NULL.
 */
static HttpResult do_request(
    CurlSession *session,
    const wchar_t *method,
    const char *url,
    const char *if_none_match,
    MemoryBuffer *body_out,
    FILE *file_out,
    HttpProgressFn progress_fn,
    void *progress_userdata,
    long *out_response_code,
    char **out_etag,
    bool *out_not_modified,
    char *err_buf,
    size_t err_buf_size)
{
    HINTERNET hConnect = NULL;
    HINTERNET hRequest = NULL;
    HttpResult result = HTTP_ERROR_NETWORK;
    wchar_t *wurl = NULL;
    wchar_t *host = NULL;
    wchar_t *object = NULL;
    char *chunk = NULL;
    URL_COMPONENTS uc;
    DWORD objLen = 0;
    bool secure = false;
    DWORD status = 0;
    DWORD status_sz = sizeof(status);
    long response_code = 0;
    double total_bytes = 0;
    double downloaded = 0;
    DWORD req_flags = 0;

    if (out_response_code) *out_response_code = 0;
    if (out_etag) *out_etag = NULL;
    if (out_not_modified) *out_not_modified = false;
    if (!session || !session->hSession || !url) return HTTP_ERROR_INIT;

    session->error_buffer[0] = '\0';

    wurl = utf8_to_wide(url);
    if (!wurl) return HTTP_ERROR_MEMORY;

    memset(&uc, 0, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    uc.dwSchemeLength = (DWORD)-1;
    uc.dwHostNameLength = (DWORD)-1;
    uc.dwUrlPathLength = (DWORD)-1;
    uc.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) {
        set_error(session, err_buf, err_buf_size, GetLastError(), "WinHttpCrackUrl");
        arena_free(wurl);
        return HTTP_ERROR_INIT;
    }

    host = arena_malloc(((size_t)uc.dwHostNameLength + 1) * sizeof(wchar_t));
    if (!host) { arena_free(wurl); return HTTP_ERROR_MEMORY; }
    wmemcpy(host, uc.lpszHostName, uc.dwHostNameLength);
    host[uc.dwHostNameLength] = 0;

    objLen = uc.dwUrlPathLength + uc.dwExtraInfoLength;
    object = arena_malloc(((size_t)objLen + 1) * sizeof(wchar_t));
    if (!object) { arena_free(host); arena_free(wurl); return HTTP_ERROR_MEMORY; }
    if (objLen > 0) wmemcpy(object, uc.lpszUrlPath, objLen);
    object[objLen] = 0;

    secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);

    hConnect = WinHttpConnect(session->hSession, host, uc.nPort, 0);
    if (!hConnect) {
        set_error(session, err_buf, err_buf_size, GetLastError(), "WinHttpConnect");
        result = HTTP_ERROR_NETWORK;
        goto cleanup;
    }

    req_flags = secure ? WINHTTP_FLAG_SECURE : 0;
    hRequest = WinHttpOpenRequest(hConnect, method, object, NULL,
                                  WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                  req_flags);
    if (!hRequest) {
        set_error(session, err_buf, err_buf_size, GetLastError(), "WinHttpOpenRequest");
        result = HTTP_ERROR_NETWORK;
        goto cleanup;
    }

    if (session->timeout_ms > 0) {
        WinHttpSetTimeouts(hRequest, (int)session->timeout_ms, (int)session->timeout_ms,
                           (int)session->timeout_ms, (int)session->timeout_ms);
    }

    if (if_none_match && if_none_match[0] != '\0') {
        size_t hlen = strlen("If-None-Match: ") + strlen(if_none_match) + 1;
        char *hline = arena_malloc(hlen);
        if (hline) {
            snprintf(hline, hlen, "If-None-Match: %s", if_none_match);
            wchar_t *whline = utf8_to_wide(hline);
            if (whline) {
                WinHttpAddRequestHeaders(hRequest, whline, (DWORD)-1,
                                         WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
                arena_free(whline);
            }
            arena_free(hline);
        }
    }

    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        DWORD e = GetLastError();
        set_error(session, err_buf, err_buf_size, e, "WinHttpSendRequest");
        result = (e == ERROR_WINHTTP_TIMEOUT) ? HTTP_ERROR_TIMEOUT : HTTP_ERROR_NETWORK;
        goto cleanup;
    }

    if (!WinHttpReceiveResponse(hRequest, NULL)) {
        DWORD e = GetLastError();
        set_error(session, err_buf, err_buf_size, e, "WinHttpReceiveResponse");
        result = (e == ERROR_WINHTTP_TIMEOUT) ? HTTP_ERROR_TIMEOUT : HTTP_ERROR_NETWORK;
        goto cleanup;
    }

    if (!WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_sz,
                             WINHTTP_NO_HEADER_INDEX)) {
        set_error(session, err_buf, err_buf_size, GetLastError(), "WinHttpQueryHeaders");
        result = HTTP_ERROR_NETWORK;
        goto cleanup;
    }
    response_code = (long)status;
    if (out_response_code) *out_response_code = response_code;

    if (out_etag) {
        *out_etag = query_header_str(hRequest, WINHTTP_QUERY_ETAG);
    }

    if (response_code == 304) {
        if (out_not_modified) *out_not_modified = true;
        result = HTTP_OK;
        goto cleanup;
    }

    if (progress_fn) {
        DWORD clen = 0;
        DWORD clen_sz = sizeof(clen);
        if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &clen, &clen_sz,
                                WINHTTP_NO_HEADER_INDEX)) {
            total_bytes = (double)clen;
        }
    }

    if (body_out || file_out || progress_fn) {
        bool read_err = false;
        chunk = arena_malloc(WINHTTP_READ_CHUNK);
        if (!chunk) { result = HTTP_ERROR_MEMORY; goto cleanup; }

        for (;;) {
            DWORD avail = 0;
            DWORD to_read;
            DWORD got = 0;

            if (!WinHttpQueryDataAvailable(hRequest, &avail)) {
                set_error(session, err_buf, err_buf_size, GetLastError(),
                          "WinHttpQueryDataAvailable");
                read_err = true;
                break;
            }
            if (avail == 0) break;

            to_read = (avail < WINHTTP_READ_CHUNK) ? avail : WINHTTP_READ_CHUNK;
            if (!WinHttpReadData(hRequest, chunk, to_read, &got)) {
                set_error(session, err_buf, err_buf_size, GetLastError(), "WinHttpReadData");
                read_err = true;
                break;
            }
            if (got == 0) break;

            if (file_out) {
                if (fwrite(chunk, 1, got, file_out) != got) {
                    read_err = true;
                    break;
                }
            }
            if (body_out) {
                if (body_out->len + got + 1 > body_out->capacity) {
                    size_t newcap = (body_out->capacity == 0)
                                        ? MEMBUF_INITIAL_CAPACITY
                                        : body_out->capacity * 2;
                    while (newcap < body_out->len + got + 1) newcap *= 2;
                    char *nd = arena_realloc(body_out->data, newcap);
                    if (!nd) { read_err = true; break; }
                    body_out->data = nd;
                    body_out->capacity = newcap;
                }
                memcpy(body_out->data + body_out->len, chunk, got);
                body_out->len += got;
                body_out->data[body_out->len] = '\0';
            }

            downloaded += got;
            if (progress_fn && progress_fn(progress_userdata, total_bytes, downloaded) != 0) {
                set_error(session, err_buf, err_buf_size,
                          ERROR_WINHTTP_OPERATION_CANCELLED, "transfer aborted");
                read_err = true;
                break;
            }
        }

        if (read_err) { result = HTTP_ERROR_NETWORK; goto cleanup; }
    }

    if (http_is_success(response_code)) {
        result = HTTP_OK;
    } else if (http_is_client_error(response_code)) {
        result = HTTP_ERROR_4XX;
    } else if (http_is_server_error(response_code)) {
        result = HTTP_ERROR_5XX;
    } else {
        result = HTTP_ERROR_NETWORK;
    }

cleanup:
    if (chunk) arena_free(chunk);
    if (hRequest) WinHttpCloseHandle(hRequest);
    if (hConnect) WinHttpCloseHandle(hConnect);
    if (object) arena_free(object);
    if (host) arena_free(host);
    if (wurl) arena_free(wurl);
    return result;
}

/* ---- public API --------------------------------------------------------- */

CurlSession *curl_session_create(void) {
    CurlSession *session = arena_calloc(1, sizeof(CurlSession));
    if (!session) return NULL;

    session->timeout_ms = DEFAULT_TIMEOUT_MS;
    session->error_buffer[0] = '\0';
    session->hSession = WinHttpOpen(L"Elm/0.19.1 (libcurl)",
                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session->hSession) {
        arena_free(session);
        return NULL;
    }
    return session;
}

void curl_session_free(CurlSession *session) {
    if (!session) return;
    if (session->hSession) {
        WinHttpCloseHandle(session->hSession);
    }
    arena_free(session);
}

void curl_session_set_timeout(CurlSession *session, long timeout_ms) {
    if (!session) return;
    session->timeout_ms = timeout_ms;
}

bool curl_session_can_connect(CurlSession *session, const char *test_url) {
    if (!session || !test_url) return false;

    long saved = session->timeout_ms;
    session->timeout_ms = CONNECT_TEST_TIMEOUT_MS;
    HttpResult res = do_request(session, L"HEAD", test_url, NULL, NULL, NULL,
                                NULL, NULL, NULL, NULL, NULL, NULL, 0);
    session->timeout_ms = saved;

    /* Any HTTP response (even 4xx/5xx) means we reached the server. */
    return (res == HTTP_OK || res == HTTP_ERROR_4XX || res == HTTP_ERROR_5XX);
}

HttpResult http_download_file_ex(
    CurlSession *session,
    const char *url,
    const char *dest_path,
    HttpProgressFn progress_fn,
    void *progress_userdata,
    long *out_response_code,
    char *err_buf,
    size_t err_buf_size)
{
    if (out_response_code) *out_response_code = 0;
    if (err_buf && err_buf_size > 0) err_buf[0] = '\0';

    if (!session || !url || !dest_path) return HTTP_ERROR_INIT;

    FILE *file = fopen(dest_path, "wb");
    if (!file) {
        if (err_buf && err_buf_size > 0) {
            snprintf(err_buf, err_buf_size, "fopen('%s')", dest_path);
        }
        return HTTP_ERROR_INIT;
    }

    HttpResult res = do_request(session, L"GET", url, NULL, NULL, file,
                                progress_fn, progress_userdata,
                                out_response_code, NULL, NULL,
                                err_buf, err_buf_size);
    fclose(file);

    if (res != HTTP_OK) {
        remove(dest_path);
    }
    return res;
}

HttpResult http_download_file(CurlSession *session, const char *url, const char *dest_path) {
    return http_download_file_ex(session, url, dest_path, NULL, NULL, NULL, NULL, 0);
}

HttpResult http_head(CurlSession *session, const char *url) {
    return do_request(session, L"HEAD", url, NULL, NULL, NULL,
                      NULL, NULL, NULL, NULL, NULL, NULL, 0);
}

HttpResult http_head_etag(
    CurlSession *session,
    const char *url,
    const char *if_none_match,
    char **out_etag,
    bool *out_not_modified)
{
    return do_request(session, L"HEAD", url, if_none_match, NULL, NULL,
                      NULL, NULL, NULL, out_etag, out_not_modified, NULL, 0);
}

HttpResult http_get_json(CurlSession *session, const char *url, MemoryBuffer *out) {
    if (!session || !url || !out) return HTTP_ERROR_INIT;
    memory_buffer_clear(out);
    return do_request(session, L"GET", url, NULL, out, NULL,
                      NULL, NULL, NULL, NULL, NULL, NULL, 0);
}

HttpResult http_get_json_etag(
    CurlSession *session,
    const char *url,
    const char *if_none_match,
    MemoryBuffer *out,
    char **out_etag,
    bool *out_not_modified)
{
    if (!session || !url || !out) return HTTP_ERROR_INIT;
    memory_buffer_clear(out);
    return do_request(session, L"GET", url, if_none_match, out, NULL,
                      NULL, NULL, NULL, out_etag, out_not_modified, NULL, 0);
}

/* ---- memory buffer + error helpers (identical to the curl backend) ------ */

MemoryBuffer *memory_buffer_create(void) {
    return arena_calloc(1, sizeof(MemoryBuffer));
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

const char *http_result_to_string(HttpResult result) {
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

const char *curl_session_get_error(CurlSession *session) {
    if (!session) return "No session";
    if (session->error_buffer[0] != '\0') {
        return session->error_buffer;
    }
    return "No error details available";
}

#endif /* _WIN32 */
