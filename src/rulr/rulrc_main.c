/**
 * rulrc - Rulr Rule Compiler
 *
 * Compiles .dl (Datalog) rule files to binary .dlc format for faster loading
 * and guaranteed error-free runtime execution.
 *
 * Usage:
 *   rulrc compile SOURCE_FILE         Compile a .dl file to .dlc
 *   rulrc compile --output PATH       Compile from stdin to output file
 *   rulrc view PATH                   Pretty-print a compiled file
 *   rulrc PATH [PATH ...]             Compile .dl files (legacy mode)
 *   rulrc --help                      Show help
 *
 * If a path is a directory, all .dl files in that directory are compiled.
 * Output files are written to the same directory with .dlc extension.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <libgen.h>

#include "alloc.h"
#include "frontend/ast.h"
#include "frontend/ast_serialize.h"
#include "ir/ir_builder.h"
#include "engine/engine.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static int verbose = 0;

static void print_usage(const char *prog) {
    printf("Usage: %s compile [options] RULE_FILE\n", prog);
    printf("       %s compile --output OUTPUT_FILE  (read from stdin)\n", prog);
    printf("       %s view COMPILED_FILE\n", prog);
    printf("       %s [options] PATH [PATH ...]  (batch mode)\n", prog);
    printf("\n");
    printf("Compile .dl source rule files to binary .dlc format.\n");
    printf("\n");
    printf("Commands:\n");
    printf("  compile FILE     Compile a source FILE\n");
    printf("  view FILE        Pretty-print a compiled FILE\n");
    printf("  PATH             Compile all source file(s) in a directory at PATH\n");
    printf("\n");
    printf("Options:\n");
    printf("  -o, --output FILE    Output file path (for compile command)\n");
    printf("  -v, --verbose        Verbose output\n");
    printf("  -h, --help           Show this help message\n");
    printf("\n");
    printf("If no --output is specified, output is written with .dlc extension.\n");
    printf("Use --output with stdin: cat rule.dl | %s compile --output rule.dlc\n", prog);
}

static int ends_with(const char *str, const char *suffix) {
    size_t str_len = strlen(str);
    size_t suffix_len = strlen(suffix);
    if (suffix_len > str_len) return 0;
    return strcmp(str + str_len - suffix_len, suffix) == 0;
}

/* Dummy symbol interning for validation pass */
typedef struct {
    char **names;
    int    count;
    int    capacity;
} TempSymTable;

static int temp_intern(void *user, const char *s) {
    TempSymTable *st = (TempSymTable *)user;
    for (int i = 0; i < st->count; i++) {
        if (strcmp(st->names[i], s) == 0) {
            return i;
        }
    }
    /* Add new symbol */
    if (st->count >= st->capacity) {
        int new_cap = st->capacity == 0 ? 64 : st->capacity * 2;
        char **new_names = (char **)arena_realloc(st->names, new_cap * sizeof(char *));
        if (!new_names) return -1;
        st->names = new_names;
        st->capacity = new_cap;
    }
    st->names[st->count] = arena_strdup(s);
    return st->count++;
}

/**
 * Compile source code to output file.
 * Returns 0 on success, non-zero on failure.
 */
static int compile_source(const char *source, const char *source_name, const char *output_path) {
    /* Parse */
    AstProgram ast;
    ast_program_init(&ast);
    ParseError parse_err = parse_program(source, &ast);
    
    if (parse_err.is_error) {
        fprintf(stderr, "Error: Parse error in %s: %s\n", source_name, parse_err.message);
        return 1;
    }
    
    /* Validate by building IR (type checking, stratification) */
    IrProgram ir;
    ir_program_init(&ir);
    TempSymTable symtab = {NULL, 0, 0};
    EngineError ir_err = ir_build_from_ast(&ast, &ir, temp_intern, &symtab);
    
    if (ir_err.is_error) {
        fprintf(stderr, "Error: Validation error in %s: %s\n", source_name, ir_err.message);
        return 1;
    }
    
    /* Serialize */
    AstSerializeError ser_err = ast_serialize_to_file(&ast, output_path);
    if (ser_err.is_error) {
        fprintf(stderr, "Error: Serialization failed: %s\n", ser_err.message);
        return 1;
    }
    
    return 0;
}

/**
 * Read all content from stdin into a string.
 */
static char *read_stdin(void) {
    size_t capacity = 4096;
    size_t size = 0;
    char *buffer = (char *)arena_malloc(capacity);
    if (!buffer) return NULL;
    
    int c;
    while ((c = getchar()) != EOF) {
        if (size + 1 >= capacity) {
            capacity *= 2;
            char *new_buf = (char *)arena_realloc(buffer, capacity);
            if (!new_buf) return NULL;
            buffer = new_buf;
        }
        buffer[size++] = (char)c;
    }
    buffer[size] = '\0';
    return buffer;
}

/**
 * Compile a single .dl file to .dlc
 * Returns 0 on success, non-zero on failure.
 */
static int compile_file(const char *input_path) {
    /* Read source file */
    FILE *f = fopen(input_path, "rb");
    if (!f) {
        fprintf(stderr, "Error: Cannot open file: %s\n", input_path);
        return 1;
    }
    
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    rewind(f);
    
    char *source = (char *)arena_malloc((size_t)file_size + 1);
    if (!source) {
        fclose(f);
        fprintf(stderr, "Error: Out of memory reading: %s\n", input_path);
        return 1;
    }
    
    size_t read_size = fread(source, 1, (size_t)file_size, f);
    fclose(f);
    source[read_size] = '\0';
    
    /* Construct output path */
    char output_path[PATH_MAX];
    size_t input_len = strlen(input_path);
    if (input_len >= 3 && strcmp(input_path + input_len - 3, ".dl") == 0) {
        snprintf(output_path, sizeof(output_path), "%.*sc", (int)(input_len), input_path);
    } else {
        snprintf(output_path, sizeof(output_path), "%s.dlc", input_path);
    }
    
    int result = compile_source(source, input_path, output_path);
    if (result != 0) {
        return result;
    }
    
    if (verbose) {
        /* Get file sizes for comparison */
        struct stat input_stat, output_stat;
        stat(input_path, &input_stat);
        stat(output_path, &output_stat);
        
        printf("Compiled: %s -> %s (%ld -> %ld bytes, %.1f%%)\n",
               input_path, output_path,
               (long)input_stat.st_size, (long)output_stat.st_size,
               100.0 * output_stat.st_size / (input_stat.st_size > 0 ? input_stat.st_size : 1));
    } else {
        printf("Compiled: %s\n", input_path);
    }
    
    return 0;
}

/**
 * Process a path: if file, compile it; if directory, compile all .dl files.
 */
static int process_path(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        fprintf(stderr, "Error: Cannot access: %s\n", path);
        return 1;
    }
    
    if (S_ISREG(st.st_mode)) {
        /* It's a file */
        if (!ends_with(path, ".dl")) {
            fprintf(stderr, "Warning: Skipping non-.dl file: %s\n", path);
            return 0;
        }
        return compile_file(path);
    }
    
    if (S_ISDIR(st.st_mode)) {
        /* It's a directory - process all .dl files */
        DIR *dir = opendir(path);
        if (!dir) {
            fprintf(stderr, "Error: Cannot open directory: %s\n", path);
            return 1;
        }
        
        int errors = 0;
        int count = 0;
        struct dirent *entry;
        
        /* Strip trailing slash from path if present */
        size_t path_len = strlen(path);
        const char *clean_path = path;
        char path_buf[PATH_MAX];
        if (path_len > 0 && path[path_len - 1] == '/') {
            snprintf(path_buf, sizeof(path_buf), "%.*s", (int)(path_len - 1), path);
            clean_path = path_buf;
        }
        
        while ((entry = readdir(dir)) != NULL) {
            if (!ends_with(entry->d_name, ".dl")) {
                continue;
            }
            
            char full_path[PATH_MAX];
            snprintf(full_path, sizeof(full_path), "%s/%s", clean_path, entry->d_name);
            
            if (compile_file(full_path) != 0) {
                errors++;
            } else {
                count++;
            }
        }
        
        closedir(dir);
        
        if (count == 0 && errors == 0) {
            fprintf(stderr, "Warning: No .dl files found in: %s\n", path);
        }
        
        return errors > 0 ? 1 : 0;
    }
    
    fprintf(stderr, "Error: Not a file or directory: %s\n", path);
    return 1;
}

/**
 * View (pretty-print) a compiled .dlc file.
 */
static int view_file(const char *path) {
    AstProgram ast;
    ast_program_init(&ast);
    
    AstSerializeError err = ast_deserialize_from_file(path, &ast);
    if (err.is_error) {
        fprintf(stderr, "Error: Failed to read %s: %s\n", path, err.message);
        return 1;
    }
    
    /* Extract rule name from path (basename without extension) */
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    size_t name_len = strlen(base);
    if (name_len > 4 && strcmp(base + name_len - 4, ".dlc") == 0) {
        name_len -= 4;
    }
    printf("%% %.*s\n\n", (int)name_len, base);
    
    ast_pretty_print(&ast);
    
    return 0;
}

int main(int argc, char *argv[]) {
    alloc_init();
    
    if (argc < 2) {
        print_usage(argv[0]);
        alloc_shutdown();
        return 1;
    }
    
    /* Check for help */
    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        print_usage(argv[0]);
        alloc_shutdown();
        return 0;
    }
    
    /* Check for view command */
    if (strcmp(argv[1], "view") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: view command requires a file path\n");
            print_usage(argv[0]);
            alloc_shutdown();
            return 1;
        }
        int result = view_file(argv[2]);
        alloc_shutdown();
        return result;
    }
    
    /* Check for compile command */
    if (strcmp(argv[1], "compile") == 0) {
        const char *output_path = NULL;
        const char *input_path = NULL;
        
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "Error: --output requires a file path\n");
                    alloc_shutdown();
                    return 1;
                }
                output_path = argv[++i];
            } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
                verbose = 1;
            } else if (argv[i][0] != '-') {
                input_path = argv[i];
            } else {
                fprintf(stderr, "Error: Unknown option: %s\n", argv[i]);
                alloc_shutdown();
                return 1;
            }
        }
        
        int result;
        if (input_path) {
            /* Compile from file */
            if (output_path) {
                /* Read file and compile to specified output */
                FILE *f = fopen(input_path, "rb");
                if (!f) {
                    fprintf(stderr, "Error: Cannot open file: %s\n", input_path);
                    alloc_shutdown();
                    return 1;
                }
                fseek(f, 0, SEEK_END);
                long file_size = ftell(f);
                rewind(f);
                char *source = (char *)arena_malloc((size_t)file_size + 1);
                if (!source) {
                    fclose(f);
                    fprintf(stderr, "Error: Out of memory\n");
                    alloc_shutdown();
                    return 1;
                }
                size_t read_size = fread(source, 1, (size_t)file_size, f);
                fclose(f);
                source[read_size] = '\0';
                
                result = compile_source(source, input_path, output_path);
                if (result == 0 && verbose) {
                    struct stat output_stat;
                    stat(output_path, &output_stat);
                    printf("Compiled: %s -> %s (%ld -> %ld bytes)\n",
                           input_path, output_path, file_size, (long)output_stat.st_size);
                } else if (result == 0) {
                    printf("Compiled: %s -> %s\n", input_path, output_path);
                }
            } else {
                /* Use default output path */
                result = compile_file(input_path);
            }
        } else if (output_path) {
            /* Compile from stdin */
            char *source = read_stdin();
            if (!source) {
                fprintf(stderr, "Error: Failed to read from stdin\n");
                alloc_shutdown();
                return 1;
            }
            result = compile_source(source, "<stdin>", output_path);
            if (result == 0) {
                if (verbose) {
                    struct stat output_stat;
                    stat(output_path, &output_stat);
                    printf("Compiled: <stdin> -> %s (%ld bytes)\n", output_path, (long)output_stat.st_size);
                }
            }
        } else {
            fprintf(stderr, "Error: compile command requires an input file or --output for stdin\n");
            print_usage(argv[0]);
            alloc_shutdown();
            return 1;
        }
        
        alloc_shutdown();
        return result;
    }
    
    /* Process compile arguments (batch mode) */
    int start_idx = 1;
    int errors = 0;
    
    for (int i = start_idx; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            verbose = 1;
            continue;
        }
        
        if (argv[i][0] == '-') {
            fprintf(stderr, "Error: Unknown option: %s\n", argv[i]);
            errors++;
            continue;
        }
        
        if (process_path(argv[i]) != 0) {
            errors++;
        }
    }
    
    alloc_shutdown();
    return errors > 0 ? 1 : 0;
}
