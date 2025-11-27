#ifndef ELM_DOCS_H
#define ELM_DOCS_H

#include <stdbool.h>
#include "dependency_cache.h"

/* Data structures for documentation */
typedef struct {
    char *name;
    char *comment;
    char *type;
} ElmValue;

typedef struct {
    char *name;
    char *comment;
    char **args;
    int args_count;
    char *type;
} ElmAlias;

typedef struct {
    char *name;
    char **arg_types;
    int arg_types_count;
} ElmUnionCase;

typedef struct {
    char *name;
    char *comment;
    char **args;
    int args_count;
    ElmUnionCase *cases;
    int cases_count;
} ElmUnion;

typedef struct {
    char *name;
    char *comment;
    char *type;
    int precedence;
    char *associativity;
} ElmBinop;

typedef struct {
    char *name;
    char *comment;
    ElmValue *values;
    int values_count;
    ElmAlias *aliases;
    int aliases_count;
    ElmUnion *unions;
    int unions_count;
    ElmBinop *binops;
    int binops_count;
} ElmModuleDocs;

/* Function declarations */
bool parse_elm_file(const char *filepath, ElmModuleDocs *docs, DependencyCache *dep_cache);
void free_elm_docs(ElmModuleDocs *docs);

#endif /* ELM_DOCS_H */
