#ifndef OS_COMPAT_H
#define OS_COMPAT_H

/*
 * os_compat.h - Windows (llvm-mingw / UCRT) portability shims.
 *
 * For the Windows build this header is force-included into every translation
 * unit via `-include src/os_compat.h` (see the `windows` target in the
 * Makefile). It supplies POSIX functions that mingw does not provide
 * (realpath, symlink, readlink) and maps a few POSIX names onto their Windows
 * CRT equivalents so the bulk of the codebase compiles unchanged. The
 * heavier process-spawning helpers are declared here and used behind
 * `#ifdef _WIN32` at the (few) call sites that fork/exec.
 *
 * On macOS and Linux this header is completely inert.
 */

#ifdef _WIN32

#include <stddef.h>
#include <sys/types.h>   /* ssize_t */
#include <direct.h>      /* _mkdir */
#include <io.h>          /* 1-arg mkdir, dup, dup2, _open, ... */
#include <sys/stat.h>    /* struct stat, S_ISDIR/S_ISREG */

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/*
 * POSIX mkdir takes a mode argument; mingw's mkdir takes only the path. Route
 * mkdir(path, mode) to _mkdir(path), dropping the (meaningless on Windows)
 * mode. <io.h>/<sys/stat.h> are included above so their 1-arg mkdir prototype
 * is processed before this macro takes effect.
 */
#ifndef mkdir
#define mkdir(path, mode) _mkdir(path)
#endif

/* mingw ships _popen/_pclose; expose them under the POSIX names. */
#ifndef popen
#define popen _popen
#endif
#ifndef pclose
#define pclose _pclose
#endif

/* fsync(2): flush file data to disk -> Windows CRT _commit(fd). */
#ifndef fsync
#define fsync _commit
#endif

/*
 * No symlink-aware stat on Windows. Plain stat() is sufficient for the
 * directory / regular-file checks wrap performs; the rare "is this path a
 * symlink?" question is answered explicitly via os_win_path_is_symlink().
 */
#ifndef lstat
#define lstat stat
#endif
#ifndef S_ISLNK
#define S_ISLNK(m) (0)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* POSIX functions absent from mingw, implemented in os_compat.c. */
char *realpath(const char *path, char *resolved_path);
int symlink(const char *target, const char *linkpath);
ssize_t readlink(const char *path, char *buf, size_t bufsize);

/*
 * mkstemps(3): like mkstemp but the template may carry a fixed suffix of
 * `suffixlen` chars after the six 'X'. mingw only ships mkstemp, so we provide
 * this. Creates the file O_EXCL and returns an open read/write fd, or -1.
 */
int mkstemps(char *template_name, int suffixlen);

/*
 * Process helpers (CreateProcess based), implemented in os_compat.c.
 *
 *   os_win_exec_replace: spawn `exe` with argv/envp, wait for it, and return
 *                        its exit code (>= 0), or -1 if it could not be
 *                        launched. This mirrors the "become the child"
 *                        semantics that execve() provides for a CLI wrapper.
 *
 *   os_win_run_capture:  spawn `exe` in directory `cwd`, capture its merged
 *                        stdout+stderr (up to `cap` bytes) into an arena
 *                        string returned via out_stdout, and return the exit
 *                        code (>= 0), or -1 on launch failure.
 *
 *   os_win_path_is_symlink: 1 if `path` is a reparse point (symlink/junction).
 *
 * argv and envp are NULL-terminated arrays. envp entries are "KEY=VALUE"
 * strings; pass NULL to inherit the parent environment.
 */
int os_win_exec_replace(const char *exe, char *const argv[], char *const envp[]);
int os_win_run_capture(const char *exe, char *const argv[], char *const envp[],
                       const char *cwd, char **out_stdout, size_t cap);
int os_win_path_is_symlink(const char *path);

/* Absolute path of the running executable (GetModuleFileName), UTF-8.
 * Returns 1 on success, 0 on failure. Used to locate the appended rules zip. */
int os_win_executable_path(char *buf, size_t bufsize);

/* rename(src,dst) replacing an existing dst (MoveFileEx). 0 ok, -1 fail. */
int os_win_rename_replace(const char *src, const char *dst);

/* Enable ANSI/VT escape processing on the console so colors render. */
void os_win_enable_vt(void);

#ifdef __cplusplus
}
#endif

#endif /* _WIN32 */
#endif /* OS_COMPAT_H */
