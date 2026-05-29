#ifndef WIN_COMPAT_LIBGEN_H
#define WIN_COMPAT_LIBGEN_H

/*
 * Minimal <libgen.h> for the Windows build. mingw-w64 does not ship libgen.h,
 * so this shim (reachable via -Isrc/win_compat) declares basename()/dirname();
 * the implementations live in os_compat.c and understand both '/' and '\'.
 */

char *basename(char *path);
char *dirname(char *path);

#endif /* WIN_COMPAT_LIBGEN_H */
