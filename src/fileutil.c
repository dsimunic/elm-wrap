#include "fileutil.h"
#include "vendor/miniz.h"
#include "alloc.h"
#include "constants.h"
#include "shared/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <ctype.h>

#ifndef PATH_MAX
#define PATH_MAX MAX_PATH_LENGTH
#endif

static bool ensure_directory(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode);
    }

    /* Create parent directory first */
    char *path_copy = arena_strdup(path);
    if (!path_copy) return false;

    char *parent = dirname(path_copy);
    if (strcmp(parent, ".") != 0 && strcmp(parent, "/") != 0) {
        if (!ensure_directory(parent)) {
            arena_free(path_copy);
            return false;
        }
    }
    arena_free(path_copy);

    /* Create this directory */
    if (mkdir(path, DIR_PERMISSIONS) != 0 && errno != EEXIST) {
        return false;
    }

    return true;
}

bool extract_zip(const char *zip_path, const char *dest_dir) {
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));

    if (!mz_zip_reader_init_file(&zip, zip_path, 0)) {
        log_error("Failed to open ZIP file: %s", zip_path);
        return false;
    }

    int num_files = mz_zip_reader_get_num_files(&zip);
    bool success = true;

    for (int i = 0; i < num_files; i++) {
        mz_zip_archive_file_stat file_stat;
        if (!mz_zip_reader_file_stat(&zip, i, &file_stat)) {
            log_error("Failed to get file stat for index %d", i);
            success = false;
            break;
        }

        char output_path[PATH_MAX];
        snprintf(output_path, sizeof(output_path), "%s/%s", dest_dir, file_stat.m_filename);

        if (mz_zip_reader_is_file_a_directory(&zip, i)) {
            if (!ensure_directory(output_path)) {
                log_error("Failed to create directory: %s", output_path);
                success = false;
                break;
            }
        } else {
            char *output_copy = arena_strdup(output_path);
            if (!output_copy) {
                success = false;
                break;
            }
            char *parent = dirname(output_copy);
            if (!ensure_directory(parent)) {
                log_error("Failed to create parent directory: %s", parent);
                arena_free(output_copy);
                success = false;
                break;
            }
            arena_free(output_copy);

            if (!mz_zip_reader_extract_to_file(&zip, i, output_path, 0)) {
                log_error("Failed to extract file: %s", file_stat.m_filename);
                success = false;
                break;
            }
        }
    }

    mz_zip_reader_end(&zip);
    return success;
}

static bool should_extract_path(const char *filename) {
    if (!filename) return false;

    /* Skip leading directory components (e.g., "author-package-hash/") */
    const char *base = strchr(filename, '/');
    if (!base) {
        base = filename;
    } else {
        base++; /* Skip the slash */
    }

    if (strcmp(base, "elm.json") == 0) return true;
    if (strcmp(base, "docs.json") == 0) return true;
    if (strcmp(base, "LICENSE") == 0) return true;
    if (strcmp(base, "README.md") == 0) return true;

    if (strncmp(base, "src/", 4) == 0) return true;
    if (strcmp(base, "src") == 0) return true;

    return false;
}

bool extract_zip_selective(const char *zip_path, const char *dest_dir) {
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));

    if (!mz_zip_reader_init_file(&zip, zip_path, 0)) {
        log_error("Failed to open ZIP file: %s", zip_path);
        return false;
    }

    int num_files = mz_zip_reader_get_num_files(&zip);
    bool success = true;

    for (int i = 0; i < num_files; i++) {
        mz_zip_archive_file_stat file_stat;
        if (!mz_zip_reader_file_stat(&zip, i, &file_stat)) {
            log_error("Failed to get file stat for index %d", i);
            success = false;
            break;
        }

        if (!should_extract_path(file_stat.m_filename)) {
            continue;
        }

        /* Find the base path (skip leading directory component if present) */
        const char *base = strchr(file_stat.m_filename, '/');
        const char *rel_path = base ? base + 1 : file_stat.m_filename;

        char output_path[PATH_MAX];
        snprintf(output_path, sizeof(output_path), "%s/%s", dest_dir, rel_path);

        if (mz_zip_reader_is_file_a_directory(&zip, i)) {
            if (!ensure_directory(output_path)) {
                log_error("Failed to create directory: %s", output_path);
                success = false;
                break;
            }
        } else {
            if (strcmp(rel_path, "elm.json") == 0 || strcmp(rel_path, "docs.json") == 0) {
                struct stat st;
                if (stat(output_path, &st) == 0) {
                    log_debug("Skipping %s (already exists)", rel_path);
                    continue;
                }
            }

            char *output_copy = arena_strdup(output_path);
            if (!output_copy) {
                success = false;
                break;
            }
            char *parent = dirname(output_copy);
            if (!ensure_directory(parent)) {
                log_error("Failed to create parent directory: %s", parent);
                arena_free(output_copy);
                success = false;
                break;
            }
            arena_free(output_copy);

            if (!mz_zip_reader_extract_to_file(&zip, i, output_path, 0)) {
                log_error("Failed to extract file: %s", file_stat.m_filename);
                success = false;
                break;
            }
        }
    }

    mz_zip_reader_end(&zip);
    return success;
}

char *find_elm_json_upwards(const char *start_path) {
    char cwd[MAX_PATH_LENGTH];
    const char *initial = start_path;

    if (!initial) {
        if (!getcwd(cwd, sizeof(cwd))) {
            return NULL;
        }
        initial = cwd;
    }

    char *path = arena_strdup(initial);
    if (!path) {
        return NULL;
    }

    /* If start_path is a file, start from its parent directory */
    struct stat st;
    if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
        char *slash = strrchr(path, '/');
        if (slash) {
            if (slash == path) {
                /* File in root dir */
                path[1] = '\0';
            } else {
                *slash = '\0';
            }
        } else {
            /* Relative filename with no slash */
            arena_free(path);
            path = arena_strdup(".");
            if (!path) {
                return NULL;
            }
        }
    }

    char *stripped = strip_trailing_slash(path);
    if (stripped) {
        arena_free(path);
        path = stripped;
    }

    while (true) {
        const bool is_root = (strcmp(path, "/") == 0);
        size_t buf_len = strlen(path) + (is_root ? sizeof("elm.json") : sizeof("/elm.json"));

        char *candidate = arena_malloc(buf_len);
        if (!candidate) {
            arena_free(path);
            return NULL;
        }

        if (is_root) {
            snprintf(candidate, buf_len, "/elm.json");
        } else {
            snprintf(candidate, buf_len, "%s/elm.json", path);
        }

        if (file_exists(candidate)) {
            arena_free(path);
            return candidate;
        }
        arena_free(candidate);

        /* Go up one directory */
        char *slash = strrchr(path, '/');
        if (!slash) {
            break;
        }
        if (slash == path) {
            /* Reached root */
            break;
        }
        *slash = '\0';

        /* Strip any trailing slashes introduced by truncation */
        while (path[0] != '\0') {
            size_t len = strlen(path);
            if (len <= 1 || path[len - 1] != '/') {
                break;
            }
            path[len - 1] = '\0';
        }
    }

    arena_free(path);
    return NULL;
}

char* find_first_subdirectory(const char *dir_path) {
    DIR *dir = opendir(dir_path);
    if (!dir) {
        return NULL;
    }

    struct dirent *entry;
    char *result = NULL;

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char full_path[PATH_MAX];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);

        struct stat st;
        if (stat(full_path, &st) == 0 && S_ISDIR(st.st_mode)) {
            result = arena_strdup(full_path);
            break;
        }
    }

    closedir(dir);
    return result;
}

static bool move_item(const char *src, const char *dest) {
    /* Try rename first (fast, atomic) */
    if (rename(src, dest) == 0) {
        return true;
    }

    /* If rename failed (possibly cross-device), fall back to copy+delete */
    struct stat st;
    if (stat(src, &st) != 0) {
        return false;
    }

    if (S_ISDIR(st.st_mode)) {
        /* For directories, use recursive copy then delete */
        if (!copy_directory_recursive(src, dest)) {
            return false;
        }
        return remove_directory_recursive(src);
    } else {
        /* For files, copy then delete */
        FILE *src_file = fopen(src, "rb");
        if (!src_file) return false;

        FILE *dest_file = fopen(dest, "wb");
        if (!dest_file) {
            fclose(src_file);
            return false;
        }

        char buffer[8192];
        size_t n;
        bool success = true;
        while ((n = fread(buffer, 1, sizeof(buffer), src_file)) > 0) {
            if (fwrite(buffer, 1, n, dest_file) != n) {
                success = false;
                break;
            }
        }

        fclose(src_file);
        fclose(dest_file);

        if (success) {
            unlink(src);
        }
        return success;
    }
}

bool move_directory_contents(const char *src_dir, const char *dest_dir) {
    DIR *dir = opendir(src_dir);
    if (!dir) {
        log_error("Failed to open directory: %s", src_dir);
        return false;
    }

    struct dirent *entry;
    bool success = true;

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char src_path[PATH_MAX];
        char dest_path[PATH_MAX];
        snprintf(src_path, sizeof(src_path), "%s/%s", src_dir, entry->d_name);
        snprintf(dest_path, sizeof(dest_path), "%s/%s", dest_dir, entry->d_name);

        if (!move_item(src_path, dest_path)) {
            log_warn("Failed to move %s to %s", src_path, dest_path);
            /* Continue with other files even if one fails */
        }
    }

    closedir(dir);
    return success;
}

bool remove_directory_recursive(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        /* Path doesn't exist, consider it success */
        return true;
    }

    if (!S_ISDIR(st.st_mode)) {
        /* It's a file, just delete it */
        return unlink(path) == 0;
    }

    /* It's a directory, delete contents first */
    DIR *dir = opendir(path);
    if (!dir) {
        return false;
    }

    struct dirent *entry;
    bool success = true;

    while ((entry = readdir(dir)) != NULL) {
        /* Skip . and .. */
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char entry_path[MAX_PATH_LENGTH];
        snprintf(entry_path, sizeof(entry_path), "%s/%s", path, entry->d_name);

        if (!remove_directory_recursive(entry_path)) {
            success = false;
            /* Continue trying to delete other entries */
        }
    }

    closedir(dir);

    /* Remove the now-empty directory */
    if (success) {
        success = (rmdir(path) == 0);
    }

    return success;
}

bool copy_directory_recursive(const char *src_path, const char *dest_path) {
    struct stat st;
    if (stat(src_path, &st) != 0) {
        return false;
    }

    if (!S_ISDIR(st.st_mode)) {
        FILE *src_file = fopen(src_path, "rb");
        if (!src_file) return false;

        FILE *dest_file = fopen(dest_path, "wb");
        if (!dest_file) {
            fclose(src_file);
            return false;
        }

        char buffer[8192];
        size_t n;
        bool success = true;
        while ((n = fread(buffer, 1, sizeof(buffer), src_file)) > 0) {
            if (fwrite(buffer, 1, n, dest_file) != n) {
                success = false;
                break;
            }
        }

        fclose(src_file);
        fclose(dest_file);

        /* Preserve permissions */
        chmod(dest_path, st.st_mode);

        return success;
    }

    if (!ensure_directory(dest_path)) {
        return false;
    }

    DIR *dir = opendir(src_path);
    if (!dir) {
        return false;
    }

    struct dirent *entry;
    bool success = true;

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char src_entry[PATH_MAX];
        char dest_entry[PATH_MAX];
        snprintf(src_entry, sizeof(src_entry), "%s/%s", src_path, entry->d_name);
        snprintf(dest_entry, sizeof(dest_entry), "%s/%s", dest_path, entry->d_name);

        if (!copy_directory_recursive(src_entry, dest_entry)) {
            success = false;
            /* Continue trying to copy other entries */
        }
    }

    closedir(dir);

    /* Preserve directory permissions */
    chmod(dest_path, st.st_mode);

    return success;
}

bool copy_directory_selective(const char *src_path, const char *dest_path) {
    struct stat st;
    if (stat(src_path, &st) != 0) {
        return false;
    }

    if (!S_ISDIR(st.st_mode)) {
        log_error("Source path must be a directory: %s", src_path);
        return false;
    }

    if (!ensure_directory(dest_path)) {
        return false;
    }

    /* List of files to copy from the root directory */
    const char *files_to_copy[] = {"elm.json", "docs.json", "LICENSE", "README.md", NULL};
    bool success = true;

    /* Copy individual files */
    for (int i = 0; files_to_copy[i] != NULL; i++) {
        char src_file[MAX_PATH_LENGTH];
        char dest_file[MAX_PATH_LENGTH];
        snprintf(src_file, sizeof(src_file), "%s/%s", src_path, files_to_copy[i]);
        snprintf(dest_file, sizeof(dest_file), "%s/%s", dest_path, files_to_copy[i]);

        /* Check if source file exists */
        if (stat(src_file, &st) == 0 && !S_ISDIR(st.st_mode)) {
            /* Copy the file */
            FILE *src_fp = fopen(src_file, "rb");
            if (!src_fp) {
                /* File exists but can't be opened - treat as error */
                log_error("Failed to open %s for reading", src_file);
                success = false;
                continue;
            }

            FILE *dest_fp = fopen(dest_file, "wb");
            if (!dest_fp) {
                fclose(src_fp);
                log_error("Failed to open %s for writing", dest_file);
                success = false;
                continue;
            }

            char buffer[8192];
            size_t n;
            bool copy_success = true;
            while ((n = fread(buffer, 1, sizeof(buffer), src_fp)) > 0) {
                if (fwrite(buffer, 1, n, dest_fp) != n) {
                    copy_success = false;
                    break;
                }
            }

            fclose(src_fp);
            fclose(dest_fp);

            if (!copy_success) {
                log_error("Failed to copy %s", files_to_copy[i]);
                success = false;
            } else {
                /* Preserve permissions */
                chmod(dest_file, st.st_mode);
            }
        }
        /* If file doesn't exist, that's okay - not all files are required */
    }

    /* Copy src/ directory recursively */
    char src_dir[MAX_PATH_LENGTH];
    char dest_dir[MAX_PATH_LENGTH];
    snprintf(src_dir, sizeof(src_dir), "%s/src", src_path);
    snprintf(dest_dir, sizeof(dest_dir), "%s/src", dest_path);

    if (stat(src_dir, &st) == 0 && S_ISDIR(st.st_mode)) {
        if (!copy_directory_recursive(src_dir, dest_dir)) {
            log_error("Failed to copy src/ directory");
            success = false;
        }
    } else {
        log_error("src/ directory not found in %s", src_path);
        success = false;
    }

    return success;
}

bool file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

char *file_read_contents_bounded(const char *filepath, size_t max_bytes, size_t *out_size) {
    if (!filepath || max_bytes == 0) {
        return NULL;
    }

    struct stat st;
    if (stat(filepath, &st) != 0) {
        return NULL;
    }
    if (!S_ISREG(st.st_mode)) {
        return NULL;
    }
    if (st.st_size < 0) {
        return NULL;
    }

    size_t fsize = (size_t)st.st_size;
    if (fsize > max_bytes) {
        return NULL;
    }

    FILE *f = fopen(filepath, "rb");
    if (!f) {
        return NULL;
    }

    char *content = arena_malloc(fsize + 1);
    if (!content) {
        fclose(f);
        return NULL;
    }

    size_t read_size = 0;
    if (fsize > 0) {
        read_size = fread(content, 1, fsize, f);
        if (read_size != fsize && ferror(f)) {
            fclose(f);
            arena_free(content);
            return NULL;
        }
    }
    content[read_size] = '\0';

    fclose(f);

    if (out_size) {
        *out_size = read_size;
    }

    return content;
}

char *file_read_contents(const char *filepath) {
    return file_read_contents_bounded(filepath, MAX_FILE_READ_CONTENTS_BYTES, NULL);
}

char *strip_trailing_slash(const char *path) {
    if (!path) return NULL;
    
    int len = strlen(path);
    while (len > 1 && path[len - 1] == '/') {
        len--;
    }
    
    char *result = arena_malloc((size_t)len + 1);
    if (!result) {
        return NULL;
    }
    strncpy(result, path, (size_t)len);
    result[len] = '\0';
    
    return result;
}
