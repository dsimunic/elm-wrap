#include "elm_docs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tree_sitter/api.h>
#include "../../../alloc.h"

/* External tree-sitter language function */
extern TSLanguage *tree_sitter_elm(void);

/* Helper function to read file contents */
static char *read_file(const char *filepath) {
    FILE *file = fopen(filepath, "r");
    if (!file) {
        fprintf(stderr, "Error: Could not open file %s\n", filepath);
        return NULL;
    }

    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    char *content = arena_malloc(file_size + 1);
    if (!content) {
        fclose(file);
        return NULL;
    }

    size_t read_size = fread(content, 1, file_size, file);
    content[read_size] = '\0';
    fclose(file);

    return content;
}

/* Helper function to get node text */
static char *get_node_text(TSNode node, const char *source_code) {
    uint32_t start = ts_node_start_byte(node);
    uint32_t end = ts_node_end_byte(node);
    uint32_t length = end - start;

    char *text = arena_malloc(length + 1);
    if (!text) return NULL;

    memcpy(text, source_code + start, length);
    text[length] = '\0';
    return text;
}

/* Import tracking */
typedef struct {
    char *type_name;       /* The type name as used in this module */
    char *module_name;     /* The module it comes from */
} TypeImport;

typedef struct {
    TypeImport *imports;
    int imports_count;
    int imports_capacity;
} ImportMap;

/* Module alias tracking (for 'import Foo as F') */
typedef struct {
    char *alias;           /* The alias used in this module (e.g., "D") */
    char *full_module;     /* The full module name (e.g., "Json.Decode") */
} ModuleAlias;

typedef struct {
    ModuleAlias *aliases;
    int aliases_count;
    int aliases_capacity;
} ModuleAliasMap;

/* Export list tracking */
typedef struct {
    char **exposed_values;
    int exposed_values_count;
    char **exposed_types;
    int exposed_types_count;
    char **exposed_types_with_constructors;  /* Types exposed with (..) */
    int exposed_types_with_constructors_count;
    bool expose_all;
} ExportList;

static void init_import_map(ImportMap *map) {
    map->imports = arena_malloc(32 * sizeof(TypeImport));
    map->imports_count = 0;
    map->imports_capacity = 32;
}

static void add_import(ImportMap *map, const char *type_name, const char *module_name) {
    if (map->imports_count >= map->imports_capacity) {
        map->imports_capacity *= 2;
        map->imports = arena_realloc(map->imports, map->imports_capacity * sizeof(TypeImport));
    }
    map->imports[map->imports_count].type_name = arena_strdup(type_name);
    map->imports[map->imports_count].module_name = arena_strdup(module_name);
    map->imports_count++;
}

static const char *lookup_import(ImportMap *map, const char *type_name) {
    for (int i = 0; i < map->imports_count; i++) {
        if (strcmp(map->imports[i].type_name, type_name) == 0) {
            return map->imports[i].module_name;
        }
    }
    return NULL;
}

static void free_import_map(ImportMap *map) {
    for (int i = 0; i < map->imports_count; i++) {
        arena_free(map->imports[i].type_name);
        arena_free(map->imports[i].module_name);
    }
    arena_free(map->imports);
}

/* Module alias map functions */
static void init_module_alias_map(ModuleAliasMap *map) {
    map->aliases = arena_malloc(16 * sizeof(ModuleAlias));
    map->aliases_count = 0;
    map->aliases_capacity = 16;
}

static void add_module_alias(ModuleAliasMap *map, const char *alias, const char *full_module) {
    if (map->aliases_count >= map->aliases_capacity) {
        map->aliases_capacity *= 2;
        map->aliases = arena_realloc(map->aliases, map->aliases_capacity * sizeof(ModuleAlias));
    }
    map->aliases[map->aliases_count].alias = arena_strdup(alias);
    map->aliases[map->aliases_count].full_module = arena_strdup(full_module);
    map->aliases_count++;
}

static const char *lookup_module_alias(ModuleAliasMap *map, const char *alias) {
    for (int i = 0; i < map->aliases_count; i++) {
        if (strcmp(map->aliases[i].alias, alias) == 0) {
            return map->aliases[i].full_module;
        }
    }
    return NULL;
}

static void free_module_alias_map(ModuleAliasMap *map) {
    for (int i = 0; i < map->aliases_count; i++) {
        arena_free(map->aliases[i].alias);
        arena_free(map->aliases[i].full_module);
    }
    arena_free(map->aliases);
}

static void free_export_list(ExportList *exports) {
    for (int i = 0; i < exports->exposed_values_count; i++) {
        arena_free(exports->exposed_values[i]);
    }
    arena_free(exports->exposed_values);

    for (int i = 0; i < exports->exposed_types_count; i++) {
        arena_free(exports->exposed_types[i]);
    }
    arena_free(exports->exposed_types);

    for (int i = 0; i < exports->exposed_types_with_constructors_count; i++) {
        arena_free(exports->exposed_types_with_constructors[i]);
    }
    arena_free(exports->exposed_types_with_constructors);
}

static bool is_exported_value(const char *name, ExportList *exports) {
    if (exports->expose_all) return true;
    for (int i = 0; i < exports->exposed_values_count; i++) {
        if (strcmp(exports->exposed_values[i], name) == 0) return true;
    }
    return false;
}

static bool is_exported_type(const char *name, ExportList *exports) {
    if (exports->expose_all) return true;
    for (int i = 0; i < exports->exposed_types_count; i++) {
        if (strcmp(exports->exposed_types[i], name) == 0) return true;
    }
    return false;
}

static bool is_type_exposed_with_constructors(const char *name, ExportList *exports) {
    if (exports->expose_all) return true;
    for (int i = 0; i < exports->exposed_types_with_constructors_count; i++) {
        if (strcmp(exports->exposed_types_with_constructors[i], name) == 0) return true;
    }
    return false;
}

/* Helper function to extract module name and exports */
static char *extract_module_info(TSNode root, const char *source_code, ExportList *exports) {
    uint32_t child_count = ts_node_child_count(root);
    char *module_name = NULL;

    /* Initialize export list */
    exports->exposed_values = NULL;
    exports->exposed_values_count = 0;
    exports->exposed_types = NULL;
    exports->exposed_types_count = 0;
    exports->exposed_types_with_constructors = NULL;
    exports->exposed_types_with_constructors_count = 0;
    exports->expose_all = false;

    for (uint32_t i = 0; i < child_count; i++) {
        TSNode child = ts_node_child(root, i);
        const char *type = ts_node_type(child);

        if (strcmp(type, "module_declaration") == 0) {
            /* Find the upper_case_qid node which contains the module name */
            uint32_t mod_child_count = ts_node_child_count(child);
            for (uint32_t j = 0; j < mod_child_count; j++) {
                TSNode mod_child = ts_node_child(child, j);
                const char *mod_type = ts_node_type(mod_child);

                if (strcmp(mod_type, "upper_case_qid") == 0) {
                    module_name = get_node_text(mod_child, source_code);
                } else if (strcmp(mod_type, "exposing_list") == 0) {
                    /* Parse exposing list */
                    uint32_t exp_child_count = ts_node_child_count(mod_child);
                    for (uint32_t k = 0; k < exp_child_count; k++) {
                        TSNode exp_child = ts_node_child(mod_child, k);
                        const char *exp_type = ts_node_type(exp_child);

                        if (strcmp(exp_type, "double_dot") == 0) {
                            exports->expose_all = true;
                        } else if (strcmp(exp_type, "exposed_value") == 0) {
                            /* Exposed value/function */
                            char *value_name = get_node_text(exp_child, source_code);
                            exports->exposed_values = arena_realloc(exports->exposed_values,
                                (exports->exposed_values_count + 1) * sizeof(char*));
                            exports->exposed_values[exports->exposed_values_count++] = value_name;
                        } else if (strcmp(exp_type, "exposed_type") == 0) {
                            /* Exposed type */
                            char *type_name = NULL;
                            bool has_constructors = false;

                            uint32_t type_child_count = ts_node_child_count(exp_child);
                            for (uint32_t m = 0; m < type_child_count; m++) {
                                TSNode type_child = ts_node_child(exp_child, m);
                                const char *tc_type = ts_node_type(type_child);

                                if (strcmp(tc_type, "upper_case_identifier") == 0) {
                                    type_name = get_node_text(type_child, source_code);
                                } else if (strcmp(tc_type, "exposed_union_constructors") == 0) {
                                    /* Check if this has (..) */
                                    has_constructors = true;
                                }
                            }

                            if (type_name) {
                                exports->exposed_types = arena_realloc(exports->exposed_types,
                                    (exports->exposed_types_count + 1) * sizeof(char*));
                                exports->exposed_types[exports->exposed_types_count++] = arena_strdup(type_name);

                                if (has_constructors) {
                                    exports->exposed_types_with_constructors = arena_realloc(
                                        exports->exposed_types_with_constructors,
                                        (exports->exposed_types_with_constructors_count + 1) * sizeof(char*));
                                    exports->exposed_types_with_constructors[
                                        exports->exposed_types_with_constructors_count++] = type_name;
                                } else {
                                    arena_free(type_name);
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    return module_name ? module_name : arena_strdup("Unknown");
}

/* Helper function to parse imports */
static void extract_imports(TSNode root, const char *source_code, ImportMap *import_map, ModuleAliasMap *alias_map) {
    uint32_t child_count = ts_node_child_count(root);

    for (uint32_t i = 0; i < child_count; i++) {
        TSNode child = ts_node_child(root, i);
        const char *type = ts_node_type(child);

        if (strcmp(type, "import_clause") == 0) {
            char *module_name = NULL;
            char *module_alias = NULL;

            /* Find the module name and optional alias */
            uint32_t import_child_count = ts_node_child_count(child);
            for (uint32_t j = 0; j < import_child_count; j++) {
                TSNode import_child = ts_node_child(child, j);
                const char *import_child_type = ts_node_type(import_child);

                if (strcmp(import_child_type, "upper_case_qid") == 0) {
                    module_name = get_node_text(import_child, source_code);
                } else if (strcmp(import_child_type, "as_clause") == 0) {
                    /* Extract the alias from as_clause */
                    uint32_t as_child_count = ts_node_child_count(import_child);
                    for (uint32_t k = 0; k < as_child_count; k++) {
                        TSNode as_child = ts_node_child(import_child, k);
                        if (strcmp(ts_node_type(as_child), "upper_case_identifier") == 0) {
                            module_alias = get_node_text(as_child, source_code);
                            break;
                        }
                    }
                } else if (strcmp(import_child_type, "exposing_list") == 0 && module_name) {
                    /* Parse the exposing list */
                    uint32_t exp_child_count = ts_node_child_count(import_child);
                    for (uint32_t k = 0; k < exp_child_count; k++) {
                        TSNode exp_child = ts_node_child(import_child, k);
                        const char *exp_type = ts_node_type(exp_child);

                        if (strcmp(exp_type, "exposed_type") == 0) {
                            /* Extract type name */
                            uint32_t type_child_count = ts_node_child_count(exp_child);
                            for (uint32_t m = 0; m < type_child_count; m++) {
                                TSNode type_child = ts_node_child(exp_child, m);
                                if (strcmp(ts_node_type(type_child), "upper_case_identifier") == 0) {
                                    char *type_name = get_node_text(type_child, source_code);
                                    add_import(import_map, type_name, module_name);
                                    arena_free(type_name);
                                    break;
                                }
                            }
                        } else if (strcmp(exp_type, "exposed_value") == 0) {
                            /* Check if it's an uppercase identifier (type constructor) */
                            char *value_name = get_node_text(exp_child, source_code);
                            if (value_name && value_name[0] >= 'A' && value_name[0] <= 'Z') {
                                add_import(import_map, value_name, module_name);
                            }
                            arena_free(value_name);
                        }
                    }
                }
            }

            /* Record module alias if present */
            if (module_name && module_alias) {
                add_module_alias(alias_map, module_alias, module_name);
            }

            if (module_name) {
                arena_free(module_name);
            }
            if (module_alias) {
                arena_free(module_alias);
            }
        }
    }
}

/* Helper function to clean documentation comment */
static char *clean_comment(const char *raw_comment) {
    if (!raw_comment) return arena_strdup("");

    size_t len = strlen(raw_comment);
    if (len < 5) return arena_strdup("");  /* Too short to be {-| ... -} */

    /* Check if it starts with {-| and ends with -} */
    if (strncmp(raw_comment, "{-|", 3) != 0) {
        return arena_strdup("");
    }
    if (len < 5 || strcmp(raw_comment + len - 2, "-}") != 0) {
        return arena_strdup("");
    }

    /* Extract content between {-| and -} */
    size_t content_len = len - 5;  /* Remove {-| and -} */
    char *cleaned = arena_malloc(content_len + 1);
    if (!cleaned) return arena_strdup("");

    memcpy(cleaned, raw_comment + 3, content_len);
    cleaned[content_len] = '\0';

    return cleaned;
}

/* Helper function to find comment preceding a node */
static char *find_preceding_comment(TSNode node, TSNode root, const char *source_code) {
    (void)root;

    /* Look for previous sibling that is a block_comment */
    TSNode prev_sibling = ts_node_prev_sibling(node);

    while (!ts_node_is_null(prev_sibling)) {
        const char *type = ts_node_type(prev_sibling);

        if (strcmp(type, "block_comment") == 0) {
            char *raw = get_node_text(prev_sibling, source_code);
            char *cleaned = clean_comment(raw);
            arena_free(raw);
            return cleaned;
        }

        /* Skip whitespace/newline nodes */
        if (strcmp(type, "\n") != 0) {
            break;
        }

        prev_sibling = ts_node_prev_sibling(prev_sibling);
    }

    return arena_strdup("");
}

/* Helper function to normalize whitespace - convert newlines and multiple spaces to single spaces */
static char *normalize_whitespace(const char *str) {
    if (!str) return arena_strdup("");

    size_t len = strlen(str);
    char *result = arena_malloc(len + 1);
    size_t pos = 0;
    bool last_was_space = false;

    for (size_t i = 0; i < len; i++) {
        char c = str[i];
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
            if (!last_was_space && pos > 0) {
                result[pos++] = ' ';
                last_was_space = true;
            }
        } else {
            result[pos++] = c;
            last_was_space = false;
        }
    }

    /* Remove trailing space */
    if (pos > 0 && result[pos - 1] == ' ') {
        pos--;
    }

    result[pos] = '\0';

    /* Remove spaces before commas */
    char *final = arena_malloc(pos + 1);
    size_t final_pos = 0;
    for (size_t i = 0; i < pos; i++) {
        if (result[i] == ' ' && i + 1 < pos && result[i + 1] == ',') {
            /* Skip space before comma */
            continue;
        }
        final[final_pos++] = result[i];
    }
    final[final_pos] = '\0';
    arena_free(result);

    return final;
}

/* Helper function to qualify type names based on import map and local types */
static char *qualify_type_names(const char *type_str, const char *module_name,
                                  ImportMap *import_map, ModuleAliasMap *alias_map,
                                  char **local_types, int local_types_count) {
    size_t buf_size = strlen(type_str) * 3 + 1024;  /* Extra space for qualifications */
    char *result = arena_malloc(buf_size);
    size_t pos = 0;
    const char *p = type_str;

    while (*p && pos < buf_size - 200) {
        if (*p >= 'A' && *p <= 'Z') {
            /* Found an uppercase identifier - might be a type */
            const char *start = p;
            while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                   (*p >= '0' && *p <= '9') || *p == '_') {
                p++;
            }
            size_t len = p - start;
            char typename[256];
            if (len < sizeof(typename)) {
                memcpy(typename, start, len);
                typename[len] = '\0';

                /* Check if this is part of an already-qualified type in the SOURCE */
                /* Look backward: if there's a dot before this, it's already qualified */
                bool already_qualified = false;
                if (start > type_str && *(start - 1) == '.') {
                    already_qualified = true;
                }

                /* Check if this is a module prefix (followed by a dot) */
                bool is_module_prefix = (*p == '.');

                if (already_qualified) {
                    /* Keep as-is - already qualified */
                    memcpy(result + pos, start, len);
                    pos += len;
                } else if (is_module_prefix) {
                    /* This is a module prefix - check if it's an alias that needs expanding */
                    const char *full_module = lookup_module_alias(alias_map, typename);
                    if (full_module) {
                        /* Expand the alias to the full module name */
                        size_t flen = strlen(full_module);
                        memcpy(result + pos, full_module, flen);
                        pos += flen;
                    } else {
                        /* Keep as-is - not an alias */
                        memcpy(result + pos, start, len);
                        pos += len;
                    }
                } else {
                    /* Check if it's imported */
                    const char *import_module = lookup_import(import_map, typename);
                    if (import_module) {
                        /* Use the imported module name */
                        pos += snprintf(result + pos, buf_size - pos, "%s.%s", import_module, typename);
                    } else {
                        /* Check if it's a local type */
                        bool is_local = false;
                        for (int i = 0; i < local_types_count; i++) {
                            if (strcmp(typename, local_types[i]) == 0) {
                                is_local = true;
                                break;
                            }
                        }

                        if (is_local) {
                            /* Qualify with current module */
                            pos += snprintf(result + pos, buf_size - pos, "%s.%s", module_name, typename);
                        } else {
                            /* Unknown type - might be from Basics or core modules that aren't explicitly imported */
                            /* Core types that need to be qualified */
                            const char *qualified = NULL;
                            if (strcmp(typename, "Task") == 0) {
                                qualified = "Task.Task";
                            } else if (strcmp(typename, "Cmd") == 0) {
                                qualified = "Platform.Cmd.Cmd";
                            } else if (strcmp(typename, "Sub") == 0) {
                                qualified = "Platform.Sub.Sub";
                            } else if (strcmp(typename, "Maybe") == 0) {
                                qualified = "Maybe.Maybe";
                            } else if (strcmp(typename, "List") == 0) {
                                qualified = "List.List";
                            } else if (strcmp(typename, "Result") == 0) {
                                qualified = "Result.Result";
                            } else if (strcmp(typename, "Dict") == 0) {
                                qualified = "Dict.Dict";
                            } else if (strcmp(typename, "Set") == 0) {
                                qualified = "Set.Set";
                            } else if (strcmp(typename, "Array") == 0) {
                                qualified = "Array.Array";
                            } else if (strcmp(typename, "Never") == 0) {
                                qualified = "Basics.Never";
                            } else if (strcmp(typename, "Int") == 0) {
                                qualified = "Basics.Int";
                            } else if (strcmp(typename, "Float") == 0) {
                                qualified = "Basics.Float";
                            } else if (strcmp(typename, "String") == 0) {
                                qualified = "String.String";
                            } else if (strcmp(typename, "Bool") == 0) {
                                qualified = "Basics.Bool";
                            } else if (strcmp(typename, "Order") == 0) {
                                qualified = "Basics.Order";
                            } else if (strcmp(typename, "Char") == 0) {
                                qualified = "Char.Char";
                            } else if (strcmp(typename, "Decoder") == 0) {
                                qualified = "Json.Decode.Decoder";
                            } else if (strcmp(typename, "Encoder") == 0) {
                                qualified = "Json.Encode.Encoder";
                            } else if (strcmp(typename, "Value") == 0) {
                                qualified = "Json.Encode.Value";
                            } else if (strcmp(typename, "Html") == 0) {
                                qualified = "Html.Html";
                            } else if (strcmp(typename, "Attribute") == 0) {
                                qualified = "Html.Attribute";
                            }

                            if (qualified) {
                                size_t qlen = strlen(qualified);
                                memcpy(result + pos, qualified, qlen);
                                pos += qlen;
                            } else {
                                /* Keep as-is - might be a type variable */
                                memcpy(result + pos, start, len);
                                pos += len;
                            }
                        }
                    }
                }
            }
        } else {
            result[pos++] = *p++;
        }
    }
    result[pos] = '\0';
    return result;
}

/* Helper function to extract and canonicalize type expression */
static char *extract_type_expression(TSNode type_node, const char *source_code, const char *module_name,
                                       ImportMap *import_map, ModuleAliasMap *alias_map,
                                       char **local_types, int local_types_count) {
    if (ts_node_is_null(type_node)) {
        return arena_strdup("");
    }

    /* Extract the raw type text */
    char *raw_type = get_node_text(type_node, source_code);

    /* Normalize whitespace first */
    char *normalized = normalize_whitespace(raw_type);
    arena_free(raw_type);

    /* Qualify type names */
    char *qualified = qualify_type_names(normalized, module_name, import_map, alias_map, local_types, local_types_count);
    arena_free(normalized);

    return qualified;
}

/* Extract value declaration (function/constant) */
static bool extract_value_decl(TSNode node, const char *source_code, ElmValue *value, const char *module_name,
                                 ImportMap *import_map, ModuleAliasMap *alias_map,
                                 char **local_types, int local_types_count) {
    /* Find type_annotation sibling first */
    TSNode type_annotation = ts_node_prev_named_sibling(node);
    if (ts_node_is_null(type_annotation) ||
        strcmp(ts_node_type(type_annotation), "type_annotation") != 0) {
        /* No type annotation found - skip this value */
        return false;
    }

    /* Extract function name from value_declaration */
    char *func_name = NULL;
    uint32_t child_count = ts_node_child_count(node);
    for (uint32_t i = 0; i < child_count; i++) {
        TSNode child = ts_node_child(node, i);
        const char *child_type = ts_node_type(child);

        if (strcmp(child_type, "function_declaration_left") == 0) {
            /* Find the function name */
            uint32_t func_child_count = ts_node_child_count(child);
            for (uint32_t j = 0; j < func_child_count; j++) {
                TSNode func_child = ts_node_child(child, j);
                if (strcmp(ts_node_type(func_child), "lower_case_identifier") == 0) {
                    func_name = get_node_text(func_child, source_code);
                    break;
                }
            }
            break;
        }
    }

    if (!func_name) {
        return false;
    }

    /* Extract type from type_annotation */
    char *type_str = NULL;
    uint32_t ann_child_count = ts_node_child_count(type_annotation);
    for (uint32_t i = 0; i < ann_child_count; i++) {
        TSNode child = ts_node_child(type_annotation, i);
        const char *child_type = ts_node_type(child);

        if (strcmp(child_type, "type_expression") == 0) {
            type_str = extract_type_expression(child, source_code, module_name, import_map, alias_map, local_types, local_types_count);
            break;
        }
    }

    if (!type_str) {
        arena_free(func_name);
        return false;
    }

    /* Extract comment */
    char *comment = find_preceding_comment(type_annotation, ts_node_parent(node), source_code);

    value->name = func_name;
    value->type = type_str;
    value->comment = comment;

    return true;
}

/* Extract type alias */
static bool extract_type_alias(TSNode node, const char *source_code, ElmAlias *alias, const char *module_name,
                                 ImportMap *import_map, ModuleAliasMap *alias_map,
                                 char **local_types, int local_types_count) {
    char *alias_name = NULL;
    char *type_expr = NULL;
    char **args = NULL;
    int args_count = 0;

    uint32_t child_count = ts_node_child_count(node);
    for (uint32_t i = 0; i < child_count; i++) {
        TSNode child = ts_node_child(node, i);
        const char *child_type = ts_node_type(child);

        if (strcmp(child_type, "upper_case_identifier") == 0 && !alias_name) {
            alias_name = get_node_text(child, source_code);
        } else if (strcmp(child_type, "lower_type_name") == 0) {
            /* Type parameter */
            if (args_count == 0) {
                args = arena_malloc(8 * sizeof(char*));
            }
            args[args_count++] = get_node_text(child, source_code);
        } else if (strcmp(child_type, "type_expression") == 0) {
            type_expr = extract_type_expression(child, source_code, module_name, import_map, alias_map, local_types, local_types_count);
        }
    }

    if (!alias_name || !type_expr) {
        arena_free(alias_name);
        arena_free(type_expr);
        for (int i = 0; i < args_count; i++) arena_free(args[i]);
        arena_free(args);
        return false;
    }

    char *comment = find_preceding_comment(node, ts_node_parent(node), source_code);

    alias->name = alias_name;
    alias->args = args;
    alias->args_count = args_count;
    alias->type = type_expr;
    alias->comment = comment;

    return true;
}

/* Extract union type */
static bool extract_union_type(TSNode node, const char *source_code, ElmUnion *union_type, const char *module_name,
                                 ImportMap *import_map, ModuleAliasMap *alias_map,
                                 char **local_types, int local_types_count) {
    char *type_name = NULL;
    char **args = NULL;
    int args_count = 0;

    uint32_t child_count = ts_node_child_count(node);
    for (uint32_t i = 0; i < child_count; i++) {
        TSNode child = ts_node_child(node, i);
        const char *child_type = ts_node_type(child);

        if (strcmp(child_type, "upper_case_identifier") == 0 && !type_name) {
            type_name = get_node_text(child, source_code);
        } else if (strcmp(child_type, "lower_type_name") == 0) {
            /* Type parameter */
            if (args_count == 0) {
                args = arena_malloc(8 * sizeof(char*));
            }
            args[args_count++] = get_node_text(child, source_code);
        } else if (strcmp(child_type, "union_variant") == 0) {
            /* We have constructors - extract them */
            /* For now, we'll handle this later when processing the union_variant list */
        }
    }

    if (!type_name) {
        for (int i = 0; i < args_count; i++) arena_free(args[i]);
        arena_free(args);
        return false;
    }

    char *comment = find_preceding_comment(node, ts_node_parent(node), source_code);

    union_type->name = type_name;
    union_type->args = args;
    union_type->args_count = args_count;
    union_type->comment = comment;
    union_type->cases = NULL;
    union_type->cases_count = 0;

    /* Extract union constructors */
    int cases_capacity = 4;
    ElmUnionCase *cases = arena_malloc(cases_capacity * sizeof(ElmUnionCase));
    int cases_count = 0;

    for (uint32_t i = 0; i < child_count; i++) {
        TSNode child = ts_node_child(node, i);
        if (strcmp(ts_node_type(child), "union_variant") == 0) {
            if (cases_count >= cases_capacity) {
                cases_capacity *= 2;
                cases = arena_realloc(cases, cases_capacity * sizeof(ElmUnionCase));
            }

            /* Extract constructor name and arguments */
            char *constructor_name = NULL;
            char **arg_types = NULL;
            int arg_types_count = 0;

            uint32_t variant_child_count = ts_node_child_count(child);
            for (uint32_t j = 0; j < variant_child_count; j++) {
                TSNode variant_child = ts_node_child(child, j);
                const char *variant_child_type = ts_node_type(variant_child);

                if (strcmp(variant_child_type, "upper_case_identifier") == 0 && !constructor_name) {
                    constructor_name = get_node_text(variant_child, source_code);
                } else if (strcmp(variant_child_type, "type_expression") == 0 ||
                           strcmp(variant_child_type, "type_ref") == 0) {
                    /* Constructor argument (either wrapped in parens or standalone) */
                    if (arg_types_count == 0) {
                        arg_types = arena_malloc(8 * sizeof(char*));
                    }
                    arg_types[arg_types_count++] = extract_type_expression(variant_child, source_code, module_name,
                                                                             import_map, alias_map, local_types, local_types_count);
                }
            }

            if (constructor_name) {
                cases[cases_count].name = constructor_name;
                cases[cases_count].arg_types = arg_types;
                cases[cases_count].arg_types_count = arg_types_count;
                cases_count++;
            }
        }
    }

    union_type->cases = cases;
    union_type->cases_count = cases_count;

    return true;
}

/* Main parsing function */
bool parse_elm_file(const char *filepath, ElmModuleDocs *docs) {
    /* Initialize the docs structure */
    memset(docs, 0, sizeof(ElmModuleDocs));

    /* Read file content */
    char *source_code = read_file(filepath);
    if (!source_code) {
        return false;
    }

    /* Create parser */
    TSParser *parser = ts_parser_new();
    if (!parser) {
        arena_free(source_code);
        return false;
    }

    /* Set the Elm language */
    if (!ts_parser_set_language(parser, tree_sitter_elm())) {
        fprintf(stderr, "Error: Failed to set Elm language\n");
        ts_parser_delete(parser);
        arena_free(source_code);
        return false;
    }

    /* Parse the source code */
    TSTree *tree = ts_parser_parse_string(
        parser,
        NULL,
        source_code,
        strlen(source_code)
    );

    if (!tree) {
        fprintf(stderr, "Error: Failed to parse file %s\n", filepath);
        ts_parser_delete(parser);
        arena_free(source_code);
        return false;
    }

    /* Get the root node */
    TSNode root_node = ts_tree_root_node(tree);

    /* Extract module name and export list */
    ExportList exports;
    docs->name = extract_module_info(root_node, source_code, &exports);

    /* Parse imports and module aliases */
    ImportMap import_map;
    init_import_map(&import_map);
    ModuleAliasMap alias_map;
    init_module_alias_map(&alias_map);
    extract_imports(root_node, source_code, &import_map, &alias_map);

    /* Extract module-level comment (comes AFTER module declaration) */
    uint32_t child_count = ts_node_child_count(root_node);
    docs->comment = NULL;
    for (uint32_t i = 0; i < child_count; i++) {
        TSNode child = ts_node_child(root_node, i);
        if (strcmp(ts_node_type(child), "module_declaration") == 0) {
            /* Look for block_comment after the module declaration */
            if (i + 1 < child_count) {
                TSNode next = ts_node_child(root_node, i + 1);
                if (strcmp(ts_node_type(next), "block_comment") == 0) {
                    char *raw = get_node_text(next, source_code);
                    docs->comment = clean_comment(raw);
                    arena_free(raw);
                }
            }
            break;
        }
    }
    if (!docs->comment) {
        docs->comment = arena_strdup("");
    }

    /* Allocate dynamic arrays for declarations */
    int values_capacity = 16;
    int aliases_capacity = 8;
    int unions_capacity = 8;

    docs->values = arena_malloc(values_capacity * sizeof(ElmValue));
    docs->aliases = arena_malloc(aliases_capacity * sizeof(ElmAlias));
    docs->unions = arena_malloc(unions_capacity * sizeof(ElmUnion));
    docs->values_count = 0;
    docs->aliases_count = 0;
    docs->unions_count = 0;

    /* First pass: collect local type names */
    int local_types_capacity = 16;
    int local_types_count = 0;
    char **local_types = arena_malloc(local_types_capacity * sizeof(char*));

    for (uint32_t i = 0; i < child_count; i++) {
        TSNode child = ts_node_child(root_node, i);
        const char *type = ts_node_type(child);

        if (strcmp(type, "type_alias_declaration") == 0 || strcmp(type, "type_declaration") == 0) {
            /* Extract type name */
            uint32_t type_child_count = ts_node_child_count(child);
            for (uint32_t j = 0; j < type_child_count; j++) {
                TSNode type_child = ts_node_child(child, j);
                if (strcmp(ts_node_type(type_child), "upper_case_identifier") == 0) {
                    char *type_name = get_node_text(type_child, source_code);
                    if (local_types_count >= local_types_capacity) {
                        local_types_capacity *= 2;
                        local_types = arena_realloc(local_types, local_types_capacity * sizeof(char*));
                    }
                    local_types[local_types_count++] = type_name;
                    break;
                }
            }
        }
    }

    /* Second pass: Walk the tree to extract declarations */
    for (uint32_t i = 0; i < child_count; i++) {
        TSNode child = ts_node_child(root_node, i);
        const char *type = ts_node_type(child);

        if (strcmp(type, "value_declaration") == 0) {
            /* Found a function/value declaration */
            ElmValue value;
            if (extract_value_decl(child, source_code, &value, docs->name, &import_map, &alias_map, local_types, local_types_count)) {
                /* Only include if exported */
                if (is_exported_value(value.name, &exports)) {
                    if (docs->values_count >= values_capacity) {
                        values_capacity *= 2;
                        docs->values = arena_realloc(docs->values, values_capacity * sizeof(ElmValue));
                    }
                    docs->values[docs->values_count++] = value;
                } else {
                    /* Not exported, free the value */
                    arena_free(value.name);
                    arena_free(value.type);
                    arena_free(value.comment);
                }
            }
        } else if (strcmp(type, "type_alias_declaration") == 0) {
            /* Found a type alias */
            ElmAlias alias;
            if (extract_type_alias(child, source_code, &alias, docs->name, &import_map, &alias_map, local_types, local_types_count)) {
                /* Only include if exported */
                if (is_exported_type(alias.name, &exports)) {
                    if (docs->aliases_count >= aliases_capacity) {
                        aliases_capacity *= 2;
                        docs->aliases = arena_realloc(docs->aliases, aliases_capacity * sizeof(ElmAlias));
                    }
                    docs->aliases[docs->aliases_count++] = alias;
                } else {
                    /* Not exported, free the alias */
                    arena_free(alias.name);
                    arena_free(alias.comment);
                    for (int j = 0; j < alias.args_count; j++) {
                        arena_free(alias.args[j]);
                    }
                    arena_free(alias.args);
                    arena_free(alias.type);
                }
            }
        } else if (strcmp(type, "type_declaration") == 0) {
            /* Found a union type */
            ElmUnion union_type;
            if (extract_union_type(child, source_code, &union_type, docs->name, &import_map, &alias_map, local_types, local_types_count)) {
                /* Only include if exported */
                if (is_exported_type(union_type.name, &exports)) {
                    /* Check if constructors should be exposed */
                    if (!is_type_exposed_with_constructors(union_type.name, &exports)) {
                        /* Opaque type - clear constructors */
                        for (int j = 0; j < union_type.cases_count; j++) {
                            arena_free(union_type.cases[j].name);
                            for (int k = 0; k < union_type.cases[j].arg_types_count; k++) {
                                arena_free(union_type.cases[j].arg_types[k]);
                            }
                            arena_free(union_type.cases[j].arg_types);
                        }
                        arena_free(union_type.cases);
                        union_type.cases = NULL;
                        union_type.cases_count = 0;
                    }

                    if (docs->unions_count >= unions_capacity) {
                        unions_capacity *= 2;
                        docs->unions = arena_realloc(docs->unions, unions_capacity * sizeof(ElmUnion));
                    }
                    docs->unions[docs->unions_count++] = union_type;
                } else {
                    /* Not exported, free the union type */
                    arena_free(union_type.name);
                    arena_free(union_type.comment);
                    for (int j = 0; j < union_type.args_count; j++) {
                        arena_free(union_type.args[j]);
                    }
                    arena_free(union_type.args);
                    for (int j = 0; j < union_type.cases_count; j++) {
                        arena_free(union_type.cases[j].name);
                        for (int k = 0; k < union_type.cases[j].arg_types_count; k++) {
                            arena_free(union_type.cases[j].arg_types[k]);
                        }
                        arena_free(union_type.cases[j].arg_types);
                    }
                    arena_free(union_type.cases);
                }
            }
        }
    }

    /* Clean up local types list */
    for (int i = 0; i < local_types_count; i++) {
        arena_free(local_types[i]);
    }
    arena_free(local_types);

    /* Clean up import map and alias map */
    free_import_map(&import_map);
    free_module_alias_map(&alias_map);

    /* Sort declarations alphabetically by name */
    /* Sort values */
    for (int i = 0; i < docs->values_count - 1; i++) {
        for (int j = i + 1; j < docs->values_count; j++) {
            if (strcmp(docs->values[i].name, docs->values[j].name) > 0) {
                ElmValue temp = docs->values[i];
                docs->values[i] = docs->values[j];
                docs->values[j] = temp;
            }
        }
    }

    /* Sort aliases */
    for (int i = 0; i < docs->aliases_count - 1; i++) {
        for (int j = i + 1; j < docs->aliases_count; j++) {
            if (strcmp(docs->aliases[i].name, docs->aliases[j].name) > 0) {
                ElmAlias temp = docs->aliases[i];
                docs->aliases[i] = docs->aliases[j];
                docs->aliases[j] = temp;
            }
        }
    }

    /* Sort unions */
    for (int i = 0; i < docs->unions_count - 1; i++) {
        for (int j = i + 1; j < docs->unions_count; j++) {
            if (strcmp(docs->unions[i].name, docs->unions[j].name) > 0) {
                ElmUnion temp = docs->unions[i];
                docs->unions[i] = docs->unions[j];
                docs->unions[j] = temp;
            }
        }
    }

    /* Sort binops */
    for (int i = 0; i < docs->binops_count - 1; i++) {
        for (int j = i + 1; j < docs->binops_count; j++) {
            if (strcmp(docs->binops[i].name, docs->binops[j].name) > 0) {
                ElmBinop temp = docs->binops[i];
                docs->binops[i] = docs->binops[j];
                docs->binops[j] = temp;
            }
        }
    }

    fprintf(stderr, "Successfully parsed: %s (Module: %s, %d values, %d aliases, %d unions)\n",
            filepath, docs->name, docs->values_count, docs->aliases_count, docs->unions_count);

    /* Clean up export list */
    free_export_list(&exports);

    /* Cleanup */
    ts_tree_delete(tree);
    ts_parser_delete(parser);
    arena_free(source_code);

    return true;
}

/* Free documentation structure */
void free_elm_docs(ElmModuleDocs *docs) {
    if (!docs) return;

    arena_free(docs->name);
    arena_free(docs->comment);

    for (int i = 0; i < docs->values_count; i++) {
        arena_free(docs->values[i].name);
        arena_free(docs->values[i].comment);
        arena_free(docs->values[i].type);
    }
    arena_free(docs->values);

    for (int i = 0; i < docs->aliases_count; i++) {
        arena_free(docs->aliases[i].name);
        arena_free(docs->aliases[i].comment);
        for (int j = 0; j < docs->aliases[i].args_count; j++) {
            arena_free(docs->aliases[i].args[j]);
        }
        arena_free(docs->aliases[i].args);
        arena_free(docs->aliases[i].type);
    }
    arena_free(docs->aliases);

    for (int i = 0; i < docs->unions_count; i++) {
        arena_free(docs->unions[i].name);
        arena_free(docs->unions[i].comment);
        for (int j = 0; j < docs->unions[i].args_count; j++) {
            arena_free(docs->unions[i].args[j]);
        }
        arena_free(docs->unions[i].args);
        for (int j = 0; j < docs->unions[i].cases_count; j++) {
            arena_free(docs->unions[i].cases[j].name);
            for (int k = 0; k < docs->unions[i].cases[j].arg_types_count; k++) {
                arena_free(docs->unions[i].cases[j].arg_types[k]);
            }
            arena_free(docs->unions[i].cases[j].arg_types);
        }
        arena_free(docs->unions[i].cases);
    }
    arena_free(docs->unions);

    for (int i = 0; i < docs->binops_count; i++) {
        arena_free(docs->binops[i].name);
        arena_free(docs->binops[i].comment);
        arena_free(docs->binops[i].type);
        arena_free(docs->binops[i].associativity);
    }
    arena_free(docs->binops);
}

/* Helper function to escape JSON string */
static void print_json_string(const char *str) {
    if (!str) {
        printf("\"\"");
        return;
    }

    printf("\"");
    for (const char *p = str; *p; p++) {
        switch (*p) {
            case '"':  printf("\\\""); break;
            case '\\': printf("\\\\"); break;
            case '\b': printf("\\b"); break;
            case '\f': printf("\\f"); break;
            case '\n': printf("\\n"); break;
            case '\r': printf("\\r"); break;
            case '\t': printf("\\t"); break;
            default:
                if ((unsigned char)*p < 32) {
                    printf("\\u%04x", *p);
                } else {
                    putchar(*p);
                }
        }
    }
    printf("\"");
}

/* Print docs to JSON stdout */
void print_docs_json(ElmModuleDocs *docs, int docs_count) {
    printf("[\n");

    for (int i = 0; i < docs_count; i++) {
        printf("  {\n");

        /* Module name */
        printf("    \"name\": ");
        print_json_string(docs[i].name);
        printf(",\n");

        /* Module comment */
        printf("    \"comment\": ");
        print_json_string(docs[i].comment);
        printf(",\n");

        /* Unions */
        printf("    \"unions\": [");
        for (int j = 0; j < docs[i].unions_count; j++) {
            ElmUnion *u = &docs[i].unions[j];
            printf("%s\n      {", j > 0 ? "," : "");
            printf("\n        \"name\": ");
            print_json_string(u->name);
            printf(",\n        \"comment\": ");
            print_json_string(u->comment);
            printf(",\n        \"args\": [");
            for (int k = 0; k < u->args_count; k++) {
                if (k > 0) printf(", ");
                print_json_string(u->args[k]);
            }
            printf("],\n        \"cases\": [");
            for (int k = 0; k < u->cases_count; k++) {
                if (k > 0) printf(", ");
                printf("[");
                print_json_string(u->cases[k].name);
                printf(", [");
                for (int m = 0; m < u->cases[k].arg_types_count; m++) {
                    if (m > 0) printf(", ");
                    print_json_string(u->cases[k].arg_types[m]);
                }
                printf("]]");
            }
            printf("]\n      }");
        }
        if (docs[i].unions_count > 0) {
            printf("\n    ");
        }
        printf("],\n");

        /* Aliases */
        printf("    \"aliases\": [");
        for (int j = 0; j < docs[i].aliases_count; j++) {
            ElmAlias *a = &docs[i].aliases[j];
            printf("%s\n      {", j > 0 ? "," : "");
            printf("\n        \"name\": ");
            print_json_string(a->name);
            printf(",\n        \"comment\": ");
            print_json_string(a->comment);
            printf(",\n        \"args\": [");
            for (int k = 0; k < a->args_count; k++) {
                if (k > 0) printf(", ");
                print_json_string(a->args[k]);
            }
            printf("],");
            printf("\n        \"type\": ");
            print_json_string(a->type);
            printf("\n      }");
        }
        if (docs[i].aliases_count > 0) {
            printf("\n    ");
        }
        printf("],\n");

        /* Values */
        printf("    \"values\": [");
        for (int j = 0; j < docs[i].values_count; j++) {
            ElmValue *v = &docs[i].values[j];
            printf("%s\n      {", j > 0 ? "," : "");
            printf("\n        \"name\": ");
            print_json_string(v->name);
            printf(",\n        \"comment\": ");
            print_json_string(v->comment);
            printf(",\n        \"type\": ");
            print_json_string(v->type);
            printf("\n      }");
        }
        if (docs[i].values_count > 0) {
            printf("\n    ");
        }
        printf("],\n");

        /* Binops */
        printf("    \"binops\": [");
        for (int j = 0; j < docs[i].binops_count; j++) {
            ElmBinop *b = &docs[i].binops[j];
            printf("%s\n      {", j > 0 ? "," : "");
            printf("\n        \"name\": ");
            print_json_string(b->name);
            printf(",\n        \"comment\": ");
            print_json_string(b->comment);
            printf(",\n        \"type\": ");
            print_json_string(b->type);
            printf(",\n        \"associativity\": ");
            print_json_string(b->associativity);
            printf(",\n        \"precedence\": %d", b->precedence);
            printf("\n      }");
        }
        if (docs[i].binops_count > 0) {
            printf("\n    ");
        }
        printf("]\n");

        printf("  }%s\n", i < docs_count - 1 ? "," : "");
    }

    printf("]\n");
}
