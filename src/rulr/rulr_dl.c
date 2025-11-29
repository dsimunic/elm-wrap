#include "rulr_dl.h"

#include <stdio.h>
#include <string.h>

#include "alloc.h"

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
