/**
 * install_cmd.c - `wrap kit install PATH` and `wrap kit update --dry-run SRC`
 *
 * Parses a kitfile manifest and installs the packages and tools it lists.
 * `kit update --dry-run` reuses the same parser to non-destructively verify
 * that a published kit (from a URL or local path) is consumable.
 * See doc/tools/Kit.md for the format and semantics.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "kit.h"
#include "../package/install_local_dev.h"
#include "../package/package_common.h"
#include "../repository/repository.h"
#include "../../alloc.h"
#include "../../constants.h"
#include "../../dyn_array.h"
#include "../../env_defaults.h"
#include "../../fileutil.h"
#include "../../global_context.h"
#include "../../http_client.h"
#include "../../install_env.h"
#include "../../shared/log.h"
#include "../../vendor/sha256.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX MAX_PATH_LENGTH
#endif

/* ===========================================================================
 * Manifest data structures
 * ========================================================================= */

typedef struct {
    char *name;            /* tool name (basename) */
    char *url;             /* download URL (NULL if discovered from tools/ folder) */
    char *local_bin_path;  /* absolute path to pre-built binary (NULL for URL tools) */
} KitTool;

typedef struct {
    char *author;
    char *name;
    char *version;         /* may be NULL if not specified */
} KitPackage;

typedef struct {
    char *author;
    char *name;
    char *version;
} KitLocalDev;

typedef struct {
    /* Manifest header */
    char *compiler_name;
    char *compiler_version;

    /* kit: entry */
    char *kit_name;
    char *kit_version;
    bool has_kit;

    /* Resolved directories */
    char *kitfile_path;    /* absolute path to the kitfile */
    char *kitfile_dir;     /* parent directory of the kitfile */

    KitTool *tools;
    int tool_count;
    int tool_capacity;

    KitPackage *packages;
    int package_count;
    int package_capacity;

    KitLocalDev *local_devs;
    int local_dev_count;
    int local_dev_capacity;
} KitManifest;

/* ===========================================================================
 * Usage
 * ========================================================================= */

static void print_install_usage(void) {
    printf("Usage: %s kit install PATH [OPTIONS]\n", global_context_program_name());
    printf("\n");
    printf("Install a kit (a manifest of tools and packages) from PATH.\n");
    printf("\n");
    printf("Arguments:\n");
    printf("  PATH                  A .kit file or a directory containing a single .kit file\n");
    printf("\n");
    printf("Options:\n");
    printf("  -y, --yes             Automatically confirm prompts\n");
    printf("      --dry-run         Verify only: parse and report, write nothing\n");
    printf("      --check-urls      With --dry-run, probe each tool URL (HTTP HEAD)\n");
    printf("  -h, --help            Show this help message\n");
    printf("\n");
    printf("Tool binaries are symlinked into ~/.local/bin (override with WRAP_TOOL_BIN_PATH).\n");
}

/* ===========================================================================
 * Small helpers
 * ========================================================================= */

static bool is_help_flag(const char *arg) {
    return arg && (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0);
}

static bool path_is_directory(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return false;
    return S_ISDIR(st.st_mode);
}

static bool path_is_regular_file(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return false;
    return S_ISREG(st.st_mode);
}

static bool path_is_nonempty_regular_file(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return false;
    return S_ISREG(st.st_mode) && st.st_size > 0;
}

static bool path_ends_with(const char *path, const char *suffix) {
    size_t pl = strlen(path);
    size_t sl = strlen(suffix);
    if (sl > pl) return false;
    return strcmp(path + pl - sl, suffix) == 0;
}

static char *join_path(const char *a, const char *b) {
    size_t len = strlen(a) + 1 + strlen(b) + 1;
    char *out = arena_malloc(len);
    if (!out) return NULL;
    snprintf(out, len, "%s/%s", a, b);
    return out;
}

static char *parent_directory_of(const char *path) {
    char buf[PATH_MAX];
    size_t pl = strlen(path);
    if (pl >= sizeof(buf)) {
        return NULL;
    }
    memcpy(buf, path, pl + 1);
    char *dn = dirname(buf);
    if (!dn) return NULL;
    return arena_strdup(dn);
}

static char *absolute_path_of(const char *path) {
    char resolved[PATH_MAX];
    if (!realpath(path, resolved)) {
        return NULL;
    }
    return arena_strdup(resolved);
}

static char *rstrip_in_place(char *s) {
    if (!s) return s;
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' ||
                        s[len - 1] == '\r' || s[len - 1] == '\n')) {
        s[--len] = '\0';
    }
    return s;
}

static const char *skip_leading_ws(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

static bool url_is_http(const char *url) {
    if (!url) return false;
    return strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0;
}

/* Return the length of `s` excluding any trailing '/' characters (always >= 1
 * if the string is non-empty so we don't reject "/"). */
static size_t len_no_trailing_slash(const char *s) {
    size_t len = strlen(s);
    while (len > 1 && s[len - 1] == '/') len--;
    return len;
}

/* True if `dir` appears (verbatim, ignoring trailing slashes) as a component
 * of the PATH environment variable. */
static bool path_env_contains_dir(const char *dir) {
    if (!dir || dir[0] == '\0') return false;
    const char *path_env = getenv("PATH");
    if (!path_env || path_env[0] == '\0') return false;

    size_t dir_len = len_no_trailing_slash(dir);

    const char *p = path_env;
    while (*p) {
        const char *colon = strchr(p, ':');
        size_t seg_len = colon ? (size_t)(colon - p) : strlen(p);
        if (seg_len > 0) {
            /* Strip trailing slashes for comparison. */
            while (seg_len > 1 && p[seg_len - 1] == '/') seg_len--;
            if (seg_len == dir_len && memcmp(p, dir, dir_len) == 0) {
                return true;
            }
        }
        if (!colon) break;
        p = colon + 1;
    }
    return false;
}

/* Detect the user's interactive shell from $SHELL. Returns "bash", "zsh",
 * "fish", or NULL when not recognized. */
static const char *detect_shell_basename(void) {
    const char *shell = getenv("SHELL");
    if (!shell || shell[0] == '\0') return NULL;
    const char *slash = strrchr(shell, '/');
    const char *base = slash ? slash + 1 : shell;
    if (strcmp(base, "bash") == 0) return "bash";
    if (strcmp(base, "zsh") == 0) return "zsh";
    if (strcmp(base, "fish") == 0) return "fish";
    return NULL;
}

/* Print a notice describing how to add `dir` to the user's $PATH. */
static void print_path_warning(const char *dir) {
    const char *shell = detect_shell_basename();
    fprintf(stderr,
            "\nNote: '%s' is not in your $PATH, so the installed tools won't "
            "be found by name.\n", dir);
    fprintf(stderr, "Add it to your shell profile:\n\n");

    if (shell == NULL || strcmp(shell, "bash") == 0) {
        fprintf(stderr, "  bash:  echo 'export PATH=\"%s:$PATH\"' >> ~/.bashrc\n",
                dir);
    }
    if (shell == NULL || strcmp(shell, "zsh") == 0) {
        fprintf(stderr, "  zsh:   echo 'export PATH=\"%s:$PATH\"' >> ~/.zshrc\n",
                dir);
    }
    if (shell == NULL || strcmp(shell, "fish") == 0) {
        fprintf(stderr, "  fish:  fish_add_path %s\n", dir);
    }
    fprintf(stderr, "\nThen start a new shell (or `source` the file you edited).\n");
}

/* Confirm with the user. Returns true if the user accepts, false otherwise. */
static bool confirm_prompt(const char *question, bool auto_yes) {
    if (auto_yes) return true;
    printf("%s [Y/n] ", question);
    fflush(stdout);
    char response[MAX_RESPONSE_LENGTH];
    if (!fgets(response, sizeof(response), stdin)) {
        return false;
    }
    return response[0] == 'Y' || response[0] == 'y' || response[0] == '\n';
}

/* ===========================================================================
 * Kitfile discovery
 * ========================================================================= */

/* Find a single .kit file in the directory.
 * Returns absolute path on success, NULL on failure (zero or many matches).
 * On error, logs a descriptive message. */
static char *find_kit_in_directory(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) {
        log_error("Cannot open directory '%s': %s", dir, strerror(errno));
        return NULL;
    }
    char *found = NULL;
    int matches = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (!path_ends_with(ent->d_name, ".kit")) continue;
        char *candidate = join_path(dir, ent->d_name);
        if (!candidate) {
            closedir(d);
            log_error("Out of memory");
            arena_free(found);
            return NULL;
        }
        if (!path_is_regular_file(candidate)) {
            arena_free(candidate);
            continue;
        }
        matches++;
        if (matches > 1) {
            arena_free(candidate);
            arena_free(found);
            closedir(d);
            log_error("A kit must contain a single kitfile.");
            return NULL;
        }
        found = candidate;
    }
    closedir(d);

    if (matches == 0) {
        log_error("No .kit manifest found in directory '%s'", dir);
        return NULL;
    }
    return found;
}

/* ===========================================================================
 * Kitfile parser
 * =========================================================================
 *
 * Grammar (per doc/tools/Kit.md):
 *
 *     format 2
 *     COMPILER VERSION
 *
 *     kit: NAME
 *         version: VERSION
 *
 *     tool: NAME
 *         url: URL
 *
 *     package: AUTHOR/NAME[@VERSION]
 *     local-dev: AUTHOR/NAME/VERSION
 *
 * Top-level entries have 0 indentation. Property lines under `kit:` and
 * `tool:` have 4-space indentation. The first non-header entry must be a
 * single `kit:` block. Any unknown top-level key is rejected.
 */

typedef enum {
    SECTION_NONE,
    SECTION_KIT,
    SECTION_TOOL,
    /* single-line sections (no nested properties expected) */
    SECTION_SINGLE_LINE
} SectionKind;

static bool starts_with(const char *s, const char *prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

/* Read a logical line into a newly arena-allocated buffer.
 * Returns NULL when end-of-input is reached. */
static char *next_line(const char **cursor, const char *end) {
    if (*cursor >= end) return NULL;
    const char *start = *cursor;
    const char *p = start;
    while (p < end && *p != '\n') p++;
    size_t len = (size_t)(p - start);
    char *line = arena_malloc(len + 1);
    if (!line) return NULL;
    memcpy(line, start, len);
    line[len] = '\0';
    *cursor = (p < end) ? p + 1 : end;
    return line;
}

/* Count leading space characters (tabs are not allowed in this format). */
static int leading_spaces(const char *line) {
    int n = 0;
    while (line[n] == ' ') n++;
    return n;
}

static bool line_has_tab_indent(const char *line) {
    return line[0] == '\t';
}

static bool line_is_blank(const char *line) {
    for (const char *p = line; *p; p++) {
        if (*p != ' ' && *p != '\t' && *p != '\r') return false;
    }
    return true;
}

/* Extract the value after `KEY:` from a top-level line. Returns arena-allocated
 * trimmed value, or NULL if the value is empty. */
static char *extract_top_value(const char *line, const char *key) {
    const char *rest = line + strlen(key);
    rest = skip_leading_ws(rest);
    if (*rest == '\0') return NULL;
    char *val = arena_strdup(rest);
    if (!val) return NULL;
    rstrip_in_place(val);
    if (val[0] == '\0') {
        arena_free(val);
        return NULL;
    }
    return val;
}

/* Parse "PACKAGE[@VERSION]" or "PACKAGE/VERSION".
 * package_required_version: if true, fails when no version is present.
 * out_version is set to NULL if version was not provided.
 * Returns true on success. */
static bool parse_pkg_with_version(const char *spec, bool version_required,
                                   char **out_author, char **out_name,
                                   char **out_version) {
    *out_author = NULL;
    *out_name = NULL;
    *out_version = NULL;

    /* Try `author/name@version` first */
    const char *at = strchr(spec, '@');
    if (at) {
        size_t pre_len = (size_t)(at - spec);
        char *pre = arena_malloc(pre_len + 1);
        if (!pre) return false;
        memcpy(pre, spec, pre_len);
        pre[pre_len] = '\0';

        if (!parse_package_name_silent(pre, out_author, out_name)) {
            arena_free(pre);
            return false;
        }
        arena_free(pre);

        const char *ver = at + 1;
        if (*ver == '\0') return false;
        *out_version = arena_strdup(ver);
        return *out_version != NULL;
    }

    /* Try `author/name/version`: count slashes */
    int slashes = 0;
    const char *last_slash = NULL;
    for (const char *p = spec; *p; p++) {
        if (*p == '/') {
            slashes++;
            last_slash = p;
        }
    }

    if (slashes == 2 && last_slash) {
        /* Split before the last slash for the package name. */
        size_t pre_len = (size_t)(last_slash - spec);
        char *pre = arena_malloc(pre_len + 1);
        if (!pre) return false;
        memcpy(pre, spec, pre_len);
        pre[pre_len] = '\0';

        if (!parse_package_name_silent(pre, out_author, out_name)) {
            arena_free(pre);
            return false;
        }
        arena_free(pre);

        const char *ver = last_slash + 1;
        if (*ver == '\0') return false;
        *out_version = arena_strdup(ver);
        return *out_version != NULL;
    }

    /* Plain `author/name` with no version */
    if (version_required) return false;
    return parse_package_name_silent(spec, out_author, out_name);
}

static bool manifest_init(KitManifest *m) {
    memset(m, 0, sizeof(*m));
    return true;
}

static bool manifest_push_tool(KitManifest *m, KitTool t) {
    DYNARRAY_PUSH(m->tools, m->tool_count, m->tool_capacity, t, KitTool);
    return m->tools != NULL;
}

static bool manifest_push_package(KitManifest *m, KitPackage p) {
    DYNARRAY_PUSH(m->packages, m->package_count, m->package_capacity, p, KitPackage);
    return m->packages != NULL;
}

static bool manifest_push_local_dev(KitManifest *m, KitLocalDev ld) {
    DYNARRAY_PUSH(m->local_devs, m->local_dev_count, m->local_dev_capacity, ld, KitLocalDev);
    return m->local_devs != NULL;
}

static bool manifest_has_tool_named(const KitManifest *m, const char *name) {
    for (int i = 0; i < m->tool_count; i++) {
        if (strcmp(m->tools[i].name, name) == 0) return true;
    }
    return false;
}

static bool manifest_has_package(const KitManifest *m, const char *author,
                                  const char *name, const char *version) {
    for (int i = 0; i < m->package_count; i++) {
        if (strcmp(m->packages[i].author, author) == 0 &&
            strcmp(m->packages[i].name, name) == 0) {
            if (!version || !m->packages[i].version) {
                if (!version && !m->packages[i].version) return true;
                continue;
            }
            if (strcmp(m->packages[i].version, version) == 0) return true;
        }
    }
    return false;
}

static bool valid_tool_name(const char *name) {
    if (!name || name[0] == '\0') return false;
    for (const char *p = name; *p; p++) {
        unsigned char c = (unsigned char)*p;
        /* Only allow safe filename characters; reject path separators. */
        if (c == '/' || c == '\\' || c == '\0') return false;
        if (!(isalnum(c) || c == '-' || c == '_' || c == '.')) return false;
    }
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return false;
    return true;
}

/* Parse the kitfile body. Returns true on success. */
static bool kitfile_parse(const char *body, size_t body_len, KitManifest *m,
                           const char *kitfile_path) {
    const char *cursor = body;
    const char *end = body + body_len;
    int line_no = 0;
    bool seen_format = false;
    bool seen_compiler_line = false;

    SectionKind active = SECTION_NONE;
    /* Indices into the manifest arrays for the currently-open block. */
    int active_idx = -1;

    while (cursor < end) {
        char *line = next_line(&cursor, end);
        if (!line) break;
        line_no++;

        /* Strip trailing whitespace once; preserve leading spaces for indent
         * detection. */
        rstrip_in_place(line);

        if (line_is_blank(line)) {
            /* Blank line ends any open block. */
            active = SECTION_NONE;
            active_idx = -1;
            arena_free(line);
            continue;
        }

        if (line_has_tab_indent(line)) {
            log_error("%s:%d: tab indentation is not allowed; use spaces",
                      kitfile_path, line_no);
            arena_free(line);
            return false;
        }

        int indent = leading_spaces(line);
        const char *content = line + indent;

        if (indent == 0) {
            /* Top-level line. */
            active = SECTION_NONE;
            active_idx = -1;

            if (!seen_format) {
                /* The very first non-blank line must be the format header. */
                if (strcmp(content, "format 2") != 0) {
                    log_error("%s:%d: expected 'format 2' as the first line",
                              kitfile_path, line_no);
                    arena_free(line);
                    return false;
                }
                seen_format = true;
                arena_free(line);
                continue;
            }

            if (!seen_compiler_line) {
                /* Second non-blank top-level line: "COMPILER VERSION". */
                const char *space = strchr(content, ' ');
                if (!space) {
                    log_error("%s:%d: expected compiler line 'NAME VERSION'",
                              kitfile_path, line_no);
                    arena_free(line);
                    return false;
                }
                size_t cn_len = (size_t)(space - content);
                if (cn_len == 0) {
                    log_error("%s:%d: empty compiler name", kitfile_path, line_no);
                    arena_free(line);
                    return false;
                }
                char *cn = arena_malloc(cn_len + 1);
                if (!cn) { arena_free(line); return false; }
                memcpy(cn, content, cn_len);
                cn[cn_len] = '\0';

                const char *ver = skip_leading_ws(space + 1);
                if (*ver == '\0') {
                    log_error("%s:%d: missing compiler version", kitfile_path, line_no);
                    arena_free(cn);
                    arena_free(line);
                    return false;
                }
                m->compiler_name = cn;
                m->compiler_version = arena_strdup(ver);
                if (!m->compiler_version) {
                    arena_free(line);
                    return false;
                }
                seen_compiler_line = true;
                arena_free(line);
                continue;
            }

            if (starts_with(content, "kit:")) {
                if (m->has_kit) {
                    log_error("%s:%d: multiple 'kit:' entries are not allowed",
                              kitfile_path, line_no);
                    arena_free(line);
                    return false;
                }
                char *name = extract_top_value(content, "kit:");
                if (!name) {
                    log_error("%s:%d: empty 'kit:' entry", kitfile_path, line_no);
                    arena_free(line);
                    return false;
                }
                m->kit_name = name;
                m->has_kit = true;
                active = SECTION_KIT;
                active_idx = -1; /* the kit block is stored on m directly */
                arena_free(line);
                continue;
            }

            /* From here on, the first entry must have been `kit:`. */
            if (!m->has_kit) {
                log_error("%s:%d: the first entry in a kitfile must be 'kit: NAME'",
                          kitfile_path, line_no);
                arena_free(line);
                return false;
            }

            if (starts_with(content, "tool:")) {
                char *name = extract_top_value(content, "tool:");
                if (!name) {
                    log_error("%s:%d: empty 'tool:' entry", kitfile_path, line_no);
                    arena_free(line);
                    return false;
                }
                if (!valid_tool_name(name)) {
                    log_error("%s:%d: invalid tool name '%s'", kitfile_path, line_no, name);
                    arena_free(line);
                    return false;
                }
                if (manifest_has_tool_named(m, name)) {
                    log_error("%s:%d: duplicate tool '%s'", kitfile_path, line_no, name);
                    arena_free(line);
                    return false;
                }
                KitTool t = { .name = name, .url = NULL, .local_bin_path = NULL };
                if (!manifest_push_tool(m, t)) {
                    arena_free(line);
                    return false;
                }
                active = SECTION_TOOL;
                active_idx = m->tool_count - 1;
                arena_free(line);
                continue;
            }

            if (starts_with(content, "package:")) {
                char *val = extract_top_value(content, "package:");
                if (!val) {
                    log_error("%s:%d: empty 'package:' entry", kitfile_path, line_no);
                    arena_free(line);
                    return false;
                }
                char *author = NULL;
                char *name = NULL;
                char *version = NULL;
                if (!parse_pkg_with_version(val, false, &author, &name, &version)) {
                    log_error("%s:%d: invalid package specification '%s'",
                              kitfile_path, line_no, val);
                    arena_free(val);
                    arena_free(line);
                    return false;
                }
                arena_free(val);
                KitPackage p = { .author = author, .name = name, .version = version };
                if (!manifest_push_package(m, p)) {
                    arena_free(line);
                    return false;
                }
                active = SECTION_SINGLE_LINE;
                active_idx = -1;
                arena_free(line);
                continue;
            }

            if (starts_with(content, "local-dev:")) {
                char *val = extract_top_value(content, "local-dev:");
                if (!val) {
                    log_error("%s:%d: empty 'local-dev:' entry", kitfile_path, line_no);
                    arena_free(line);
                    return false;
                }
                char *author = NULL;
                char *name = NULL;
                char *version = NULL;
                if (!parse_pkg_with_version(val, true, &author, &name, &version)) {
                    log_error("%s:%d: invalid local-dev specification '%s' (expected AUTHOR/NAME/VERSION)",
                              kitfile_path, line_no, val);
                    arena_free(val);
                    arena_free(line);
                    return false;
                }
                arena_free(val);
                KitLocalDev ld = { .author = author, .name = name, .version = version };
                if (!manifest_push_local_dev(m, ld)) {
                    arena_free(line);
                    return false;
                }
                active = SECTION_SINGLE_LINE;
                active_idx = -1;
                arena_free(line);
                continue;
            }

            log_error("%s:%d: unknown top-level key in kitfile",
                      kitfile_path, line_no);
            arena_free(line);
            return false;
        }

        /* Indented line: must belong to an open kit: or tool: block. */
        if (indent != 4) {
            log_error("%s:%d: expected 4-space indentation for property line",
                      kitfile_path, line_no);
            arena_free(line);
            return false;
        }

        if (active == SECTION_KIT) {
            if (!starts_with(content, "version:")) {
                log_error("%s:%d: 'kit:' block only allows 'version:' (got '%s')",
                          kitfile_path, line_no, content);
                arena_free(line);
                return false;
            }
            char *val = extract_top_value(content, "version:");
            if (!val) {
                log_error("%s:%d: empty 'version:' for kit", kitfile_path, line_no);
                arena_free(line);
                return false;
            }
            if (m->kit_version) {
                log_error("%s:%d: duplicate 'version:' for kit", kitfile_path, line_no);
                arena_free(val);
                arena_free(line);
                return false;
            }
            m->kit_version = val;
            arena_free(line);
            continue;
        }

        if (active == SECTION_TOOL && active_idx >= 0) {
            if (!starts_with(content, "url:")) {
                log_error("%s:%d: 'tool:' block only allows 'url:' (got '%s')",
                          kitfile_path, line_no, content);
                arena_free(line);
                return false;
            }
            char *val = extract_top_value(content, "url:");
            if (!val) {
                log_error("%s:%d: empty 'url:' for tool", kitfile_path, line_no);
                arena_free(line);
                return false;
            }
            if (!url_is_http(val)) {
                log_error("%s:%d: tool url must start with http:// or https://",
                          kitfile_path, line_no);
                arena_free(val);
                arena_free(line);
                return false;
            }
            if (m->tools[active_idx].url) {
                log_error("%s:%d: duplicate 'url:' for tool", kitfile_path, line_no);
                arena_free(val);
                arena_free(line);
                return false;
            }
            m->tools[active_idx].url = val;
            arena_free(line);
            continue;
        }

        log_error("%s:%d: indented property has no parent block",
                  kitfile_path, line_no);
        arena_free(line);
        return false;
    }

    /* Final validations. */
    if (!seen_format) {
        log_error("%s: missing 'format 2' header", kitfile_path);
        return false;
    }
    if (!m->has_kit || !m->kit_name) {
        log_error("%s: kitfile is missing required 'kit:' entry", kitfile_path);
        return false;
    }
    if (!m->kit_version || m->kit_version[0] == '\0') {
        log_error("%s: 'kit:' entry must have a non-empty 'version:'", kitfile_path);
        return false;
    }
    for (int i = 0; i < m->tool_count; i++) {
        if (!m->tools[i].url) {
            log_error("%s: tool '%s' is missing a 'url:'", kitfile_path,
                      m->tools[i].name);
            return false;
        }
    }
    return true;
}

/* ===========================================================================
 * Discover prebuilt tools and packages on disk
 * ========================================================================= */

/* Check whether tools/TOOL_NAME/ contains any LICENSE file. Looks for a
 * direct LICENSE at the tool's top-level directory, or under any version
 * subdirectory `tools/TOOL_NAME/<dir>/LICENSE`. */
static bool tool_has_local_license(const char *kitfile_dir, const char *tool_name) {
    char tool_root[PATH_MAX];
    int n = snprintf(tool_root, sizeof(tool_root), "%s/tools/%s",
                     kitfile_dir, tool_name);
    if (n <= 0 || (size_t)n >= sizeof(tool_root)) return false;
    if (!path_is_directory(tool_root)) return false;

    char direct[PATH_MAX];
    int dn = snprintf(direct, sizeof(direct), "%s/LICENSE", tool_root);
    if (dn > 0 && (size_t)dn < sizeof(direct) &&
        path_is_nonempty_regular_file(direct)) {
        return true;
    }

    DIR *d = opendir(tool_root);
    if (!d) return false;
    struct dirent *ent;
    bool found = false;
    while (!found && (ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char sub[PATH_MAX];
        int sn = snprintf(sub, sizeof(sub), "%s/%s/LICENSE",
                          tool_root, ent->d_name);
        if (sn <= 0 || (size_t)sn >= sizeof(sub)) continue;
        if (path_is_nonempty_regular_file(sub)) {
            found = true;
        }
    }
    closedir(d);
    return found;
}

/* Scan tools/NAME/VERSION/ for a binary at bin/NAME and a LICENSE file.
 * Adds discovered tools to the manifest unless overridden by a `tool:` entry.
 * Returns true on success (no I/O error). */
static bool scan_tools_folder(KitManifest *m) {
    char *tools_root = join_path(m->kitfile_dir, "tools");
    if (!tools_root) return false;
    if (!path_is_directory(tools_root)) {
        arena_free(tools_root);
        return true;
    }

    DIR *d = opendir(tools_root);
    if (!d) {
        log_error("Cannot scan tools directory '%s': %s", tools_root, strerror(errno));
        arena_free(tools_root);
        return false;
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (!valid_tool_name(ent->d_name)) continue;
        if (manifest_has_tool_named(m, ent->d_name)) continue;

        char *tool_dir = join_path(tools_root, ent->d_name);
        if (!tool_dir) continue;
        if (!path_is_directory(tool_dir)) {
            arena_free(tool_dir);
            continue;
        }

        /* Find a version subdirectory containing bin/NAME and LICENSE. */
        DIR *td = opendir(tool_dir);
        if (!td) {
            arena_free(tool_dir);
            continue;
        }
        char *chosen_version = NULL;
        struct dirent *vent;
        while ((vent = readdir(td)) != NULL) {
            if (vent->d_name[0] == '.') continue;
            char *vdir = join_path(tool_dir, vent->d_name);
            if (!vdir) continue;
            if (!path_is_directory(vdir)) {
                arena_free(vdir);
                continue;
            }
            char bin_path[PATH_MAX];
            char lic_path[PATH_MAX];
            int bn = snprintf(bin_path, sizeof(bin_path), "%s/bin/%s",
                              vdir, ent->d_name);
            int ln = snprintf(lic_path, sizeof(lic_path), "%s/LICENSE", vdir);
            if (bn <= 0 || (size_t)bn >= sizeof(bin_path) ||
                ln <= 0 || (size_t)ln >= sizeof(lic_path)) {
                arena_free(vdir);
                continue;
            }
            if (path_is_nonempty_regular_file(bin_path) &&
                path_is_nonempty_regular_file(lic_path)) {
                if (chosen_version) arena_free(chosen_version);
                chosen_version = arena_strdup(bin_path);
                /* Keep scanning — last lexical version wins; we don't sort
                 * versions, but discovery is meant for cases where the kit
                 * ships a single version. */
            }
            arena_free(vdir);
        }
        closedir(td);
        arena_free(tool_dir);

        if (chosen_version) {
            KitTool t = {
                .name = arena_strdup(ent->d_name),
                .url = NULL,
                .local_bin_path = chosen_version,
            };
            if (!t.name || !manifest_push_tool(m, t)) {
                closedir(d);
                arena_free(tools_root);
                return false;
            }
        }
    }
    closedir(d);
    arena_free(tools_root);
    return true;
}

/* Scan packages/ELM_VERSION/AUTHOR/NAME/VERSION for valid packages and add
 * them to the manifest unless an explicit `package:` entry shadows them. */
static bool scan_packages_folder(KitManifest *m) {
    if (!m->compiler_version) return true;

    char buf[PATH_MAX];
    int n = snprintf(buf, sizeof(buf), "%s/packages/%s",
                     m->kitfile_dir, m->compiler_version);
    if (n <= 0 || (size_t)n >= sizeof(buf)) return true;
    if (!path_is_directory(buf)) return true;

    DIR *dpa = opendir(buf);
    if (!dpa) return true;

    struct dirent *aent;
    while ((aent = readdir(dpa)) != NULL) {
        if (aent->d_name[0] == '.') continue;
        char author_dir[PATH_MAX];
        int an = snprintf(author_dir, sizeof(author_dir), "%s/%s",
                          buf, aent->d_name);
        if (an <= 0 || (size_t)an >= sizeof(author_dir)) continue;
        if (!path_is_directory(author_dir)) continue;

        DIR *dpn = opendir(author_dir);
        if (!dpn) continue;
        struct dirent *nent;
        while ((nent = readdir(dpn)) != NULL) {
            if (nent->d_name[0] == '.') continue;
            char name_dir[PATH_MAX];
            int nn = snprintf(name_dir, sizeof(name_dir), "%s/%s",
                              author_dir, nent->d_name);
            if (nn <= 0 || (size_t)nn >= sizeof(name_dir)) continue;
            if (!path_is_directory(name_dir)) continue;

            DIR *dpv = opendir(name_dir);
            if (!dpv) continue;
            struct dirent *vent;
            while ((vent = readdir(dpv)) != NULL) {
                if (vent->d_name[0] == '.') continue;
                char ver_dir[PATH_MAX];
                int vn = snprintf(ver_dir, sizeof(ver_dir), "%s/%s",
                                  name_dir, vent->d_name);
                if (vn <= 0 || (size_t)vn >= sizeof(ver_dir)) continue;
                if (!path_is_directory(ver_dir)) continue;

                char elm_json_path[PATH_MAX];
                int en = snprintf(elm_json_path, sizeof(elm_json_path),
                                  "%s/elm.json", ver_dir);
                if (en <= 0 || (size_t)en >= sizeof(elm_json_path)) continue;
                if (!path_is_regular_file(elm_json_path)) continue;

                /* Skip if an explicit `package:` already names this. */
                if (manifest_has_package(m, aent->d_name, nent->d_name, vent->d_name) ||
                    manifest_has_package(m, aent->d_name, nent->d_name, NULL)) {
                    continue;
                }

                /* Also skip if it's already a local-dev (those win). */
                bool is_local_dev = false;
                for (int i = 0; i < m->local_dev_count; i++) {
                    if (strcmp(m->local_devs[i].author, aent->d_name) == 0 &&
                        strcmp(m->local_devs[i].name, nent->d_name) == 0 &&
                        strcmp(m->local_devs[i].version, vent->d_name) == 0) {
                        is_local_dev = true;
                        break;
                    }
                }
                if (is_local_dev) continue;

                KitPackage p = {
                    .author = arena_strdup(aent->d_name),
                    .name = arena_strdup(nent->d_name),
                    .version = arena_strdup(vent->d_name),
                };
                if (!p.author || !p.name || !p.version) {
                    closedir(dpv);
                    closedir(dpn);
                    closedir(dpa);
                    return false;
                }
                if (!manifest_push_package(m, p)) {
                    closedir(dpv);
                    closedir(dpn);
                    closedir(dpa);
                    return false;
                }
            }
            closedir(dpv);
        }
        closedir(dpn);
    }
    closedir(dpa);
    return true;
}

/* Apply the local source model used by both `kit install` and local-path
 * `kit update`: discover sibling tools/packages and require a LICENSE for
 * URL-backed tools. URL sources skip this because they have no sibling tree. */
static bool kit_prepare_local_source_manifest(KitManifest *m) {
    if (!m->kitfile_dir) return true;

    if (!scan_tools_folder(m)) return false;
    if (!scan_packages_folder(m)) return false;

    int kept = 0;
    for (int i = 0; i < m->tool_count; i++) {
        const KitTool *t = &m->tools[i];
        if (t->url && !tool_has_local_license(m->kitfile_dir, t->name)) {
            fprintf(stderr,
                    "Skipping tool '%s': no LICENSE found under tools/%s/\n",
                    t->name, t->name);
            continue;
        }
        if (kept != i) m->tools[kept] = m->tools[i];
        kept++;
    }
    m->tool_count = kept;
    return true;
}

static int kit_verify_dry_run(KitManifest *m, const char *source,
                              bool check_urls, CurlSession *session);

/* ===========================================================================
 * Installation actions
 * ========================================================================= */

/* Ensure a directory exists, prompting the user to create it if missing.
 * If the directory exists but isn't writable, returns false (caller errors). */
static bool ensure_tool_bin_path(const char *tool_bin_path, bool auto_yes) {
    struct stat st;
    if (stat(tool_bin_path, &st) == 0) {
        if (!S_ISDIR(st.st_mode)) {
            log_error("Tool install path is not a directory: %s", tool_bin_path);
            return false;
        }
        if (access(tool_bin_path, W_OK) != 0) {
            log_error("No write permission for '%s'", tool_bin_path);
            return false;
        }
        return true;
    }
    if (errno != ENOENT) {
        log_error("Cannot stat '%s': %s", tool_bin_path, strerror(errno));
        return false;
    }
    char prompt[MAX_PATH_LENGTH + 64];
    int pn = snprintf(prompt, sizeof(prompt),
                      "Tool install directory '%s' does not exist. Create it?",
                      tool_bin_path);
    if (pn <= 0 || (size_t)pn >= sizeof(prompt)) {
        log_error("Tool install path too long: %s", tool_bin_path);
        return false;
    }
    if (!confirm_prompt(prompt, auto_yes)) {
        log_error("Cannot install tools without '%s'", tool_bin_path);
        return false;
    }
    if (!mkdir_p(tool_bin_path)) {
        log_error("Failed to create '%s': %s", tool_bin_path, strerror(errno));
        return false;
    }
    return true;
}

/* Set an error message into a caller-supplied buffer, if provided. */
static void set_err(char *buf, size_t buf_size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#include <stdarg.h>
static void set_err(char *buf, size_t buf_size, const char *fmt, ...) {
    if (!buf || buf_size == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, buf_size, fmt, ap);
    va_end(ap);
}

/* Create or replace a symlink at link_path pointing to target. */
static bool replace_symlink(const char *target, const char *link_path,
                             char *err, size_t err_size) {
    struct stat st;
    if (lstat(link_path, &st) == 0) {
        /* Remove existing link or file; never recursively remove a directory. */
        if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
            set_err(err, err_size, "refusing to replace existing directory: %s",
                    link_path);
            return false;
        }
        if (unlink(link_path) != 0) {
            set_err(err, err_size, "unlink('%s'): %s",
                    link_path, strerror(errno));
            return false;
        }
    } else if (errno != ENOENT) {
        set_err(err, err_size, "lstat('%s'): %s",
                link_path, strerror(errno));
        return false;
    }
    if (symlink(target, link_path) != 0) {
        set_err(err, err_size, "symlink('%s' -> '%s'): %s",
                link_path, target, strerror(errno));
        return false;
    }
    return true;
}

/* Progress callback state — prints dots to stdout every PROGRESS_BYTES_PER_DOT
 * bytes received. Holds the human-readable total size shown after download. */
typedef struct {
    long long last_dot_bytes;
    long long total_bytes;
} DotProgress;

static int dot_progress_cb(void *userdata, double total_bytes,
                            double downloaded_bytes) {
    DotProgress *p = (DotProgress *)userdata;
    if (!p) return 0;
    if (total_bytes > 0) p->total_bytes = (long long)total_bytes;
    if (downloaded_bytes <= 0) return 0;
    long long now = (long long)downloaded_bytes;
    while (now - p->last_dot_bytes >= PROGRESS_BYTES_PER_DOT) {
        printf(".");
        fflush(stdout);
        p->last_dot_bytes += PROGRESS_BYTES_PER_DOT;
    }
    return 0;
}

/* Download a URL into $WRAP_HOME/tools/NAME/KIT_VERSION/bin/NAME atomically.
 * Returns arena-allocated absolute path to the installed binary, or NULL. */
static char *download_tool_binary(CurlSession *session, const KitTool *tool,
                                   const char *kit_version,
                                   char *err, size_t err_size,
                                   long long *out_bytes) {
    if (out_bytes) *out_bytes = 0;

    char *wrap_home = env_get_wrap_home();
    if (!wrap_home || wrap_home[0] == '\0') {
        set_err(err, err_size, "WRAP_HOME is not configured");
        arena_free(wrap_home);
        return NULL;
    }

    char tool_dir[PATH_MAX];
    int n = snprintf(tool_dir, sizeof(tool_dir), "%s/%s/%s/%s/bin",
                     wrap_home, KIT_TOOLS_DIR, tool->name, kit_version);
    arena_free(wrap_home);
    if (n <= 0 || (size_t)n >= sizeof(tool_dir)) {
        set_err(err, err_size, "install path too long");
        return NULL;
    }
    if (!mkdir_p(tool_dir)) {
        set_err(err, err_size, "mkdir_p('%s'): %s", tool_dir, strerror(errno));
        return NULL;
    }

    char dest_path[PATH_MAX];
    int dn = snprintf(dest_path, sizeof(dest_path), "%s/%s", tool_dir, tool->name);
    if (dn <= 0 || (size_t)dn >= sizeof(dest_path)) {
        set_err(err, err_size, "install path too long");
        return NULL;
    }

    char tmp_path[PATH_MAX];
    int tn = snprintf(tmp_path, sizeof(tmp_path), "%s.part.%ld",
                      dest_path, (long)getpid());
    if (tn <= 0 || (size_t)tn >= sizeof(tmp_path)) {
        set_err(err, err_size, "tmp path too long");
        return NULL;
    }

    DotProgress prog = { 0, 0 };
    char curl_err[256];
    long http_code = 0;
    HttpResult r = http_download_file_ex(session, tool->url, tmp_path,
                                          dot_progress_cb, &prog,
                                          &http_code, curl_err, sizeof(curl_err));
    if (r != HTTP_OK) {
        if (curl_err[0] != '\0') {
            set_err(err, err_size, "%s (%s)", curl_err,
                    http_result_to_string(r));
        } else if (http_code > 0) {
            set_err(err, err_size, "HTTP %ld: %s",
                    http_code, http_result_to_string(r));
        } else {
            set_err(err, err_size, "%s", http_result_to_string(r));
        }
        unlink(tmp_path);
        return NULL;
    }

    if (chmod(tmp_path, 0755) != 0) {
        set_err(err, err_size, "chmod('%s'): %s", tmp_path, strerror(errno));
        unlink(tmp_path);
        return NULL;
    }

    if (os_rename_replace(tmp_path, dest_path) != 0) {
        set_err(err, err_size, "rename('%s'): %s", dest_path, strerror(errno));
        unlink(tmp_path);
        return NULL;
    }

    if (out_bytes) *out_bytes = prog.total_bytes;
    return arena_strdup(dest_path);
}

/* Symlink a tool binary into TOOL_BIN_PATH. */
static bool symlink_tool(const char *bin_path, const char *tool_name,
                          const char *tool_bin_path,
                          char *err, size_t err_size) {
    char link_path[PATH_MAX];
    int ln = snprintf(link_path, sizeof(link_path), "%s/%s",
                      tool_bin_path, tool_name);
    if (ln <= 0 || (size_t)ln >= sizeof(link_path)) {
        set_err(err, err_size, "symlink path too long");
        return false;
    }
    return replace_symlink(bin_path, link_path, err, err_size);
}

/* Install a single package: copy from packages/ELM_VERSION/AUTHOR/NAME/VERSION
 * if it exists, otherwise fall through to the registry. */
static bool install_one_package(const KitManifest *m, InstallEnv *env,
                                 const KitPackage *p,
                                 char *err, size_t err_size) {
    /* If we have a version + on-disk source, install from there. */
    if (p->version) {
        char src[PATH_MAX];
        int n = snprintf(src, sizeof(src), "%s/packages/%s/%s/%s/%s",
                         m->kitfile_dir, m->compiler_version,
                         p->author, p->name, p->version);
        if (n > 0 && (size_t)n < sizeof(src) && path_is_directory(src)) {
            char elm_json[PATH_MAX];
            int en = snprintf(elm_json, sizeof(elm_json), "%s/elm.json", src);
            if (en > 0 && (size_t)en < sizeof(elm_json) &&
                path_is_regular_file(elm_json)) {
                if (!install_from_file(src, env, p->author, p->name, p->version)) {
                    set_err(err, err_size, "install_from_file('%s') failed", src);
                    return false;
                }
                return true;
            }
        }
    }

    /* If no version, look for the first directory under
     * packages/ELM_VERSION/AUTHOR/NAME/ with a valid elm.json. */
    if (!p->version) {
        char name_dir[PATH_MAX];
        int nn = snprintf(name_dir, sizeof(name_dir), "%s/packages/%s/%s/%s",
                          m->kitfile_dir, m->compiler_version, p->author, p->name);
        if (nn > 0 && (size_t)nn < sizeof(name_dir) && path_is_directory(name_dir)) {
            DIR *d = opendir(name_dir);
            if (d) {
                struct dirent *ent;
                while ((ent = readdir(d)) != NULL) {
                    if (ent->d_name[0] == '.') continue;
                    char ver_dir[PATH_MAX];
                    int vn = snprintf(ver_dir, sizeof(ver_dir), "%s/%s",
                                      name_dir, ent->d_name);
                    if (vn <= 0 || (size_t)vn >= sizeof(ver_dir)) continue;
                    if (!path_is_directory(ver_dir)) continue;
                    char elm_json[PATH_MAX];
                    int en = snprintf(elm_json, sizeof(elm_json), "%s/elm.json",
                                      ver_dir);
                    if (en <= 0 || (size_t)en >= sizeof(elm_json)) continue;
                    if (!path_is_regular_file(elm_json)) continue;

                    bool ok = install_from_file(ver_dir, env, p->author, p->name,
                                                ent->d_name);
                    closedir(d);
                    if (!ok) {
                        set_err(err, err_size, "install_from_file('%s') failed",
                                ver_dir);
                    }
                    return ok;
                }
                closedir(d);
            }
        }
        set_err(err, err_size,
                "no version specified and no on-disk fallback at %s/packages/%s/%s/%s/",
                m->kitfile_dir, m->compiler_version, p->author, p->name);
        return false;
    }

    /* Fall back to registry. */
    if (!install_env_download_package(env, p->author, p->name, p->version)) {
        set_err(err, err_size,
                "registry download failed for %s/%s %s",
                p->author, p->name, p->version);
        return false;
    }
    return true;
}

/* Install a local-dev package from packages/ELM_VERSION/AUTHOR/NAME/VERSION. */
static bool install_one_local_dev(const KitManifest *m, InstallEnv *env,
                                   const KitLocalDev *ld,
                                   char *err, size_t err_size) {
    char src[PATH_MAX];
    int n = snprintf(src, sizeof(src), "%s/packages/%s/%s/%s/%s",
                     m->kitfile_dir, m->compiler_version,
                     ld->author, ld->name, ld->version);
    if (n <= 0 || (size_t)n >= sizeof(src)) {
        set_err(err, err_size, "path too long");
        return false;
    }
    if (!path_is_directory(src)) {
        set_err(err, err_size, "path not found: %s", src);
        return false;
    }
    char elm_json[PATH_MAX];
    int en = snprintf(elm_json, sizeof(elm_json), "%s/elm.json", src);
    if (en <= 0 || (size_t)en >= sizeof(elm_json) ||
        !path_is_regular_file(elm_json)) {
        set_err(err, err_size, "missing elm.json at %s", src);
        return false;
    }
    int rc = register_local_dev_package(src, ld->author, ld->name, ld->version,
                                         env, true, true);
    if (rc != 0) {
        set_err(err, err_size, "register_local_dev_package returned %d", rc);
        return false;
    }
    return true;
}

/* ===========================================================================
 * Plan output
 * ========================================================================= */

static void print_plan(const KitManifest *m) {
    printf("Here is my plan:\n");
    printf("\n");
    printf("  Kit:     %s %s\n", m->kit_name, m->kit_version);
    printf("  Compiler: %s %s\n", m->compiler_name, m->compiler_version);
    printf("\n");
    if (m->local_dev_count > 0) {
        printf("  Local-dev packages (%d):\n", m->local_dev_count);
        for (int i = 0; i < m->local_dev_count; i++) {
            printf("    %s/%s    %s (local)\n",
                   m->local_devs[i].author, m->local_devs[i].name,
                   m->local_devs[i].version);
        }
        printf("\n");
    }
    if (m->package_count > 0) {
        printf("  Packages (%d):\n", m->package_count);
        for (int i = 0; i < m->package_count; i++) {
            const KitPackage *p = &m->packages[i];
            if (p->version) {
                printf("    %s/%s    %s\n", p->author, p->name, p->version);
            } else {
                printf("    %s/%s    (latest)\n", p->author, p->name);
            }
        }
        printf("\n");
    }
    if (m->tool_count > 0) {
        printf("  Tools (%d):\n", m->tool_count);
        for (int i = 0; i < m->tool_count; i++) {
            const KitTool *t = &m->tools[i];
            if (t->url) {
                printf("    %s (download: %s)\n", t->name, t->url);
            } else {
                printf("    %s (local: %s)\n", t->name, t->local_bin_path);
            }
        }
        printf("\n");
    }
}

/* ===========================================================================
 * Shared install actions (used by `kit install` and `kit update`)
 * ========================================================================= */

/* A growable list of human-readable failure descriptions for the summary. */
typedef struct {
    char **items;
    int count;
    int capacity;
} FailList;

static void faillist_addf(FailList *fl, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
static void faillist_addf(FailList *fl, const char *fmt, ...) {
    if (!fl) return;
    char buf[MAX_PACKAGE_NAME_LENGTH];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    char *copy = arena_strdup(buf);
    if (copy) {
        DYNARRAY_PUSH(fl->items, fl->count, fl->capacity, copy, char *);
    }
}

/* Install all local-dev packages (best-effort), recording failures. */
static void install_all_local_devs(const KitManifest *m, InstallEnv *env, FailList *fl) {
    if (m->local_dev_count <= 0) return;
    char err[MAX_ERROR_MESSAGE_LENGTH];
    printf("\nInstalling local-dev packages:\n");
    for (int i = 0; i < m->local_dev_count; i++) {
        const KitLocalDev *ld = &m->local_devs[i];
        printf("  - %s/%s %s ... ", ld->author, ld->name, ld->version);
        fflush(stdout);
        err[0] = '\0';
        if (install_one_local_dev(m, env, ld, err, sizeof(err))) {
            printf("OK\n");
        } else {
            printf("FAILED\n");
            if (err[0] != '\0') printf("      %s\n", err);
            faillist_addf(fl, "local-dev %s/%s %s", ld->author, ld->name, ld->version);
        }
    }
}

/* Install all regular packages (best-effort), recording failures. */
static void install_all_packages(const KitManifest *m, InstallEnv *env, FailList *fl) {
    if (m->package_count <= 0) return;
    char err[MAX_ERROR_MESSAGE_LENGTH];
    printf("\nInstalling packages:\n");
    for (int i = 0; i < m->package_count; i++) {
        const KitPackage *p = &m->packages[i];
        printf("  - %s/%s %s ... ", p->author, p->name,
               p->version ? p->version : "(latest)");
        fflush(stdout);
        err[0] = '\0';
        if (install_one_package(m, env, p, err, sizeof(err))) {
            printf("OK\n");
        } else {
            printf("FAILED\n");
            if (err[0] != '\0') printf("      %s\n", err);
            faillist_addf(fl, "package %s/%s %s", p->author, p->name,
                          p->version ? p->version : "(latest)");
        }
    }
}

/* Download (URL tools) and symlink every tool into TOOL_BIN_PATH (best-effort).
 * Returns false only on a fatal error (HTTP client init); per-tool failures are
 * recorded in `fl`. */
static bool install_all_tools(const KitManifest *m, const char *tool_bin_path,
                              FailList *fl, int *tools_installed) {
    if (m->tool_count <= 0) return true;
    printf("\nInstalling tools:\n");
    CurlSession *session = NULL;
    bool need_network = false;
    for (int i = 0; i < m->tool_count; i++) {
        if (m->tools[i].url) { need_network = true; break; }
    }
    if (need_network) {
        session = curl_session_create();
        if (!session) {
            log_error("Failed to initialize HTTP client");
            return false;
        }
        /* Tool downloads can be large; bump the default 10s timeout. */
        curl_session_set_timeout(session, 600000L);
    }

    char err[MAX_ERROR_MESSAGE_LENGTH];
    for (int i = 0; i < m->tool_count; i++) {
        const KitTool *t = &m->tools[i];
        err[0] = '\0';
        const char *bin_path = NULL;
        char *downloaded = NULL;
        long long downloaded_bytes = 0;

        if (t->url) {
            printf("  - %s ", t->name);
            fflush(stdout);
            downloaded = download_tool_binary(session, t, m->kit_version,
                                              err, sizeof(err), &downloaded_bytes);
            if (!downloaded) {
                printf("FAILED\n");
                if (err[0] != '\0') printf("      %s\n", err);
                faillist_addf(fl, "tool %s (download: %s)", t->name, t->url);
                continue;
            }
            bin_path = downloaded;
            if (downloaded_bytes > 0) {
                printf(" %.1f KB", (double)downloaded_bytes / BYTES_PER_KB);
            }
            printf(" ... ");
            fflush(stdout);
        } else {
            printf("  - %s (local) ... ", t->name);
            fflush(stdout);
            bin_path = t->local_bin_path;
        }

        err[0] = '\0';
        if (!symlink_tool(bin_path, t->name, tool_bin_path, err, sizeof(err))) {
            printf("FAILED\n");
            if (err[0] != '\0') printf("      %s\n", err);
            faillist_addf(fl, "tool %s (symlink)", t->name);
        } else {
            printf("OK\n");
            (*tools_installed)++;
        }
        if (downloaded) arena_free(downloaded);
    }

    if (session) curl_session_free(session);
    return true;
}

/* ===========================================================================
 * Kit state management (used by `kit update`)
 *
 * Installed kit state lives under $WRAP_HOME/kits/<KIT>/<VERSION>/<KIT>.kit,
 * with $WRAP_HOME/kits/<KIT>/current symlinked to the active version dir.
 * ========================================================================= */

/* Lowercase hex SHA-256 of a buffer. `out` must hold 2*SHA256_BLOCK_SIZE+1. */
static void kit_hash_hex(const char *data, size_t len, char *out) {
    SHA256_CTX ctx;
    BYTE digest[SHA256_BLOCK_SIZE];
    sha256_init(&ctx);
    sha256_update(&ctx, (const BYTE *)data, len);
    sha256_final(&ctx, digest);
    static const char hexd[] = "0123456789abcdef";
    for (int i = 0; i < SHA256_BLOCK_SIZE; i++) {
        out[2 * i] = hexd[(digest[i] >> 4) & 0xF];
        out[2 * i + 1] = hexd[digest[i] & 0xF];
    }
    out[2 * SHA256_BLOCK_SIZE] = '\0';
}

/* Read and parse the kit currently linked at `current_link` (a symlink to a
 * version directory). Returns true and fills the out-params on success; false
 * when there is no usable current kit. No folder scanning is performed: the
 * stored kitfile lists exactly the explicit entries to tear down later. */
static bool kit_read_current_manifest(const char *current_link, KitManifest *out_m,
                                      char **out_body, size_t *out_len) {
    if (!path_is_directory(current_link)) {
        return false; /* missing or broken 'current' symlink */
    }
    char *cur_kit = find_kit_in_directory(current_link);
    if (!cur_kit) {
        return false;
    }
    size_t len = 0;
    char *body = file_read_contents_bounded(cur_kit, MAX_KIT_FILE_BYTES, &len);
    if (!body || len == 0) {
        return false;
    }
    manifest_init(out_m);
    if (!kitfile_parse(body, len, out_m, cur_kit)) {
        return false;
    }
    *out_body = body;
    *out_len = len;
    return true;
}

/* Write the kitfile body to <version_dir>/<filename> (creating directories). */
static bool kit_store_kitfile(const char *version_dir, const char *filename,
                              const char *body, size_t len) {
    if (!mkdir_p(version_dir)) {
        log_error("Failed to create kit directory '%s': %s",
                  version_dir, strerror(errno));
        return false;
    }
    char dest[PATH_MAX];
    int n = snprintf(dest, sizeof(dest), "%s/%s", version_dir, filename);
    if (n <= 0 || (size_t)n >= sizeof(dest)) {
        log_error("Kit storage path too long");
        return false;
    }
    if (!file_write_bytes_atomic(dest, body, len)) {
        log_error("Failed to write kitfile to '%s'", dest);
        return false;
    }
    return true;
}

/* Point <kit_dir>/current at <version_dir>, replacing any existing symlink. */
static bool kit_promote_current(const char *kit_dir, const char *version_dir) {
    char current_link[PATH_MAX];
    int n = snprintf(current_link, sizeof(current_link), "%s/current", kit_dir);
    if (n <= 0 || (size_t)n >= sizeof(current_link)) {
        log_error("Kit 'current' path too long");
        return false;
    }
    char err[MAX_TEMP_BUFFER_LENGTH];
    err[0] = '\0';
    if (!replace_symlink(version_dir, current_link, err, sizeof(err))) {
        log_error("Failed to update 'current' symlink: %s", err);
        return false;
    }
    return true;
}

/* Remove the ELM_HOME local-dev cache symlink for a package (if it is one). */
static void kit_remove_cache_symlink(InstallEnv *env, const char *author,
                                     const char *name, const char *version) {
    if (!env || !env->cache || !env->cache->packages_dir) return;
    char link_path[PATH_MAX];
    int n = snprintf(link_path, sizeof(link_path), "%s/%s/%s/%s",
                     env->cache->packages_dir, author, name, version);
    if (n <= 0 || (size_t)n >= sizeof(link_path)) return;
    struct stat st;
    if (lstat(link_path, &st) == 0 && S_ISLNK(st.st_mode)) {
        unlink(link_path);
    }
}

/* Remove the TOOL_BIN_PATH symlinks for the tools listed in `old_m`. Only
 * removes entries that are symlinks; never touches real files or directories. */
static void remove_old_tool_symlinks(const KitManifest *old_m, const char *tool_bin_path) {
    if (!tool_bin_path) return;
    for (int i = 0; i < old_m->tool_count; i++) {
        const char *name = old_m->tools[i].name;
        char link_path[PATH_MAX];
        int n = snprintf(link_path, sizeof(link_path), "%s/%s", tool_bin_path, name);
        if (n <= 0 || (size_t)n >= sizeof(link_path)) continue;
        struct stat st;
        if (lstat(link_path, &st) == 0 && S_ISLNK(st.st_mode)) {
            if (unlink(link_path) == 0) {
                printf("  - removed tool symlink: %s\n", name);
            }
        }
    }
}

/* Deregister the local-dev packages listed in `old_m`: remove the ELM_HOME
 * cache symlink and the registry + tracking entries. */
static void deregister_old_local_devs(const KitManifest *old_m, InstallEnv *env) {
    for (int i = 0; i < old_m->local_dev_count; i++) {
        const KitLocalDev *ld = &old_m->local_devs[i];
        kit_remove_cache_symlink(env, ld->author, ld->name, ld->version);
        repository_clear_local_dev_registration(ld->author, ld->name, ld->version);
        printf("  - removed local-dev: %s/%s %s\n", ld->author, ld->name, ld->version);
    }
}

/* Download every tool to its versioned WRAP_HOME location, recording each
 * resulting binary path in out_bin_paths (local tools keep their on-disk path).
 * Returns false on the FIRST failure so the caller can abort an update without
 * having changed any installed state. */
static bool kit_download_tools(const KitManifest *m, char **out_bin_paths) {
    if (m->tool_count <= 0) return true;

    CurlSession *session = NULL;
    bool need_network = false;
    for (int i = 0; i < m->tool_count; i++) {
        if (m->tools[i].url) { need_network = true; break; }
    }
    if (need_network) {
        session = curl_session_create();
        if (!session) {
            log_error("Failed to initialize HTTP client");
            return false;
        }
        curl_session_set_timeout(session, 600000L);
    }

    char err[MAX_ERROR_MESSAGE_LENGTH];
    bool ok = true;
    for (int i = 0; i < m->tool_count; i++) {
        const KitTool *t = &m->tools[i];
        if (t->url) {
            printf("  - %s ", t->name);
            fflush(stdout);
            long long bytes = 0;
            err[0] = '\0';
            char *dl = download_tool_binary(session, t, m->kit_version,
                                            err, sizeof(err), &bytes);
            if (!dl) {
                printf("FAILED\n");
                if (err[0] != '\0') printf("      %s\n", err);
                ok = false;
                break;
            }
            if (bytes > 0) printf(" %.1f KB", (double)bytes / BYTES_PER_KB);
            printf(" ... downloaded\n");
            out_bin_paths[i] = dl;
        } else {
            out_bin_paths[i] = arena_strdup(t->local_bin_path);
            if (!out_bin_paths[i]) { ok = false; break; }
            printf("  - %s (local)\n", t->name);
        }
    }

    if (session) curl_session_free(session);
    return ok;
}

/* Symlink already-downloaded tools (bin_paths) into TOOL_BIN_PATH (best-effort). */
static void kit_symlink_tools(const KitManifest *m, char **bin_paths,
                              const char *tool_bin_path, FailList *fl,
                              int *tools_installed) {
    char err[MAX_ERROR_MESSAGE_LENGTH];
    for (int i = 0; i < m->tool_count; i++) {
        const KitTool *t = &m->tools[i];
        printf("  - %s ... ", t->name);
        fflush(stdout);
        err[0] = '\0';
        if (symlink_tool(bin_paths[i], t->name, tool_bin_path, err, sizeof(err))) {
            printf("OK\n");
            (*tools_installed)++;
        } else {
            printf("FAILED\n");
            if (err[0] != '\0') printf("      %s\n", err);
            faillist_addf(fl, "tool %s (symlink)", t->name);
        }
    }
}

/* ===========================================================================
 * Main command entry
 * ========================================================================= */

int cmd_kit_install(int argc, char *argv[]) {
    /* Unbuffered stdout keeps per-item progress lines and download dots in
     * the right order when stderr also writes (e.g., install_from_file). */
    setvbuf(stdout, NULL, _IOLBF, 0);

    const char *path_arg = NULL;
    bool auto_yes = false;
    bool dry_run = false;
    bool check_urls = false;

    for (int i = 1; i < argc; i++) {
        if (is_help_flag(argv[i])) {
            print_install_usage();
            return 0;
        }
        if (strcmp(argv[i], "-y") == 0 || strcmp(argv[i], "--yes") == 0) {
            auto_yes = true;
        } else if (strcmp(argv[i], "--dry-run") == 0) {
            dry_run = true;
        } else if (strcmp(argv[i], "--check-urls") == 0) {
            check_urls = true;
        } else if (argv[i][0] != '-') {
            if (path_arg) {
                log_error("Unexpected argument '%s'", argv[i]);
                print_install_usage();
                return 1;
            }
            path_arg = argv[i];
        } else {
            log_error("Unknown option: %s", argv[i]);
            print_install_usage();
            return 1;
        }
    }

    if (!path_arg) {
        print_install_usage();
        return 1;
    }

    /* Resolve PATH to an absolute kitfile path. */
    char *kitfile_path = NULL;
    if (path_ends_with(path_arg, ".kit")) {
        if (!path_is_regular_file(path_arg)) {
            log_error("Kitfile not found: %s", path_arg);
            return 1;
        }
        kitfile_path = absolute_path_of(path_arg);
    } else {
        if (!path_is_directory(path_arg)) {
            log_error("Not a directory and not a .kit file: %s", path_arg);
            return 1;
        }
        char *abs = absolute_path_of(path_arg);
        if (!abs) {
            log_error("Cannot resolve path: %s", path_arg);
            return 1;
        }
        kitfile_path = find_kit_in_directory(abs);
    }
    if (!kitfile_path) {
        return 1;
    }

    char *kitfile_dir = parent_directory_of(kitfile_path);
    if (!kitfile_dir) {
        log_error("Cannot determine parent directory of '%s'", kitfile_path);
        return 1;
    }

    /* Read the kitfile (size-bounded). */
    size_t body_len = 0;
    char *body = file_read_contents_bounded(kitfile_path, MAX_KIT_FILE_BYTES, &body_len);
    if (!body) {
        log_error("Cannot read kitfile: %s", kitfile_path);
        return 1;
    }

    KitManifest m;
    manifest_init(&m);
    m.kitfile_path = kitfile_path;
    m.kitfile_dir = kitfile_dir;

    if (!kitfile_parse(body, body_len, &m, kitfile_path)) {
        return 1;
    }
    if (dry_run) {
        return kit_verify_dry_run(&m, kitfile_path, check_urls, NULL);
    }

    if (!kit_prepare_local_source_manifest(&m)) {
        return 1;
    }

    print_plan(&m);

    if (!auto_yes) {
        if (!confirm_prompt("Proceed with installation?", false)) {
            printf("Aborted.\n");
            return 0;
        }
    }

    /* Tool install path: validate up front if we have any tools to install. */
    char *tool_bin_path = NULL;
    if (m.tool_count > 0) {
        tool_bin_path = env_get_tool_bin_path();
        if (!tool_bin_path || tool_bin_path[0] == '\0') {
            log_error("Could not determine tool install path");
            return 1;
        }
        if (!ensure_tool_bin_path(tool_bin_path, auto_yes)) {
            return 1;
        }
    }

    /* Set up install environment for packages. */
    InstallEnv *env = install_env_create();
    if (!env) {
        log_error("Failed to create install environment");
        return 1;
    }
    if (!install_env_init(env)) {
        log_error("Failed to initialize install environment");
        install_env_free(env);
        return 1;
    }

    FailList fl = {0};
    int tools_installed = 0;

    /* Install local-dev packages first so any dependent packages see them in
     * the registry, then regular packages, then tools. */
    install_all_local_devs(&m, env, &fl);
    install_all_packages(&m, env, &fl);
    if (!install_all_tools(&m, tool_bin_path, &fl, &tools_installed)) {
        install_env_free(env);
        return 1;
    }

    install_env_free(env);

    if (fl.count > 0) {
        fflush(stdout);
        fprintf(stderr,
                "\nKit installation completed with %d failure(s):\n",
                fl.count);
        for (int i = 0; i < fl.count; i++) {
            fprintf(stderr, "  - %s\n", fl.items[i]);
        }
        if (tools_installed > 0 && tool_bin_path &&
            !path_env_contains_dir(tool_bin_path)) {
            print_path_warning(tool_bin_path);
        }
        return 1;
    }
    printf("\nSuccessfully installed kit %s %s.\n", m.kit_name, m.kit_version);
    if (tools_installed > 0 && tool_bin_path &&
        !path_env_contains_dir(tool_bin_path)) {
        print_path_warning(tool_bin_path);
    }
    return 0;
}

/* ===========================================================================
 * `wrap kit update --dry-run SRC`
 *
 * Non-destructive verification used by the release pipeline to confirm that a
 * just-published kit is consumable. It accepts a kit URL or a local path,
 * fetches/reads and parses the manifest, reports the tools and packages it
 * would register/download, and writes nothing to disk or the registry.
 * ========================================================================= */

static void print_update_usage(void) {
    printf("Usage: %s kit update <KIT-URL|PATH> [--dry-run] [--yes] [--check-urls]\n",
           global_context_program_name());
    printf("\n");
    printf("Install, reinstall, or update a kit, tracking versions under\n");
    printf("$WRAP_HOME/kits/<KIT>/<VERSION> with a 'current' symlink.\n");
    printf("\n");
    printf("Arguments:\n");
    printf("  KIT-URL|PATH          A kit URL, a .kit file, or a directory containing one\n");
    printf("\n");
    printf("Options:\n");
    printf("  --dry-run             Verify only: parse and report, write nothing\n");
    printf("  -y, --yes             Skip the confirmation prompt\n");
    printf("  --check-urls          With --dry-run, probe each tool URL (HTTP HEAD)\n");
    printf("  -h, --help            Show this help message\n");
}

/* Report what an install would do, without performing any action. */
static void print_dry_run_report(const KitManifest *m, const char *source) {
    printf("Dry run: verifying kit from %s\n", source);
    printf("(no changes will be written to disk or the registry)\n\n");
    printf("  Kit:      %s %s\n", m->kit_name, m->kit_version);
    if (m->compiler_name && m->compiler_version) {
        printf("  Compiler: %s %s\n", m->compiler_name, m->compiler_version);
    }
    printf("\n");

    if (m->local_dev_count > 0) {
        printf("  Would register %d local-dev package(s):\n", m->local_dev_count);
        for (int i = 0; i < m->local_dev_count; i++) {
            printf("    %s/%s %s\n", m->local_devs[i].author,
                   m->local_devs[i].name, m->local_devs[i].version);
        }
        printf("\n");
    }
    if (m->package_count > 0) {
        printf("  Would install %d package(s):\n", m->package_count);
        for (int i = 0; i < m->package_count; i++) {
            const KitPackage *p = &m->packages[i];
            if (p->version) {
                printf("    %s/%s %s\n", p->author, p->name, p->version);
            } else {
                printf("    %s/%s (latest)\n", p->author, p->name);
            }
        }
        printf("\n");
    }
    if (m->tool_count > 0) {
        printf("  Would install %d tool(s):\n", m->tool_count);
        for (int i = 0; i < m->tool_count; i++) {
            const KitTool *t = &m->tools[i];
            if (t->url) {
                printf("    %s  (download: %s)\n", t->name, t->url);
            } else {
                printf("    %s  (local: %s)\n", t->name,
                       t->local_bin_path ? t->local_bin_path : "(local)");
            }
        }
        printf("\n");
    }
    if (m->local_dev_count == 0 && m->package_count == 0 && m->tool_count == 0) {
        printf("  (kit declares no tools or packages)\n\n");
    }
}

static int kit_verify_dry_run(KitManifest *m, const char *source,
                              bool check_urls, CurlSession *session) {
    if (!kit_prepare_local_source_manifest(m)) {
        return 1;
    }

    print_dry_run_report(m, source);

    int malformed = 0;
    int url_tool_count = 0;
    for (int i = 0; i < m->tool_count; i++) {
        if (!m->tools[i].url) continue;
        url_tool_count++;
        if (!url_is_http(m->tools[i].url)) {
            log_error("tool '%s' has a malformed URL", m->tools[i].name);
            malformed++;
        }
    }
    if (malformed > 0) {
        return 1;
    }

    if (check_urls && url_tool_count > 0) {
        bool owns_session = false;
        if (!session) {
            session = curl_session_create();
            if (!session) {
                log_error("Failed to initialize HTTP client");
                return 1;
            }
            owns_session = true;
        }

        printf("Checking tool URLs:\n");
        int unreachable = 0;
        for (int i = 0; i < m->tool_count; i++) {
            const KitTool *t = &m->tools[i];
            if (!t->url) continue;
            printf("  - %s ... ", t->name);
            fflush(stdout);
            HttpResult r = http_head(session, t->url);
            if (r == HTTP_OK) {
                printf("OK\n");
            } else {
                printf("UNREACHABLE (%s)\n", http_result_to_string(r));
                unreachable++;
            }
        }
        printf("\n");

        if (owns_session) {
            curl_session_free(session);
        }
        if (unreachable > 0) {
            log_error("%d tool URL(s) are not reachable", unreachable);
            return 1;
        }
    }

    printf("OK: kit '%s %s' is valid and consumable (dry run, no changes made).\n",
           m->kit_name, m->kit_version);
    return 0;
}

/* Execute a real (non-dry-run) `kit update`: decide install vs reinstall vs
 * update, perform the actions, and persist kit state under $WRAP_HOME/kits.
 * `m` must already have kitfile_dir set for local sources (NULL for URLs). */
static int kit_update_apply(KitManifest *m, const char *body, size_t body_len,
                            bool auto_yes) {
    /* Validate name/version as safe single path components: they come from the
     * kitfile and form $WRAP_HOME/kits/<KIT>/<VERSION>. */
    if (!valid_tool_name(m->kit_name)) {
        log_error("Kit name '%s' is not a valid path component", m->kit_name);
        return 1;
    }
    if (!valid_tool_name(m->kit_version)) {
        log_error("Kit version '%s' is not a valid path component", m->kit_version);
        return 1;
    }

    if (!kit_prepare_local_source_manifest(m)) return 1;

    /* Compute kit storage paths under $WRAP_HOME/kits. */
    char *wrap_home = env_get_wrap_home();
    if (!wrap_home || wrap_home[0] == '\0') {
        log_error("WRAP_HOME is not configured");
        arena_free(wrap_home);
        return 1;
    }
    char kit_dir[PATH_MAX];
    char version_dir[PATH_MAX];
    char stored_name[MAX_PACKAGE_NAME_LENGTH];
    int n1 = snprintf(kit_dir, sizeof(kit_dir), "%s/kits/%s", wrap_home, m->kit_name);
    int n2 = snprintf(version_dir, sizeof(version_dir), "%s/kits/%s/%s",
                      wrap_home, m->kit_name, m->kit_version);
    int n3 = snprintf(stored_name, sizeof(stored_name), "%s.kit", m->kit_name);
    arena_free(wrap_home);
    if (n1 <= 0 || (size_t)n1 >= sizeof(kit_dir) ||
        n2 <= 0 || (size_t)n2 >= sizeof(version_dir) ||
        n3 <= 0 || (size_t)n3 >= sizeof(stored_name)) {
        log_error("Kit path too long for kit '%s' version '%s'",
                  m->kit_name, m->kit_version);
        return 1;
    }
    char current_link[PATH_MAX];
    int n4 = snprintf(current_link, sizeof(current_link), "%s/current", kit_dir);
    if (n4 <= 0 || (size_t)n4 >= sizeof(current_link)) {
        log_error("Kit 'current' path too long");
        return 1;
    }

    /* Read the currently-installed kit (if any) to choose the mode. */
    KitManifest old_m;
    manifest_init(&old_m);
    char *old_body = NULL;
    size_t old_len = 0;
    bool have_current = kit_read_current_manifest(current_link, &old_m,
                                                  &old_body, &old_len);

    enum { MODE_INSTALL, MODE_REINSTALL, MODE_UPDATE } mode = MODE_INSTALL;
    if (have_current) {
        if (strcmp(old_m.kit_version, m->kit_version) == 0) {
            char new_hash[2 * SHA256_BLOCK_SIZE + 1];
            char old_hash[2 * SHA256_BLOCK_SIZE + 1];
            kit_hash_hex(body, body_len, new_hash);
            kit_hash_hex(old_body, old_len, old_hash);
            if (strcmp(new_hash, old_hash) != 0) {
                log_error("This kitfile is invalid as an update source for version %s.",
                          m->kit_version);
                return 1;
            }
            mode = MODE_REINSTALL;
        } else {
            mode = MODE_UPDATE;
        }
    }

    /* Plan + confirm. */
    print_plan(m);
    if (mode == MODE_UPDATE) {
        printf("This will update %s from %s to %s.\n",
               m->kit_name, old_m.kit_version, m->kit_version);
    } else if (mode == MODE_REINSTALL) {
        printf("This will reinstall %s %s.\n", m->kit_name, m->kit_version);
    }
    const char *prompt = (mode == MODE_UPDATE)    ? "Proceed with update?"
                       : (mode == MODE_REINSTALL) ? "Proceed with reinstall?"
                       :                            "Proceed with installation?";
    if (!auto_yes && !confirm_prompt(prompt, false)) {
        printf("Aborted.\n");
        return 0;
    }

    /* Resolve the tool bin path if we will add or remove tool symlinks. */
    char *tool_bin_path = NULL;
    if (m->tool_count > 0 || (mode == MODE_UPDATE && old_m.tool_count > 0)) {
        tool_bin_path = env_get_tool_bin_path();
        if (!tool_bin_path || tool_bin_path[0] == '\0') {
            log_error("Could not determine tool install path");
            return 1;
        }
    }
    if (m->tool_count > 0) {
        if (!ensure_tool_bin_path(tool_bin_path, auto_yes)) {
            return 1;
        }
    }

    /* Install environment for package + local-dev operations. */
    InstallEnv *env = install_env_create();
    if (!env) {
        log_error("Failed to create install environment");
        return 1;
    }
    if (!install_env_init(env)) {
        log_error("Failed to initialize install environment");
        install_env_free(env);
        return 1;
    }

    FailList fl = {0};
    int tools_installed = 0;

    if (mode == MODE_INSTALL || mode == MODE_REINSTALL) {
        install_all_local_devs(m, env, &fl);
        install_all_packages(m, env, &fl);
        if (!install_all_tools(m, tool_bin_path, &fl, &tools_installed)) {
            install_env_free(env);
            return 1;
        }
        if (!kit_store_kitfile(version_dir, stored_name, body, body_len) ||
            !kit_promote_current(kit_dir, version_dir)) {
            install_env_free(env);
            return 1;
        }
    } else { /* MODE_UPDATE */
        /* 1. Download all new tools first; abort untouched on any failure. */
        char **bin_paths = NULL;
        if (m->tool_count > 0) {
            bin_paths = arena_calloc((size_t)m->tool_count, sizeof(char *));
            if (!bin_paths) {
                log_error("Out of memory");
                install_env_free(env);
                return 1;
            }
            printf("\nDownloading tools for %s:\n", m->kit_version);
            if (!kit_download_tools(m, bin_paths)) {
                log_error("Tool download failed; update aborted (no changes made).");
                install_env_free(env);
                return 1;
            }
        }
        /* 2. Store the new kitfile. */
        if (!kit_store_kitfile(version_dir, stored_name, body, body_len)) {
            install_env_free(env);
            return 1;
        }
        /* 3. Tear down the previous version (as listed in its kitfile). */
        printf("\nRemoving previous version %s:\n", old_m.kit_version);
        remove_old_tool_symlinks(&old_m, tool_bin_path);
        deregister_old_local_devs(&old_m, env);
        /* 4. Install the new version's tools, local-dev, and packages. */
        if (m->tool_count > 0) {
            printf("\nLinking tools:\n");
            kit_symlink_tools(m, bin_paths, tool_bin_path, &fl, &tools_installed);
        }
        install_all_local_devs(m, env, &fl);
        install_all_packages(m, env, &fl);
        /* 5. Promote the new version to 'current'. */
        if (!kit_promote_current(kit_dir, version_dir)) {
            install_env_free(env);
            return 1;
        }
    }

    install_env_free(env);

    const char *what = (mode == MODE_UPDATE)    ? "update"
                     : (mode == MODE_REINSTALL) ? "reinstall" : "installation";
    if (fl.count > 0) {
        fflush(stdout);
        fprintf(stderr, "\nKit %s completed with %d failure(s):\n", what, fl.count);
        for (int i = 0; i < fl.count; i++) {
            fprintf(stderr, "  - %s\n", fl.items[i]);
        }
        if (tools_installed > 0 && tool_bin_path &&
            !path_env_contains_dir(tool_bin_path)) {
            print_path_warning(tool_bin_path);
        }
        return 1;
    }

    if (mode == MODE_UPDATE) {
        printf("\nUpdated kit %s from %s to %s.\n",
               m->kit_name, old_m.kit_version, m->kit_version);
    } else if (mode == MODE_REINSTALL) {
        printf("\nReinstalled kit %s %s.\n", m->kit_name, m->kit_version);
    } else {
        printf("\nInstalled kit %s %s.\n", m->kit_name, m->kit_version);
    }
    if (tools_installed > 0 && tool_bin_path &&
        !path_env_contains_dir(tool_bin_path)) {
        print_path_warning(tool_bin_path);
    }
    return 0;
}

int cmd_kit_update(int argc, char *argv[]) {
    /* Line-buffered stdout keeps per-item progress lines ordered with stderr. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    const char *src_arg = NULL;
    bool dry_run = false;
    bool check_urls = false;
    bool auto_yes = false;

    for (int i = 1; i < argc; i++) {
        if (is_help_flag(argv[i])) {
            print_update_usage();
            return 0;
        }
        if (strcmp(argv[i], "--dry-run") == 0) {
            dry_run = true;
        } else if (strcmp(argv[i], "--check-urls") == 0) {
            check_urls = true;
        } else if (strcmp(argv[i], "-y") == 0 || strcmp(argv[i], "--yes") == 0) {
            auto_yes = true;
        } else if (argv[i][0] != '-') {
            if (src_arg) {
                log_error("Unexpected argument '%s'", argv[i]);
                print_update_usage();
                return 1;
            }
            src_arg = argv[i];
        } else {
            log_error("Unknown option: %s", argv[i]);
            print_update_usage();
            return 1;
        }
    }

    if (!src_arg) {
        print_update_usage();
        return 1;
    }

    /* Acquire the kitfile body. For URLs, fetch into memory and bound the size
     * after the transfer (MAX_KIT_FILE_BYTES), consistent with the registry
     * index fetch. For local paths, use the size-bounded file reader. */
    char *body = NULL;
    size_t body_len = 0;
    const char *label = src_arg;
    char *local_kitfile_path = NULL;
    CurlSession *session = NULL;

    if (url_is_http(src_arg)) {
        session = curl_session_create();
        if (!session) {
            log_error("Failed to initialize HTTP client");
            return 1;
        }
        MemoryBuffer *buf = memory_buffer_create();
        if (!buf) {
            log_error("Out of memory");
            curl_session_free(session);
            return 1;
        }
        HttpResult r = http_get_json(session, src_arg, buf);
        if (r != HTTP_OK) {
            log_error("Failed to fetch kit from '%s': %s",
                      src_arg, http_result_to_string(r));
            curl_session_free(session);
            return 1;
        }
        if (buf->len > (size_t)MAX_KIT_FILE_BYTES) {
            log_error("Kit manifest from '%s' exceeds the maximum size of %zu bytes",
                      src_arg, (size_t)MAX_KIT_FILE_BYTES);
            curl_session_free(session);
            return 1;
        }
        body = buf->data;
        body_len = buf->len;
    } else {
        if (path_ends_with(src_arg, ".kit")) {
            if (!path_is_regular_file(src_arg)) {
                log_error("Kitfile not found: %s", src_arg);
                return 1;
            }
            local_kitfile_path = absolute_path_of(src_arg);
        } else if (path_is_directory(src_arg)) {
            char *abs = absolute_path_of(src_arg);
            if (!abs) {
                log_error("Cannot resolve path: %s", src_arg);
                return 1;
            }
            local_kitfile_path = find_kit_in_directory(abs);
        } else {
            log_error("Not a kit URL, a .kit file, or a directory: %s", src_arg);
            return 1;
        }
        if (!local_kitfile_path) {
            return 1; /* find_kit_in_directory logged the reason */
        }
        body = file_read_contents_bounded(local_kitfile_path, MAX_KIT_FILE_BYTES, &body_len);
        if (!body) {
            log_error("Cannot read kitfile: %s", local_kitfile_path);
            return 1;
        }
        label = local_kitfile_path;
    }

    if (!body || body_len == 0) {
        log_error("Kit manifest is empty: %s", label);
        if (session) curl_session_free(session);
        return 1;
    }

    KitManifest m;
    manifest_init(&m);
    if (local_kitfile_path) {
        m.kitfile_path = local_kitfile_path;
        m.kitfile_dir = parent_directory_of(local_kitfile_path);
        if (!m.kitfile_dir) {
            log_error("Cannot determine parent directory of '%s'", local_kitfile_path);
            if (session) curl_session_free(session);
            return 1;
        }
    }

    if (!kitfile_parse(body, body_len, &m, label)) {
        if (session) curl_session_free(session);
        return 1; /* parse error already logged */
    }

    if (dry_run) {
        int rc = kit_verify_dry_run(&m, label, check_urls, session);
        if (session) curl_session_free(session);
        return rc;
    }

    /* ---- Real install / reinstall / update. ---- */
    if (session) {
        /* Tool downloads create their own (longer-timeout) sessions. */
        curl_session_free(session);
    }
    return kit_update_apply(&m, body, body_len, auto_yes);
}
