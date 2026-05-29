/*
 * os_compat.c - Windows implementations of the shims declared in os_compat.h.
 *
 * Compiled and linked only for the Windows target (added to OBJECTS inside the
 * TARGET_OS=windows block in the Makefile). The whole file is guarded by
 * _WIN32 so it is an empty translation unit anywhere else.
 */

#ifdef _WIN32

#include "os_compat.h"
#include "alloc.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>   /* our win_compat shim; declares basename/dirname */

/* ---- basename / dirname (POSIX libgen, absent on mingw) ----------------- */

static int is_sep(char c) { return c == '/' || c == '\\'; }

char *basename(char *path) {
    static char dot[] = ".";
    if (!path || !*path) return dot;

    size_t len = strlen(path);
    while (len > 1 && is_sep(path[len - 1])) {
        path[--len] = '\0';
    }
    char *p = path + len;
    while (p > path && !is_sep(p[-1])) {
        p--;
    }
    return p;
}

char *dirname(char *path) {
    static char dot[] = ".";
    if (!path || !*path) return dot;

    size_t len = strlen(path);
    while (len > 1 && is_sep(path[len - 1])) {
        path[--len] = '\0';
    }
    char *p = path + len;
    while (p > path && !is_sep(p[-1])) {
        p--;
    }
    if (p == path) {
        return dot;
    }
    while (p > path + 1 && is_sep(p[-1])) {
        p--;
    }
    *p = '\0';
    return path;
}

/* ---- realpath / symlink / readlink ------------------------------------- */

char *realpath(const char *path, char *resolved_path) {
    if (!path || !resolved_path) return NULL;

    char tmp[PATH_MAX];
    if (!_fullpath(tmp, path, sizeof(tmp))) {
        return NULL;
    }
    /* POSIX realpath fails if the target does not exist; match that so the
     * many "if (realpath(...))" existence probes keep working. */
    if (GetFileAttributesA(tmp) == INVALID_FILE_ATTRIBUTES) {
        return NULL;
    }
    strncpy(resolved_path, tmp, PATH_MAX - 1);
    resolved_path[PATH_MAX - 1] = '\0';
    return resolved_path;
}

int symlink(const char *target, const char *linkpath) {
    if (!target || !linkpath) return -1;

    DWORD flags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
    DWORD attr = GetFileAttributesA(target);
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        flags |= SYMBOLIC_LINK_FLAG_DIRECTORY;
    }
    if (CreateSymbolicLinkA(linkpath, target, flags)) {
        return 0;
    }
    return -1;
}

ssize_t readlink(const char *path, char *buf, size_t bufsize) {
    if (!path || !buf || bufsize == 0) return -1;

    HANDLE h = CreateFileA(path, 0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return -1;
    }

    char tmp[PATH_MAX];
    DWORD n = GetFinalPathNameByHandleA(h, tmp, sizeof(tmp),
                                        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(h);

    if (n == 0 || n >= sizeof(tmp)) {
        return -1;
    }

    /* Strip the \\?\ prefix that GetFinalPathNameByHandle prepends. */
    char *p = tmp;
    if (strncmp(p, "\\\\?\\", 4) == 0) {
        p += 4;
    }

    size_t len = strlen(p);
    if (len > bufsize) {
        len = bufsize;
    }
    memcpy(buf, p, len);          /* readlink() does not NUL-terminate */
    return (ssize_t)len;
}

int os_win_path_is_symlink(const char *path) {
    if (!path) return 0;
    DWORD attr = GetFileAttributesA(path);
    if (attr == INVALID_FILE_ATTRIBUTES) return 0;
    return (attr & FILE_ATTRIBUTE_REPARSE_POINT) ? 1 : 0;
}

int mkstemps(char *template_name, int suffixlen) {
    if (!template_name || suffixlen < 0) {
        errno = EINVAL;
        return -1;
    }
    size_t len = strlen(template_name);
    if ((size_t)suffixlen + 6 > len) {
        errno = EINVAL;
        return -1;
    }
    char *xpos = template_name + (len - 6 - (size_t)suffixlen);
    for (int i = 0; i < 6; i++) {
        if (xpos[i] != 'X') {
            errno = EINVAL;
            return -1;
        }
    }

    static const char charset[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    const unsigned cs = (unsigned)(sizeof(charset) - 1);

    /* LCG seeded from time + pid; O_EXCL makes collisions safe regardless. */
    unsigned long long seed = (unsigned long long)GetTickCount64()
                            ^ ((unsigned long long)GetCurrentProcessId() << 16);

    for (int attempt = 0; attempt < 512; attempt++) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        unsigned long long r = seed >> 16;
        for (int i = 0; i < 6; i++) {
            xpos[i] = charset[r % cs];
            r /= cs;
        }
        int fd = _open(template_name, _O_CREAT | _O_EXCL | _O_RDWR | _O_BINARY,
                       _S_IREAD | _S_IWRITE);
        if (fd >= 0) {
            return fd;
        }
        if (errno != EEXIST) {
            return -1;
        }
    }
    errno = EEXIST;
    return -1;
}

int os_win_executable_path(char *buf, size_t bufsize) {
    if (!buf || bufsize == 0) return 0;
    wchar_t wpath[PATH_MAX];
    DWORD n = GetModuleFileNameW(NULL, wpath,
                                 (DWORD)(sizeof(wpath) / sizeof(wpath[0])));
    if (n == 0 || n >= (DWORD)(sizeof(wpath) / sizeof(wpath[0]))) {
        return 0;
    }
    int u = WideCharToMultiByte(CP_UTF8, 0, wpath, -1, buf, (int)bufsize, NULL, NULL);
    return (u > 0) ? 1 : 0;
}

int os_win_rename_replace(const char *src, const char *dst) {
    if (!src || !dst) return -1;
    if (MoveFileExA(src, dst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
        return 0;
    }
    return -1;
}

void os_win_enable_vt(void) {
    DWORD mode = 0;
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out != INVALID_HANDLE_VALUE && GetConsoleMode(out, &mode)) {
        SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if (err != INVALID_HANDLE_VALUE && GetConsoleMode(err, &mode)) {
        SetConsoleMode(err, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
}

/* ---- process spawning -------------------------------------------------- */

/*
 * Build a single command-line string from argv. Each argument is wrapped in
 * double quotes with embedded quotes/backslashes escaped. Adequate for the
 * compiler paths and option strings wrap passes around.
 */
static char *build_cmdline(char *const argv[]) {
    size_t cap = 256;
    size_t len = 0;
    char *cmd = arena_malloc(cap);
    if (!cmd) return NULL;
    cmd[0] = '\0';

    for (int i = 0; argv[i] != NULL; i++) {
        const char *a = argv[i];
        size_t need = len + (strlen(a) * 2) + 4; /* quotes, escapes, space, NUL */
        if (need >= cap) {
            while (cap <= need) cap *= 2;
            char *grown = arena_realloc(cmd, cap);
            if (!grown) return NULL;
            cmd = grown;
        }
        if (i > 0) cmd[len++] = ' ';
        cmd[len++] = '"';
        for (const char *p = a; *p; p++) {
            if (*p == '"' || *p == '\\') {
                cmd[len++] = '\\';
            }
            cmd[len++] = *p;
        }
        cmd[len++] = '"';
        cmd[len] = '\0';
    }
    return cmd;
}

/*
 * Build a Windows environment block (a sequence of "KEY=VALUE\0" entries
 * terminated by an extra \0) from a NULL-terminated envp array. Returns NULL
 * if envp is NULL, meaning "inherit the parent environment".
 */
static char *build_env_block(char *const envp[]) {
    if (!envp) return NULL;

    size_t total = 1; /* trailing terminator */
    for (int i = 0; envp[i] != NULL; i++) {
        total += strlen(envp[i]) + 1;
    }

    char *block = arena_malloc(total + 1);
    if (!block) return NULL;

    size_t off = 0;
    for (int i = 0; envp[i] != NULL; i++) {
        size_t l = strlen(envp[i]);
        memcpy(block + off, envp[i], l);
        off += l;
        block[off++] = '\0';
    }
    block[off] = '\0';
    return block;
}

int os_win_exec_replace(const char *exe, char *const argv[], char *const envp[]) {
    if (!exe || !argv) return -1;

    char *cmdline = build_cmdline(argv);
    if (!cmdline) return -1;
    char *env_block = build_env_block(envp);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);

    BOOL ok = CreateProcessA(exe, cmdline, NULL, NULL, TRUE, 0,
                             env_block, NULL, &si, &pi);
    if (!ok) {
        return -1;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

int os_win_run_capture(const char *exe, char *const argv[], char *const envp[],
                       const char *cwd, char **out_stdout, size_t cap) {
    if (out_stdout) *out_stdout = NULL;
    if (!exe || !argv) return -1;

    SECURITY_ATTRIBUTES sa;
    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        return -1;
    }
    /* The read end must not be inherited by the child. */
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    char *cmdline = build_cmdline(argv);
    if (!cmdline) {
        CloseHandle(rd);
        CloseHandle(wr);
        return -1;
    }
    char *env_block = build_env_block(envp);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    BOOL ok = CreateProcessA(exe, cmdline, NULL, NULL, TRUE, 0,
                             env_block, cwd, &si, &pi);
    if (!ok) {
        CloseHandle(rd);
        CloseHandle(wr);
        return -1;
    }
    /* Parent closes its copy of the write end so the read sees EOF. */
    CloseHandle(wr);

    char *buf = arena_malloc(cap + 1);
    size_t total = 0;
    if (buf) {
        for (;;) {
            if (total >= cap) break;
            DWORD want = (DWORD)(cap - total);
            DWORD got = 0;
            if (!ReadFile(rd, buf + total, want, &got, NULL) || got == 0) {
                break;
            }
            total += got;
        }
        buf[total] = '\0';
    }
    CloseHandle(rd);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (out_stdout) *out_stdout = buf;
    return (int)code;
}

#else

/* Non-Windows: keep this translation unit non-empty for strict compilers. */
typedef int os_compat_tu_not_empty;

#endif /* _WIN32 */
