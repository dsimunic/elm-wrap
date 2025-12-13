/*
 * help-report-html-gen.c
 * 
 * Generates a static HTML viewer for wrap help reports with embedded diffs.
 * 
 * Usage: help-report-html-gen <data-dir> <wrap-bin> <output-dir>
 * 
 * Reads all .txt files in data-dir, compares with .backup files if present,
 * gets version from wrap-bin, and generates an HTML file with embedded JSON
 * data showing all reports and their diffs. Output goes to output-dir/VERSION/index.html
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

/* Override arena allocator with standard malloc/free for this tool */
#define arena_malloc malloc
#define arena_calloc calloc
#define arena_realloc realloc
#define arena_free free
#define arena_strdup strdup

/* Use cJSON with standard malloc/free */
#define CJSON_NESTING_LIMIT 1000
#include "../src/vendor/cJSON.c"

#define MAX_PATH_LENGTH 4096
#define MAX_FILE_SIZE (1024 * 1024)  // 1MB max per file

/* Read entire file into a string */
static char* read_file(const char *filepath) {
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        return NULL;
    }
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (size > MAX_FILE_SIZE) {
        fclose(f);
        return NULL;
    }
    
    char *content = malloc(size + 1);
    if (!content) {
        fclose(f);
        return NULL;
    }
    
    size_t read_size = fread(content, 1, size, f);
    content[read_size] = '\0';
    fclose(f);
    
    return content;
}

/* Generate unified diff between two strings */
static char* generate_diff(const char *old_content, const char *new_content) {
    /* For simplicity, we'll use a basic line-by-line comparison */
    /* In production, you might want to use a proper diff library */
    
    if (!old_content || !new_content) {
        return strdup("");
    }
    
    /* If identical, return empty string */
    if (strcmp(old_content, new_content) == 0) {
        return strdup("");
    }
    
    /* Simple line-by-line diff */
    char *result = malloc(MAX_FILE_SIZE);
    if (!result) return strdup("");
    
    result[0] = '\0';
    size_t result_len = 0;
    
    /* Split into lines and compare */
    char *old_copy = strdup(old_content);
    char *new_copy = strdup(new_content);
    
    char *old_line = strtok(old_copy, "\n");
    char *new_line = strtok(new_copy, "\n");
    
    while (old_line || new_line) {
        if (!old_line && new_line) {
            /* Added line */
            result_len += snprintf(result + result_len, MAX_FILE_SIZE - result_len, "+%s\n", new_line);
            new_line = strtok(NULL, "\n");
        } else if (old_line && !new_line) {
            /* Deleted line */
            result_len += snprintf(result + result_len, MAX_FILE_SIZE - result_len, "-%s\n", old_line);
            old_line = strtok(NULL, "\n");
        } else if (strcmp(old_line, new_line) != 0) {
            /* Changed line */
            result_len += snprintf(result + result_len, MAX_FILE_SIZE - result_len, "-%s\n", old_line);
            result_len += snprintf(result + result_len, MAX_FILE_SIZE - result_len, "+%s\n", new_line);
            old_line = strtok(NULL, "\n");
            new_line = strtok(NULL, "\n");
        } else {
            /* Identical line - skip in diff view */
            old_line = strtok(NULL, "\n");
            new_line = strtok(NULL, "\n");
        }
    }
    
    free(old_copy);
    free(new_copy);
    
    return result;
}

/* HTML escape a string */
static char* html_escape(const char *str) {
    if (!str) return strdup("");
    
    size_t len = strlen(str);
    char *escaped = malloc(len * 6 + 1);  // Worst case: every char becomes &xxxx;
    if (!escaped) return strdup("");
    
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        switch (str[i]) {
            case '&':
                strcpy(&escaped[j], "&amp;");
                j += 5;
                break;
            case '<':
                strcpy(&escaped[j], "&lt;");
                j += 4;
                break;
            case '>':
                strcpy(&escaped[j], "&gt;");
                j += 4;
                break;
            case '"':
                strcpy(&escaped[j], "&quot;");
                j += 6;
                break;
            default:
                escaped[j++] = str[i];
                break;
        }
    }
    escaped[j] = '\0';
    
    return escaped;
}

/* Format diff with HTML spans for coloring */
static char* format_diff_html(const char *diff_text) {
    if (!diff_text || !diff_text[0]) {
        return strdup("");
    }
    
    char *escaped = html_escape(diff_text);
    size_t len = strlen(escaped);
    char *result = malloc(len * 3 + 1024);  // Room for span tags
    if (!result) {
        free(escaped);
        return strdup("");
    }
    
    size_t j = 0;
    char *line = strtok(escaped, "\n");
    
    while (line) {
        if (line[0] == '+') {
            j += snprintf(result + j, len * 3 - j, "<span class=\"diff-add\">%s</span>\n", line);
        } else if (line[0] == '-') {
            j += snprintf(result + j, len * 3 - j, "<span class=\"diff-del\">%s</span>\n", line);
        } else if (line[0] == '@') {
            j += snprintf(result + j, len * 3 - j, "<span class=\"diff-hunk\">%s</span>\n", line);
        } else {
            j += snprintf(result + j, len * 3 - j, "%s\n", line);
        }
        line = strtok(NULL, "\n");
    }
    
    free(escaped);
    return result;
}

/* Get version from wrap binary */
static char* get_wrap_version(const char *wrap_bin) {
    char cmd[MAX_PATH_LENGTH + 10];
    snprintf(cmd, sizeof(cmd), "%s -V", wrap_bin);
    
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        fprintf(stderr, "Warning: Could not get wrap version\n");
        return strdup("unknown");
    }
    
    char version[256];
    if (fgets(version, sizeof(version), fp) == NULL) {
        pclose(fp);
        return strdup("unknown");
    }
    
    pclose(fp);
    
    /* Remove trailing newline */
    size_t len = strlen(version);
    if (len > 0 && version[len - 1] == '\n') {
        version[len - 1] = '\0';
    }
    
    return strdup(version);
}

/* Create directory recursively */
static int mkdir_recursive(const char *path) {
    char tmp[MAX_PATH_LENGTH];
    char *p = NULL;
    size_t len;
    
    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (tmp[len - 1] == '/') {
        tmp[len - 1] = 0;
    }
    
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
    
    return 0;
}

/* Compare function for qsort */
static int compare_strings(const void *a, const void *b) {
    return strcmp(*(const char **)a, *(const char **)b);
}

/* Extract usage line from help text (handles multi-line Usage sections) */
static char* extract_usage(const char *content) {
    if (!content) return strdup("");
    
    const char *usage_header = strstr(content, "Usage:");
    if (!usage_header) return strdup("");
    
    /* Check if "Usage:" is alone on its line (multi-line format) */
    const char *after_header = usage_header + 6;  /* Skip "Usage:" */
    
    /* Skip whitespace */
    while (*after_header == ' ' || *after_header == '\t') {
        after_header++;
    }
    
    /* If newline immediately follows "Usage:", it's multi-line format */
    if (*after_header == '\n') {
        /* Multi-line format - collect all indented lines until next section */
        char *result = malloc(MAX_FILE_SIZE);
        if (!result) return strdup("");
        
        result[0] = '\0';
        size_t result_len = 0;
        
        const char *line_start = after_header + 1;  /* Skip the newline */
        
        while (*line_start) {
            /* Check if line starts with whitespace (indented usage line) */
            if (*line_start != ' ' && *line_start != '\t') {
                /* Non-indented line means end of Usage section */
                break;
            }
            
            /* Skip leading whitespace */
            while (*line_start == ' ' || *line_start == '\t') {
                line_start++;
            }
            
            /* Find end of this line */
            const char *line_end = strchr(line_start, '\n');
            if (!line_end) {
                line_end = line_start + strlen(line_start);
            }
            
            /* Add this usage line to result with separator if not first */
            if (result_len > 0) {
                result_len += snprintf(result + result_len, MAX_FILE_SIZE - result_len, " OR ");
            }
            
            size_t line_len = line_end - line_start;
            if (result_len + line_len + 1 < MAX_FILE_SIZE) {
                memcpy(result + result_len, line_start, line_len);
                result_len += line_len;
                result[result_len] = '\0';
            }
            
            if (*line_end == '\0') break;
            line_start = line_end + 1;
        }
        
        return result;
    } else {
        /* Single-line format: "Usage: command args" */
        const char *usage_start = after_header;
        
        /* Find end of line */
        const char *line_end = strchr(usage_start, '\n');
        if (!line_end) {
            return strdup(usage_start);
        }
        
        size_t len = line_end - usage_start;
        char *usage = malloc(len + 1);
        if (!usage) return strdup("");
        
        memcpy(usage, usage_start, len);
        usage[len] = '\0';
        
        return usage;
    }
}

/* Generate fragment ID from filename (remove .txt extension) */
static char* filename_to_fragment(const char *filename) {
    size_t len = strlen(filename);
    if (len < 5 || strcmp(filename + len - 4, ".txt") != 0) {
        return strdup(filename);
    }
    
    char *fragment = malloc(len - 3);
    if (!fragment) return strdup("");
    
    memcpy(fragment, filename, len - 4);
    fragment[len - 4] = '\0';
    
    return fragment;
}

/* Generate JSON data for all files */
static cJSON* generate_data_json(const char *data_dir) {
    cJSON *root = cJSON_CreateObject();
    cJSON *files_array = cJSON_CreateArray();
    cJSON *changed_array = cJSON_CreateArray();
    
    /* Read directory */
    DIR *dir = opendir(data_dir);
    if (!dir) {
        fprintf(stderr, "Error: Cannot open directory: %s\n", data_dir);
        cJSON_Delete(root);
        return NULL;
    }
    
    /* Collect all .txt files (not .backup) */
    char **filenames = NULL;
    int file_count = 0;
    int file_capacity = 64;
    filenames = malloc(file_capacity * sizeof(char*));
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        /* Skip . and .. */
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        size_t name_len = strlen(entry->d_name);
        if (name_len < 5 || strcmp(entry->d_name + name_len - 4, ".txt") != 0) {
            continue;
        }
        
        /* Skip .backup files */
        if (name_len > 11 && strcmp(entry->d_name + name_len - 11, ".txt.backup") == 0) {
            continue;
        }
        
        /* Check if it's a regular file using stat */
        char filepath_check[MAX_PATH_LENGTH];
        snprintf(filepath_check, sizeof(filepath_check), "%s/%s", data_dir, entry->d_name);
        struct stat file_stat;
        if (stat(filepath_check, &file_stat) != 0 || !S_ISREG(file_stat.st_mode)) {
            continue;
        }
        
        if (file_count >= file_capacity) {
            file_capacity *= 2;
            filenames = realloc(filenames, file_capacity * sizeof(char*));
        }
        
        filenames[file_count++] = strdup(entry->d_name);
    }
    closedir(dir);
    
    /* Sort filenames */
    qsort(filenames, file_count, sizeof(char*), compare_strings);
    
    /* Process each file */
    for (int i = 0; i < file_count; i++) {
        char filepath[MAX_PATH_LENGTH];
        char backup_path[MAX_PATH_LENGTH];
        
        snprintf(filepath, sizeof(filepath), "%s/%s", data_dir, filenames[i]);
        snprintf(backup_path, sizeof(backup_path), "%s.backup", filepath);
        
        /* Read current file */
        char *content = read_file(filepath);
        if (!content) continue;
        
        /* Check for backup */
        char *backup_content = read_file(backup_path);
        
        const char *status = "unchanged";
        char *diff_text = NULL;
        
        if (backup_content) {
            if (strcmp(content, backup_content) != 0) {
                status = "changed";
                diff_text = generate_diff(backup_content, content);
                cJSON_AddItemToArray(changed_array, cJSON_CreateString(filenames[i]));
            }
            free(backup_content);
        } else {
            status = "new";
            cJSON_AddItemToArray(changed_array, cJSON_CreateString(filenames[i]));
        }
        
        /* Extract usage and fragment */
        char *usage = extract_usage(content);
        char *fragment = filename_to_fragment(filenames[i]);
        
        /* Create file object */
        cJSON *file_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(file_obj, "filename", filenames[i]);
        cJSON_AddStringToObject(file_obj, "fragment", fragment);
        cJSON_AddStringToObject(file_obj, "usage", usage);
        cJSON_AddStringToObject(file_obj, "status", status);
        cJSON_AddStringToObject(file_obj, "content", content);
        
        if (diff_text) {
            char *formatted_diff = format_diff_html(diff_text);
            cJSON_AddStringToObject(file_obj, "diff", formatted_diff);
            free(formatted_diff);
            free(diff_text);
        } else {
            cJSON_AddStringToObject(file_obj, "diff", "");
        }
        
        cJSON_AddItemToArray(files_array, file_obj);
        
        free(usage);
        free(fragment);
        
        free(content);
        free(filenames[i]);
    }
    
    free(filenames);
    
    cJSON_AddItemToObject(root, "files", files_array);
    cJSON_AddItemToObject(root, "changed", changed_array);
    
    return root;
}

/* Generate the complete HTML file */
static int generate_html(const char *data_dir, const char *wrap_bin, const char *output_dir) {
    /* Get wrap version */
    char *version = get_wrap_version(wrap_bin);
    
    /* Create version-specific directory */
    char version_dir[MAX_PATH_LENGTH];
    snprintf(version_dir, sizeof(version_dir), "%s/%s", output_dir, version);
    mkdir_recursive(version_dir);
    
    /* Generate JSON data */
    cJSON *data = generate_data_json(data_dir);
    if (!data) {
        free(version);
        return 1;
    }
    
    char *json_str = cJSON_PrintUnformatted(data);
    cJSON_Delete(data);
    
    if (!json_str) {
        fprintf(stderr, "Error: Failed to generate JSON\n");
        free(version);
        return 1;
    }
    
    /* Create output file path */
    char output_path[MAX_PATH_LENGTH];
    snprintf(output_path, sizeof(output_path), "%s/index.html", version_dir);
    
    /* Open output file */
    FILE *out = fopen(output_path, "w");
    if (!out) {
        fprintf(stderr, "Error: Cannot create output file: %s\n", output_path);
        free(json_str);
        free(version);
        return 1;
    }
    
    printf("Writing HTML to: %s\n", output_path);
    
    /* Write HTML with embedded JSON */
    fprintf(out, "%s", 
        "<!DOCTYPE html>\n"
        "<html lang=\"en\">\n"
        "<head>\n"
        "    <meta charset=\"UTF-8\">\n"
        "    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"
        "    <title>wrap Help Reports</title>\n"
        "    <style>\n"
        "        * {\n"
        "            margin: 0;\n"
        "            padding: 0;\n"
        "            box-sizing: border-box;\n"
        "        }\n"
        "        \n"
        "        body {\n"
        "            font-family: 'SF Mono', 'Monaco', 'Consolas', 'Liberation Mono', 'Courier New', monospace;\n"
        "            font-size: 13px;\n"
        "            line-height: 1.5;\n"
        "            color: #e0e0e0;\n"
        "            background: #1e1e1e;\n"
        "            display: flex;\n"
        "            height: 100vh;\n"
        "            overflow: hidden;\n"
        "        }\n"
        "        \n"
        "        .sidebar {\n"
        "            width: 25vw;\n"
        "            min-width: 320px;\n"
        "            background: #252526;\n"
        "            border-right: 1px solid #3e3e42;\n"
        "            overflow-y: auto;\n"
        "            flex-shrink: 0;\n"
        "        }\n"
        "        \n"
        "        .sidebar-header {\n"
        "            padding: 16px 16px 0;\n"
        "            background: #2d2d30;\n"
        "            border-bottom: 1px solid #3e3e42;\n"
        "            font-weight: 600;\n"
        "            color: #cccccc;\n"
        "            min-height: 4rem;\n"
        "        }\n"
        "        \n"
        "        .file-list {\n"
        "            list-style: none;\n"
        "        }\n"
        "        \n"
        "        .file-item {\n"
        "            padding: 8px 16px;\n"
        "            cursor: pointer;\n"
        "            border-bottom: 1px solid #2d2d30;\n"
        "            transition: background 0.1s;\n"
        "            display: flex;\n"
        "            align-items: center;\n"
        "            gap: 8px;\n"
        "        }\n"
        "        \n"
        "        .file-item:hover {\n"
        "            background: #2d2d30;\n"
        "        }\n"
        "        \n"
        "        .file-item.active {\n"
        "            background: #094771;\n"
        "            border-left: 2px solid #007acc;\n"
        "        }\n"
        "        \n"
        "        .file-item.changed {\n"
        "            color: #f48771;\n"
        "        }\n"
        "        \n"
        "        .file-item.new {\n"
        "            color: #89d185;\n"
        "        }\n"
        "        \n"
        "        .status-badge {\n"
        "            font-size: 10px;\n"
        "            padding: 2px 6px;\n"
        "            border-radius: 3px;\n"
        "            font-weight: 600;\n"
        "            text-transform: uppercase;\n"
        "        }\n"
        "        \n"
        "        .status-badge.changed {\n"
        "            background: #5a1d1d;\n"
        "            color: #f48771;\n"
        "        }\n"
        "        \n"
        "        .status-badge.new {\n"
        "            background: #1d3b1d;\n"
        "            color: #89d185;\n"
        "        }\n"
        "        \n"
        "        .content-area {\n"
        "            flex: 1;\n"
        "            display: flex;\n"
        "            flex-direction: column;\n"
        "            overflow: hidden;\n"
        "        }\n"
        "        \n"
        "        .content-header {\n"
        "            padding: 16px;\n"
        "            background: #2d2d30;\n"
        "            border-bottom: 1px solid #3e3e42;\n"
        "            display: flex;\n"
        "            justify-content: space-between;\n"
        "            align-items: center;\n"
        "            min-height: 4rem;\n"
        "        }\n"
        "        \n"
        "        .content-title {\n"
        "            font-weight: 600;\n"
        "            color: #cccccc;\n"
        "            font-size: 14px;\n"
        "        }\n"
        "        \n"
        "        .view-toggle {\n"
        "            display: flex;\n"
        "            gap: 8px;\n"
        "        }\n"
        "        \n"
        "        .view-button {\n"
        "            padding: 4px 12px;\n"
        "            background: #3e3e42;\n"
        "            border: 1px solid #555;\n"
        "            color: #cccccc;\n"
        "            cursor: pointer;\n"
        "            border-radius: 3px;\n"
        "            font-size: 11px;\n"
        "            transition: all 0.1s;\n"
        "        }\n"
        "        \n"
        "        .view-button:hover {\n"
        "            background: #505050;\n"
        "        }\n"
        "        \n"
        "        .view-button.active {\n"
        "            background: #094771;\n"
        "            border-color: #007acc;\n"
        "            color: #fff;\n"
        "        }\n"
        "        \n"
        "        .content-body {\n"
        "            flex: 1;\n"
        "            overflow-y: auto;\n"
        "            padding: 20px;\n"
        "            background: #1e1e1e;\n"
        "        }\n"
        "        \n"
        "        .help-content {\n"
        "            white-space: pre-wrap;\n"
        "            font-family: inherit;\n"
        "            color: #d4d4d4;\n"
        "        }\n"
        "        \n"
        "        .diff-content {\n"
        "            font-family: inherit;\n"
        "            color: #d4d4d4;\n"
        "            white-space: pre-wrap;\n"
        "        }\n"
        "        \n"
        "        .diff-add {\n"
        "            color: #89d185;\n"
        "            background: #1d3b1d;\n"
        "            display: block;\n"
        "            padding: 0 4px;\n"
        "        }\n"
        "        \n"
        "        .diff-del {\n"
        "            color: #f48771;\n"
        "            background: #5a1d1d;\n"
        "            display: block;\n"
        "            padding: 0 4px;\n"
        "        }\n"
        "        \n"
        "        .diff-hunk {\n"
        "            color: #569cd6;\n"
        "            display: block;\n"
        "            padding: 8px 4px 4px;\n"
        "            font-weight: 600;\n"
        "        }\n"
        "        \n"
        "        .no-diff {\n"
        "            color: #89d185;\n"
        "            font-weight: 600;\n"
        "        }\n"
        "        \n"
        "        .empty-state {\n"
        "            display: flex;\n"
        "            align-items: center;\n"
        "            justify-content: center;\n"
        "            height: 100%;\n"
        "            color: #6a6a6a;\n"
        "            font-size: 14px;\n"
        "        }\n"
        "        \n"
        "        ::-webkit-scrollbar {\n"
        "            width: 10px;\n"
        "            height: 10px;\n"
        "        }\n"
        "        \n"
        "        ::-webkit-scrollbar-track {\n"
        "            background: #1e1e1e;\n"
        "        }\n"
        "        \n"
        "        ::-webkit-scrollbar-thumb {\n"
        "            background: #424242;\n"
        "            border-radius: 5px;\n"
        "        }\n"
        "        \n"
        "        ::-webkit-scrollbar-thumb:hover {\n"
        "            background: #4e4e4e;\n"
        "        }\n"
        "    </style>\n"
        "</head>\n"
        "<body>\n"
        "    <div class=\"sidebar\">\n"
        "        <div class=\"sidebar-header\">\n"
        "            <div>Help Reports</div>\n"
        "            <small style=\"font-size: 10px; font-weight: normal; color: #888; margin-top: 4px;\">version ");
    
    fprintf(out, "%s", version);
    
    fprintf(out, "%s",
        "</small>\n"
        "        </div>\n"
        "        <ul class=\"file-list\" id=\"fileList\"></ul>\n"
        "    </div>\n"
        "    \n"
        "    <div class=\"content-area\">\n"
        "        <div class=\"content-header\">\n"
        "            <div class=\"content-title\" id=\"contentTitle\">Select a help report</div>\n"
        "            <div class=\"view-toggle\" id=\"viewToggle\" style=\"display: none;\">\n"
        "                <button class=\"view-button active\" data-view=\"content\">Help Text</button>\n"
        "                <button class=\"view-button\" data-view=\"diff\">Diff</button>\n"
        "            </div>\n"
        "        </div>\n"
        "        <div class=\"content-body\" id=\"contentBody\">\n"
        "            <div class=\"empty-state\">Select a help report from the sidebar</div>\n"
        "        </div>\n"
        "    </div>\n"
        "    \n"
        "    <script>\n"
        "        const data = ");
    
    fprintf(out, "%s", json_str);
    free(json_str);
    
    fprintf(out, "%s",
        ";\n"
        "        \n"
        "        let currentFile = null;\n"
        "        let currentView = 'content';\n"
        "        \n"
        "        // Initialize file list\n"
        "        function initFileList() {\n"
        "            const fileList = document.getElementById('fileList');\n"
        "            fileList.innerHTML = '';\n"
        "            \n"
        "            data.files.forEach(file => {\n"
        "                const li = document.createElement('li');\n"
        "                li.className = 'file-item';\n"
        "                li.dataset.fragment = file.fragment;\n"
        "                if (file.status !== 'unchanged') {\n"
        "                    li.classList.add(file.status);\n"
        "                }\n"
        "                \n"
        "                const filename = document.createElement('span');\n"
        "                filename.textContent = file.usage || file.filename;\n"
        "                filename.title = file.content;\n"
        "                li.appendChild(filename);\n"
        "                \n"
        "                if (file.status !== 'unchanged') {\n"
        "                    const badge = document.createElement('span');\n"
        "                    badge.className = `status-badge ${file.status}`;\n"
        "                    badge.textContent = file.status;\n"
        "                    li.appendChild(badge);\n"
        "                }\n"
        "                \n"
        "                li.addEventListener('click', () => {\n"
        "                    window.location.hash = file.fragment;\n"
        "                    showFile(file);\n"
        "                });\n"
        "                \n"
        "                li.addEventListener('mouseenter', () => {\n"
        "                    if (currentFile !== file) {\n"
        "                        showFilePreview(file);\n"
        "                    }\n"
        "                });\n"
        "                \n"
        "                li.addEventListener('mouseleave', () => {\n"
        "                    if (currentFile !== file) {\n"
        "                        restoreCurrentFile();\n"
        "                    }\n"
        "                });\n"
        "                \n"
        "                fileList.appendChild(li);\n"
        "            });\n"
        "        }\n"
        "        \n"
        "        // Show file preview on hover (without changing fragment)\n"
        "        function showFilePreview(file) {\n"
        "            document.getElementById('contentTitle').textContent = file.usage || file.filename;\n"
        "            \n"
        "            const contentBody = document.getElementById('contentBody');\n"
        "            const pre = document.createElement('pre');\n"
        "            pre.className = 'help-content';\n"
        "            pre.textContent = file.content;\n"
        "            contentBody.innerHTML = '';\n"
        "            contentBody.appendChild(pre);\n"
        "        }\n"
        "        \n"
        "        // Restore the current file after hover\n"
        "        function restoreCurrentFile() {\n"
        "            if (currentFile) {\n"
        "                document.getElementById('contentTitle').textContent = currentFile.usage || currentFile.filename;\n"
        "                showCurrentView();\n"
        "            } else {\n"
        "                document.getElementById('contentTitle').textContent = 'Select a help report';\n"
        "                document.getElementById('contentBody').innerHTML = '<div class=\"empty-state\">Select a help report from the sidebar</div>';\n"
        "            }\n"
        "        }\n"
        "        \n"
        "        // Show file content\n"
        "        function showFile(file) {\n"
        "            currentFile = file;\n"
        "            currentView = 'content';\n"
        "            \n"
        "            // Update active state\n"
        "            document.querySelectorAll('.file-item').forEach((item, idx) => {\n"
        "                item.classList.toggle('active', data.files[idx] === file);\n"
        "            });\n"
        "            \n"
        "            // Update header\n"
        "            document.getElementById('contentTitle').textContent = file.usage || file.filename;\n"
        "            \n"
        "            // Show/hide view toggle\n"
        "            const viewToggle = document.getElementById('viewToggle');\n"
        "            if (file.status === 'changed') {\n"
        "                viewToggle.style.display = 'flex';\n"
        "            } else {\n"
        "                viewToggle.style.display = 'none';\n"
        "            }\n"
        "            \n"
        "            // Reset view buttons\n"
        "            document.querySelectorAll('.view-button').forEach(btn => {\n"
        "                btn.classList.toggle('active', btn.dataset.view === 'content');\n"
        "            });\n"
        "            \n"
        "            // Show content\n"
        "            showCurrentView();\n"
        "        }\n"
        "        \n"
        "        // Show current view (content or diff)\n"
        "        function showCurrentView() {\n"
        "            const contentBody = document.getElementById('contentBody');\n"
        "            \n"
        "            if (currentView === 'content') {\n"
        "                const pre = document.createElement('pre');\n"
        "                pre.className = 'help-content';\n"
        "                pre.textContent = currentFile.content;\n"
        "                contentBody.innerHTML = '';\n"
        "                contentBody.appendChild(pre);\n"
        "            } else if (currentView === 'diff') {\n"
        "                if (currentFile.diff && currentFile.diff.trim()) {\n"
        "                    contentBody.innerHTML = `<pre class=\"diff-content\">${currentFile.diff}</pre>`;\n"
        "                } else {\n"
        "                    contentBody.innerHTML = `<div class=\"empty-state\"><span class=\"no-diff\">No differences</span></div>`;\n"
        "                }\n"
        "            }\n"
        "        }\n"
        "        \n"
        "        // Load file from fragment\n"
        "        function loadFromFragment() {\n"
        "            const hash = window.location.hash.slice(1);\n"
        "            if (hash) {\n"
        "                const file = data.files.find(f => f.fragment === hash);\n"
        "                if (file) {\n"
        "                    showFile(file);\n"
        "                }\n"
        "            }\n"
        "        }\n"
        "        \n"
        "        // View toggle handlers\n"
        "        document.getElementById('viewToggle').addEventListener('click', (e) => {\n"
        "            if (e.target.classList.contains('view-button')) {\n"
        "                currentView = e.target.dataset.view;\n"
        "                document.querySelectorAll('.view-button').forEach(btn => {\n"
        "                    btn.classList.toggle('active', btn === e.target);\n"
        "                });\n"
        "                showCurrentView();\n"
        "            }\n"
        "        });\n"
        "        \n"
        "        // Handle fragment changes\n"
        "        window.addEventListener('hashchange', loadFromFragment);\n"
        "        \n"
        "        // Initialize\n"
        "        initFileList();\n"
        "        loadFromFragment();\n"
        "    </script>\n"
        "</body>\n"
        "</html>\n");
    
    fclose(out);
    free(version);
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <data-dir> <wrap-bin> <output-dir>\n", argv[0]);
        return 1;
    }
    
    const char *data_dir = argv[1];
    const char *wrap_bin = argv[2];
    const char *output_dir = argv[3];
    
    /* Check if data directory exists */
    struct stat st;
    if (stat(data_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Error: Data directory does not exist: %s\n", data_dir);
        return 1;
    }
    
    /* Check if wrap binary exists */
    if (stat(wrap_bin, &st) != 0) {
        fprintf(stderr, "Error: wrap binary does not exist: %s\n", wrap_bin);
        return 1;
    }
    
    return generate_html(data_dir, wrap_bin, output_dir);
}
