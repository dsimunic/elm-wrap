#include "rulr_dl.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "alloc.h"
#include "frontend/ast.h"
#include "frontend/ast_serialize.h"

typedef struct {
    char  *data;
    size_t len;
} LoadedFile;

static RulrError read_entire_file(const char *path, LoadedFile *out) {
    if (!path || !out) {
        return rulr_error("Invalid file input");
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        return rulr_error("Failed to open file");
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return rulr_error("Failed to seek file");
    }
    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        return rulr_error("Failed to tell file size");
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return rulr_error("Failed to rewind file");
    }
    char *buffer = (char *)arena_malloc((size_t)size + 1);
    if (!buffer) {
        fclose(f);
        return rulr_error("Out of memory reading file");
    }
    size_t read_len = fread(buffer, 1, (size_t)size, f);
    fclose(f);
    buffer[read_len] = '\0';
    out->data = buffer;
    out->len = read_len;
    return rulr_ok();
}

static int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static int has_extension(const char *path, const char *ext) {
    size_t path_len = strlen(path);
    size_t ext_len = strlen(ext);
    if (ext_len > path_len) return 0;
    return strcmp(path + path_len - ext_len, ext) == 0;
}

RulrError rulr_load_dl_file(Rulr *r, const char *path) {
    LoadedFile file = {0};
    RulrError err = read_entire_file(path, &file);
    if (err.is_error) {
        return err;
    }
    return rulr_load_program(r, file.data);
}

RulrError rulr_load_dl_files(Rulr *r, const char *rule_path, const char *fact_path) {
    LoadedFile rules = {0};
    LoadedFile facts = {0};
    RulrError err = read_entire_file(rule_path, &rules);
    if (err.is_error) {
        return err;
    }
    if (!fact_path) {
        return rulr_load_program(r, rules.data);
    }
    err = read_entire_file(fact_path, &facts);
    if (err.is_error) {
        return err;
    }
    size_t total = rules.len + facts.len + 2;
    char *combined = (char *)arena_malloc(total + 1);
    if (!combined) {
        return rulr_error("Out of memory combining files");
    }
    memcpy(combined, rules.data, rules.len);
    combined[rules.len] = '\n';
    memcpy(combined + rules.len + 1, facts.data, facts.len);
    combined[total - 1] = '\n';
    combined[total] = '\0';
    return rulr_load_program(r, combined);
}

RulrError rulr_load_compiled_file(Rulr *r, const char *path) {
    if (!r || !path) {
        return rulr_error("Invalid input to rulr_load_compiled_file");
    }
    
    AstProgram ast;
    ast_program_init(&ast);
    
    AstSerializeError serr = ast_deserialize_from_file(path, &ast);
    if (serr.is_error) {
        return rulr_error(serr.message);
    }
    
    return rulr_load_program_ast(r, &ast);
}

RulrError rulr_load_rule_file(Rulr *r, const char *name) {
    if (!r || !name) {
        return rulr_error("Invalid input to rulr_load_rule_file");
    }
    
    /* If name already has an extension, use it directly */
    if (has_extension(name, RULR_SOURCE_EXT)) {
        return rulr_load_dl_file(r, name);
    }
    if (has_extension(name, RULR_COMPILED_EXT)) {
        return rulr_load_compiled_file(r, name);
    }
    
    /* Build paths with extensions */
    size_t name_len = strlen(name);
    size_t src_ext_len = strlen(RULR_SOURCE_EXT);
    size_t cmp_ext_len = strlen(RULR_COMPILED_EXT);
    
    char *compiled_path = arena_malloc(name_len + cmp_ext_len + 1);
    char *source_path = arena_malloc(name_len + src_ext_len + 1);
    
    if (!compiled_path || !source_path) {
        return rulr_error("Out of memory building rule paths");
    }
    
    snprintf(compiled_path, name_len + cmp_ext_len + 1, "%s%s", name, RULR_COMPILED_EXT);
    snprintf(source_path, name_len + src_ext_len + 1, "%s%s", name, RULR_SOURCE_EXT);
    
    /* Try compiled file first */
    if (file_exists(compiled_path)) {
        return rulr_load_compiled_file(r, compiled_path);
    }
    
    /* Fall back to source file */
    if (file_exists(source_path)) {
        return rulr_load_dl_file(r, source_path);
    }
    
    /* Neither exists - build error message with enough space for all paths */
    size_t errmsg_len = name_len + strlen(compiled_path) + strlen(source_path) + 64;
    char *errmsg = arena_malloc(errmsg_len);
    if (!errmsg) {
        return rulr_error("Rule file not found (out of memory for error details)");
    }
    snprintf(errmsg, errmsg_len, "Rule file not found: %s (tried %s and %s)", 
             name, compiled_path, source_path);
    return rulr_error(errmsg);
}
