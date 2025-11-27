#include "type_qualify.h"
#include "tree_util.h"
#include "../../../alloc.h"
#include <stdio.h>
#include <string.h>

/* Helper function to normalize whitespace - convert newlines and multiple spaces to single spaces */
char *normalize_whitespace(const char *str) {
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

    /* Remove spaces before commas, ensure spaces around colons in record types, */
    /* and handle spaces before closing parens based on tuple vs function type context */
    char *final = arena_malloc(pos * 3 + 1);  /* Extra space for potential additions */
    size_t final_pos = 0;

    /* First pass: mark which paren pairs contain commas (tuples) */
    /* We match each opening paren with its closing paren */
    bool *is_tuple_paren = arena_malloc(pos * sizeof(bool));
    int *paren_match = arena_malloc(pos * sizeof(int));  /* For each '(', store index of matching ')' */
    bool *has_comma = arena_malloc(pos * sizeof(bool));   /* For each position, whether it contains a comma */

    /* Initialize arrays */
    for (size_t i = 0; i < pos; i++) {
        is_tuple_paren[i] = false;
        paren_match[i] = -1;
        has_comma[i] = false;
    }

    /* Build a stack to match parens and track commas */
    int *paren_stack = arena_malloc(pos * sizeof(int));
    int *brace_depth_stack = arena_malloc(pos * sizeof(int));  /* Track brace depth at each paren level */
    int stack_top = -1;
    int brace_depth = 0;

    for (size_t i = 0; i < pos; i++) {
        if (result[i] == '{') {
            brace_depth++;
        } else if (result[i] == '}') {
            if (brace_depth > 0) brace_depth--;
        } else if (result[i] == '(') {
            stack_top++;
            paren_stack[stack_top] = i;
            brace_depth_stack[stack_top] = brace_depth;
            has_comma[i] = false;  /* This paren pair hasn't seen a comma yet */
        } else if (result[i] == ',') {
            /* Mark that the current paren level has a comma, but only if not inside braces */
            if (stack_top >= 0 && brace_depth == brace_depth_stack[stack_top]) {
                has_comma[paren_stack[stack_top]] = true;
            }
        } else if (result[i] == ')') {
            if (stack_top >= 0) {
                int open_idx = paren_stack[stack_top];
                paren_match[open_idx] = i;
                /* If this paren pair has a comma, mark both as tuple parens */
                if (has_comma[open_idx]) {
                    is_tuple_paren[open_idx] = true;
                    is_tuple_paren[i] = true;
                }
                stack_top--;
            }
        }
    }

    arena_free(paren_stack);
    arena_free(brace_depth_stack);
    arena_free(has_comma);

    /* Track which closing parens to skip (for removing redundant parens in record fields) */
    bool *skip_paren = arena_malloc(pos * sizeof(bool));
    for (size_t i = 0; i < pos; i++) {
        skip_paren[i] = false;
    }

    /* Track brace depth for detecting record fields */
    int current_brace_depth = 0;

    /* Second pass: build final string */
    for (size_t i = 0; i < pos; i++) {
        if (result[i] == '{') {
            current_brace_depth++;
            final[final_pos++] = result[i];
            /* Ensure space after opening brace unless followed by } */
            if (i + 1 < pos && result[i + 1] != '}') {
                /* Skip any existing space */
                if (i + 1 < pos && result[i + 1] == ' ') {
                    i++;
                }
                /* Add exactly one space */
                final[final_pos++] = ' ';
            }
        } else if (result[i] == '}') {
            current_brace_depth--;
            /* Ensure space before closing brace in record types, except for empty records {} */
            if (final_pos > 0 && final[final_pos - 1] != ' ' && final[final_pos - 1] != '{') {
                final[final_pos++] = ' ';
            }
            final[final_pos++] = result[i];
        } else if (result[i] == ' ' && i + 1 < pos && result[i + 1] == '}') {
            /* Keep space before closing brace, unless it's an empty record */
            if (final_pos > 0 && final[final_pos - 1] != '{') {
                final[final_pos++] = result[i];
            }
        } else if (result[i] == ' ' && i + 1 < pos && result[i + 1] == ',') {
            /* Skip space before comma */
            continue;
        } else if (result[i] == ',') {
            /* Add comma and ensure space after it */
            final[final_pos++] = result[i];
            if (i + 1 < pos && result[i + 1] != ' ') {
                final[final_pos++] = ' ';
            }
        } else if (result[i] == ' ' && i + 1 < pos && result[i + 1] == ')') {
            /* Keep space before closing paren only if it's a tuple */
            if (!is_tuple_paren[i + 1]) {
                continue;  /* Skip space before non-tuple closing paren */
            }
            final[final_pos++] = result[i];
        } else if (result[i] == ')') {
            /* Skip if marked for removal (redundant paren in record field) */
            if (skip_paren[i]) {
                /* Skip any space before this closing paren */
                if (final_pos > 0 && final[final_pos - 1] == ' ') {
                    final_pos--;
                }
                continue;
            }
            /* Handle closing paren */
            if (is_tuple_paren[i]) {
                /* Ensure space before closing paren in tuples */
                if (final_pos > 0 && final[final_pos - 1] != ' ') {
                    final[final_pos++] = ' ';
                }
            }
            final[final_pos++] = result[i];
            /* Ensure space after closing paren if followed by -> */
            if (i + 1 < pos && result[i + 1] == '-' && i + 2 < pos && result[i + 2] == '>') {
                /* No space before ->, add one */
                final[final_pos++] = ' ';
            } else if (i + 1 < pos && result[i + 1] == ' ' &&
                       i + 2 < pos && result[i + 2] == '-' &&
                       i + 3 < pos && result[i + 3] == '>') {
                /* Already has space before ->, keep it (will be added in next iteration) */
            }
        } else if (result[i] == '(' && is_tuple_paren[i]) {
            /* Opening paren of a tuple */
            final[final_pos++] = result[i];
            /* Ensure space after opening paren in tuples */
            if (i + 1 < pos && result[i + 1] != ' ') {
                final[final_pos++] = ' ';
            }
        } else if (result[i] == '(' && !is_tuple_paren[i]) {
            /* Opening paren of a non-tuple (function type, parenthesized type) */
            /* Check if we're in a record field (after : and before }) and this paren wraps a function type */
            if (current_brace_depth > 0 && final_pos >= 2 &&
                final[final_pos - 1] == ' ' && final[final_pos - 2] == ':') {
                /* Look ahead to see if this paren wraps a function type */
                /* Find the matching closing paren */
                int paren_depth = 1;
                size_t j = i + 1;
                bool has_arrow = false;
                while (j < pos && paren_depth > 0) {
                    if (result[j] == '(') paren_depth++;
                    else if (result[j] == ')') paren_depth--;
                    else if (paren_depth == 1 && result[j] == '-' && j + 1 < pos && result[j + 1] == '>') {
                        has_arrow = true;
                    }
                    if (paren_depth > 0) j++;
                }
                /* If the paren wraps a function type, check if it's followed by an arrow */
                if (has_arrow && paren_depth == 0) {
                    /* Check if there's a " -> " after the closing paren */
                    /* This indicates the function type is a parameter, not the return type */
                    /* Example: (Int -> String) -> Bool  -- parens are necessary */
                    /*          Int -> (String -> Bool) -- parens are redundant */
                    bool followed_by_arrow = false;
                    if (j + 1 < pos && result[j + 1] == ' ' &&
                        j + 2 < pos && result[j + 2] == '-' &&
                        j + 3 < pos && result[j + 3] == '>') {
                        followed_by_arrow = true;
                    } else if (j + 1 < pos && result[j + 1] == '-' &&
                               j + 2 < pos && result[j + 2] == '>') {
                        followed_by_arrow = true;
                    }

                    /* Only skip parens if NOT followed by an arrow */
                    if (!followed_by_arrow) {
                        /* Mark the closing paren at position j for skipping */
                        skip_paren[j] = true;
                        /* Skip this opening paren and continue */
                        continue;
                    }
                }
            }
            final[final_pos++] = result[i];
            /* Skip any space after opening paren in non-tuples */
            /* This handles the case where type substitution or other processing
             * may have introduced unwanted spaces */
        } else if (result[i] == ' ' && i > 0 && result[i - 1] == '(' && !is_tuple_paren[i - 1]) {
            /* Skip space after non-tuple opening paren */
            continue;
        } else if (result[i] == ':') {
            /* Ensure space before colon if not present */
            if (final_pos > 0 && final[final_pos - 1] != ' ') {
                final[final_pos++] = ' ';
            }
            final[final_pos++] = ':';
            /* Ensure space after colon if not already present */
            if (i + 1 < pos && result[i + 1] != ' ') {
                final[final_pos++] = ' ';
            }
        } else if (result[i] == '-' && i + 1 < pos && result[i + 1] == '>') {
            /* Ensure space before -> if not present */
            if (final_pos > 0 && final[final_pos - 1] != ' ') {
                final[final_pos++] = ' ';
            }
            final[final_pos++] = result[i];
            final[final_pos++] = result[++i];  /* Add the '>' */
            /* Ensure space after -> if not already present */
            if (i + 1 < pos && result[i + 1] != ' ') {
                final[final_pos++] = ' ';
            }
        } else {
            final[final_pos++] = result[i];
        }
    }
    final[final_pos] = '\0';

    arena_free(is_tuple_paren);
    arena_free(paren_match);
    arena_free(skip_paren);
    arena_free(result);

    return final;
}

/* Helper function to parse a single type argument from a string
 * Returns the end position of the argument, or NULL if no valid argument found
 * Handles: simple identifiers, qualified types, parenthesized types, record types, tuple types */
const char *parse_type_arg(const char *start, char *out_arg, size_t out_size) {
    const char *p = start;
    
    /* Skip leading whitespace */
    while (*p == ' ') p++;
    
    if (!*p) return NULL;
    
    const char *arg_start = p;
    const char *arg_end = p;
    int depth = 0;
    
    if (*p == '(') {
        /* Parenthesized type or tuple */
        depth = 1;
        arg_end++;
        while (*arg_end && depth > 0) {
            if (*arg_end == '(') depth++;
            else if (*arg_end == ')') depth--;
            arg_end++;
        }
    } else if (*p == '{') {
        /* Record type */
        depth = 1;
        arg_end++;
        while (*arg_end && depth > 0) {
            if (*arg_end == '{') depth++;
            else if (*arg_end == '}') depth--;
            arg_end++;
        }
    } else if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) {
        /* Type name, qualified type, or type variable */
        while (*arg_end && ((*arg_end >= 'A' && *arg_end <= 'Z') ||
                           (*arg_end >= 'a' && *arg_end <= 'z') ||
                           (*arg_end >= '0' && *arg_end <= '9') ||
                           *arg_end == '_' || *arg_end == '.')) {
            arg_end++;
        }
        /* Check if this type has type arguments itself (e.g., "List Int") */
        /* Only consume simple type names here; nested type applications are parenthesized */
    } else {
        return NULL;  /* Not a valid type start */
    }
    
    size_t arg_len = arg_end - arg_start;
    if (arg_len == 0 || arg_len >= out_size) return NULL;
    
    memcpy(out_arg, arg_start, arg_len);
    out_arg[arg_len] = '\0';
    
    return arg_end;
}

/* Helper function to substitute type variables with type arguments in an expansion string */
char *substitute_type_vars(const char *expansion, char **type_vars, int type_vars_count,
                           char **type_args, int type_args_count) {
    if (type_vars_count == 0 || type_args_count == 0) {
        return arena_strdup(expansion);
    }
    
    /* Use the minimum count to avoid out-of-bounds access */
    int subst_count = type_vars_count < type_args_count ? type_vars_count : type_args_count;
    
    /* Calculate maximum possible size */
    size_t max_arg_len = 0;
    for (int i = 0; i < type_args_count; i++) {
        size_t len = strlen(type_args[i]);
        if (len > max_arg_len) max_arg_len = len;
    }
    
    size_t new_size = strlen(expansion) * 2 + max_arg_len * 20 + 1024;
    char *result = arena_malloc(new_size);
    size_t pos = 0;
    const char *p = expansion;
    
    while (*p && pos < new_size - max_arg_len - 10) {
        /* Check if this is an identifier (potential type variable) */
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')) {
            const char *id_start = p;
            while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                   (*p >= '0' && *p <= '9') || *p == '_') {
                p++;
            }
            size_t id_len = p - id_start;
            
            /* Check if this identifier matches any type variable */
            bool substituted = false;
            for (int i = 0; i < subst_count; i++) {
                size_t var_len = strlen(type_vars[i]);
                if (id_len == var_len && memcmp(id_start, type_vars[i], var_len) == 0) {
                    /* Substitute with the corresponding type argument */
                    size_t arg_len = strlen(type_args[i]);
                    memcpy(result + pos, type_args[i], arg_len);
                    pos += arg_len;
                    substituted = true;
                    break;
                }
            }
            
            if (!substituted) {
                /* Not a type variable - copy as-is */
                memcpy(result + pos, id_start, id_len);
                pos += id_len;
            }
        } else {
            result[pos++] = *p++;
        }
    }
    
    result[pos] = '\0';
    return result;
}

/* Helper function to check if a string contains a function arrow */
bool contains_function_arrow(const char *str) {
    /* Look for " -> " pattern (with spaces) */
    const char *p = str;
    while (*p && *(p + 1)) {  /* Ensure we have at least 2 chars ahead */
        if (*p == '-' && *(p + 1) == '>' &&
            (p > str && *(p - 1) == ' ') &&
            *(p + 2) && *(p + 2) == ' ') {  /* Check *(p + 2) exists before dereferencing */
            return true;
        }
        p++;
    }
    return false;
}

/* Helper function to expand type aliases that are function types
 * Only expands the final return type, not parameter types
 * Only expands if implementation has more params than type arrows suggest */
char *expand_function_type_aliases(const char *type_str, TypeAliasMap *type_alias_map, int implementation_param_count) {
    if (!type_str || !type_alias_map) {
        return arena_strdup(type_str ? type_str : "");
    }

    /* Count arrows in the type to determine expected parameter count */
    int arrow_count = count_type_arrows(type_str);

    /* Only expand if implementation has more parameters than the type suggests */
    /* This means the return type alias is being "called" with the extra parameters */
    if (implementation_param_count <= arrow_count) {
        return arena_strdup(type_str);
    }

    /* Find the last occurrence of " -> " (function arrow) */
    const char *last_arrow = NULL;
    const char *p = type_str;
    int paren_depth = 0;

    while (*p) {
        if (*p == '(') {
            paren_depth++;
        } else if (*p == ')') {
            paren_depth--;
        } else if (paren_depth == 0 && *p == '-' && *(p + 1) == '>' &&
                   (p > type_str && *(p - 1) == ' ') && *(p + 2) == ' ') {
            last_arrow = p;
        }
        p++;
    }

    /* If there's no arrow, expand the entire type */
    /* If there's an arrow, only expand the part after the last arrow */
    const char *expand_start = last_arrow ? last_arrow + 3 : type_str;  /* Skip " -> " */
    while (*expand_start == ' ') expand_start++;  /* Skip leading spaces */

    /* Extract the return type */
    size_t prefix_len = expand_start - type_str;

    /* Check if the return type is a type alias */
    const char *return_type_start = expand_start;
    const char *rt = return_type_start;

    /* Skip to the first uppercase letter (start of type name) */
    while (*rt && !((*rt >= 'A' && *rt <= 'Z'))) {
        rt++;
    }

    if (*rt >= 'A' && *rt <= 'Z') {
        /* Extract the type name */
        const char *type_name_start = rt;
        while ((*rt >= 'A' && *rt <= 'Z') || (*rt >= 'a' && *rt <= 'z') ||
               (*rt >= '0' && *rt <= '9') || *rt == '_' || *rt == '.') {
            rt++;
        }

        size_t type_name_len = rt - type_name_start;
        char type_name[256];
        if (type_name_len < sizeof(type_name)) {
            memcpy(type_name, type_name_start, type_name_len);
            type_name[type_name_len] = '\0';

            /* Skip module qualifiers - only look at the last part */
            char *last_dot = strrchr(type_name, '.');
            const char *simple_name = last_dot ? last_dot + 1 : type_name;

            /* Look up the type alias */
            const TypeAliasExpansion *alias = lookup_type_alias(type_alias_map, simple_name);

            if (alias && contains_function_arrow(alias->expansion)) {
                /* This is a function type alias - expand it */

                /* Parse type arguments from the return type if present */
                /* For example: "Decoder Bool" -> type arg is "Bool" */
                /*              "Result String Int" -> type args are "String", "Int" */
                const char *type_args_pos = rt;
                
                /* Collect all type arguments */
                char **type_args = arena_malloc(8 * sizeof(char*));
                int type_args_count = 0;
                int type_args_capacity = 8;
                
                while (*type_args_pos && type_args_count < alias->type_vars_count) {
                    char arg_buf[512];
                    const char *next_pos = parse_type_arg(type_args_pos, arg_buf, sizeof(arg_buf));
                    if (!next_pos) break;
                    
                    if (type_args_count >= type_args_capacity) {
                        type_args_capacity *= 2;
                        type_args = arena_realloc(type_args, type_args_capacity * sizeof(char*));
                    }
                    type_args[type_args_count++] = arena_strdup(arg_buf);
                    type_args_pos = next_pos;
                }

                /* Substitute type variables in the expansion */
                char *expanded = substitute_type_vars(alias->expansion, alias->type_vars, 
                                                       alias->type_vars_count, type_args, type_args_count);

                /* Free type args */
                for (int i = 0; i < type_args_count; i++) {
                    arena_free(type_args[i]);
                }
                arena_free(type_args);

                size_t buf_size = prefix_len + strlen(expanded) + 10;
                char *result = arena_malloc(buf_size);

                /* Copy the prefix (everything before the return type) */
                memcpy(result, type_str, prefix_len);

                /* Copy the expanded type */
                strcpy(result + prefix_len, expanded);

                arena_free(expanded);

                return result;
            }
        }
    }

    /* No expansion needed - return a copy of the original */
    return arena_strdup(type_str);
}

/* Helper function to check if parentheses around a type component are necessary.
 * Parens are necessary if:
 * 1. The content contains a function arrow " -> " at the top level (function type as argument)
 * 2. The content contains a comma at the top level (tuple)
 * 3. The content is empty (unit type ())
 * Returns true if parens are necessary, false if they can be removed. */
static bool parens_are_necessary(const char *inner, size_t inner_len) {
    /* Empty parens = unit type (), must keep */
    if (inner_len == 0) {
        return true;
    }

    int paren_depth = 0;
    int brace_depth = 0;

    for (size_t i = 0; i < inner_len; i++) {
        char c = inner[i];

        if (c == '(') {
            paren_depth++;
        } else if (c == ')') {
            paren_depth--;
        } else if (c == '{') {
            brace_depth++;
        } else if (c == '}') {
            brace_depth--;
        } else if (paren_depth == 0 && brace_depth == 0) {
            /* Check for comma at top level (tuple) */
            if (c == ',') {
                return true;
            }
            /* Check for function arrow at top level */
            if (c == '-' && i + 1 < inner_len && inner[i + 1] == '>') {
                /* Verify it's a proper arrow with surrounding spaces */
                bool has_space_before = (i > 0 && inner[i - 1] == ' ');
                bool has_space_after = (i + 2 < inner_len && inner[i + 2] == ' ');
                if (has_space_before && has_space_after) {
                    return true;  /* Function type - parens needed */
                }
            }
        }
    }

    return false;  /* No arrow or comma at top level - parens not needed */
}

/* Helper function to remove unnecessary parentheses from function argument positions.
 * For example: "a -> (Maybe.Maybe b) -> Result.Result a ()" 
 *    becomes:  "a -> Maybe.Maybe b -> Result.Result a ()"
 * 
 * This handles cases where type alias expansion introduces parens that are no longer
 * needed in the function type context.
 * 
 * IMPORTANT: This only removes parens that wrap an ENTIRE function argument component
 * (i.e., the part between " -> " arrows). It does NOT remove parens that are arguments
 * to type constructors like `Field a b (Html (Msg x))` - those parens are necessary.
 * 
 * Parens are kept when they:
 * 1. Contain a function arrow (grouping a function type as an argument)
 * 2. Contain a comma (tuple)
 * 3. Are empty (unit type)
 */
char *remove_unnecessary_arg_parens(const char *type_str) {
    if (!type_str || !*type_str) {
        return arena_strdup(type_str ? type_str : "");
    }

    /* Strategy: Split by top-level " -> ", process each component, rejoin.
     * A component's outer parens are unnecessary only if:
     * 1. They wrap the ENTIRE component (not just part of it)
     * 2. The inner content doesn't contain top-level " -> " or ","
     * 3. The inner content is not empty
     */

    size_t len = strlen(type_str);
    
    /* First, find all top-level " -> " positions */
    const char *arrows[64];  /* Max 64 arrows should be plenty */
    int arrow_count = 0;
    
    const char *p = type_str;
    int paren_depth = 0;
    int brace_depth = 0;
    
    while (*p && arrow_count < 64) {
        if (*p == '(') paren_depth++;
        else if (*p == ')') paren_depth--;
        else if (*p == '{') brace_depth++;
        else if (*p == '}') brace_depth--;
        else if (paren_depth == 0 && brace_depth == 0 &&
                 *p == ' ' && *(p+1) == '-' && *(p+2) == '>' && *(p+3) == ' ') {
            arrows[arrow_count++] = p;
            p += 3;  /* Skip past " ->" , the loop will advance past the trailing space */
        }
        p++;
    }
    
    /* If no arrows, nothing to process for function arguments */
    if (arrow_count == 0) {
        return arena_strdup(type_str);
    }
    
    /* Build result by processing each component */
    char *result = arena_malloc(len + 1);
    size_t result_pos = 0;
    
    const char *comp_start = type_str;
    
    for (int i = 0; i <= arrow_count; i++) {
        const char *comp_end = (i < arrow_count) ? arrows[i] : (type_str + len);
        size_t comp_len = comp_end - comp_start;
        
        /* Skip leading spaces */
        while (comp_len > 0 && *comp_start == ' ') {
            comp_start++;
            comp_len--;
        }
        /* Skip trailing spaces */
        while (comp_len > 0 && *(comp_start + comp_len - 1) == ' ') {
            comp_len--;
        }
        
        /* Check if this component is wrapped in parens that cover the entire component */
        bool should_unwrap = false;
        if (comp_len >= 2 && *comp_start == '(') {
            /* Check if the matching ')' is at the end */
            int depth = 1;
            const char *scan = comp_start + 1;
            const char *comp_limit = comp_start + comp_len;
            
            while (scan < comp_limit && depth > 0) {
                if (*scan == '(') depth++;
                else if (*scan == ')') depth--;
                scan++;
            }
            
            /* If depth reached 0 exactly at the end, parens wrap entire component */
            if (depth == 0 && scan == comp_limit) {
                /* Check if inner content requires parens */
                const char *inner = comp_start + 1;
                size_t inner_len = comp_len - 2;  /* Exclude both parens */
                
                if (!parens_are_necessary(inner, inner_len)) {
                    should_unwrap = true;
                }
            }
        }
        
        /* Copy component (unwrapped if applicable) */
        if (should_unwrap) {
            memcpy(result + result_pos, comp_start + 1, comp_len - 2);
            result_pos += comp_len - 2;
        } else {
            memcpy(result + result_pos, comp_start, comp_len);
            result_pos += comp_len;
        }
        
        /* Add " -> " separator if not the last component */
        if (i < arrow_count) {
            memcpy(result + result_pos, " -> ", 4);
            result_pos += 4;
            comp_start = arrows[i] + 4;  /* Skip past " -> " */
        }
    }
    
    result[result_pos] = '\0';
    
    return result;
}

/* Helper function to qualify type names based on import map and local types */
char *qualify_type_names(const char *type_str, const char *module_name,
                         ImportMap *import_map, ModuleAliasMap *alias_map,
                         DirectModuleImports *direct_imports,
                         char **local_types, int local_types_count,
                         DependencyCache *dep_cache) {
    size_t buf_size = strlen(type_str) * 3 + 1024;  /* Extra space for qualifications */
    char *result = arena_malloc(buf_size);
    size_t pos = 0;
    const char *p = type_str;

    while (*p && pos < buf_size - 200) {
        if (*p >= 'A' && *p <= 'Z') {
            /* Check if this uppercase letter is part of a camelCase identifier */
            /* by checking if the previous character was alphanumeric */
            bool is_camel_case = false;
            if (p > type_str) {
                char prev = *(p - 1);
                if ((prev >= 'a' && prev <= 'z') || (prev >= '0' && prev <= '9') || prev == '_') {
                    is_camel_case = true;
                }
            }

            if (is_camel_case) {
                /* Part of camelCase identifier - copy as-is */
                result[pos++] = *p++;
            } else {
                /* Found an uppercase identifier - might be a type */
                const char *start = p;
                while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                       (*p >= '0' && *p <= '9') || *p == '_') {
                    p++;
                }
                size_t len = p - start;
                char typename_buf[256];
                if (len < sizeof(typename_buf)) {
                    memcpy(typename_buf, start, len);
                    typename_buf[len] = '\0';

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
                        /* This is a module prefix - check if it's an alias that should be expanded.
                         * Since aliased modules are NOT added to direct_imports (the original name
                         * is unavailable in Elm when aliased), we expand whenever we find an alias.
                         * The is_directly_imported check handles the rare case where a module is
                         * imported both directly AND via an alias (two separate import statements). */

                        /* Extract the type name after the dot for ambiguous alias resolution */
                        const char *referenced_type = NULL;
                        char type_after_dot[256];
                        if (*p == '.') {
                            const char *after_dot = p + 1;
                            /* Skip to the next uppercase letter (handle cases like "C . Dot") */
                            while (*after_dot && (*after_dot == ' ' || *after_dot == '\t')) {
                                after_dot++;
                            }
                            if (*after_dot >= 'A' && *after_dot <= 'Z') {
                                const char *type_start = after_dot;
                                while ((*after_dot >= 'A' && *after_dot <= 'Z') ||
                                       (*after_dot >= 'a' && *after_dot <= 'z') ||
                                       (*after_dot >= '0' && *after_dot <= '9') ||
                                       *after_dot == '_') {
                                    after_dot++;
                                }
                                size_t type_len = after_dot - type_start;
                                if (type_len > 0 && type_len < sizeof(type_after_dot)) {
                                    memcpy(type_after_dot, type_start, type_len);
                                    type_after_dot[type_len] = '\0';
                                    referenced_type = type_after_dot;
                                }
                            }
                        }

                        bool is_ambiguous = false;
                        const char *ambig_mod1 = NULL;
                        const char *ambig_mod2 = NULL;
                        const char *full_module = lookup_module_alias(alias_map, typename_buf, referenced_type,
                                                                      dep_cache, &is_ambiguous,
                                                                      &ambig_mod1, &ambig_mod2);

                        if (is_ambiguous) {
                            /* AMBIGUOUS: Two different modules use the same alias.
                             * This matches Elm compiler behavior - it's an error to use this alias.
                             * We report it to stderr and keep the alias unexpanded. */
                            fprintf(stderr, "Warning: Ambiguous alias '%s' - refers to both '%s' and '%s'\n",
                                    typename_buf, ambig_mod1 ? ambig_mod1 : "?", ambig_mod2 ? ambig_mod2 : "?");
                            /* Keep as-is - cannot resolve */
                            memcpy(result + pos, start, len);
                            pos += len;
                        } else {
                            /* Check if this is a case where an aliased submodule should use the parent module name.
                             * Example: "import Svg" + "import Svg.Lazy as Svg"
                             * When we see Svg.Attribute (which refers to Svg.Lazy via the alias),
                             * we should keep it as "Svg" instead of expanding to "Svg.Lazy"
                             * because Svg is directly imported. */
                            bool is_aliased_submodule_of_direct_parent = false;
                            if (full_module != NULL) {
                                /* Check if full_module (e.g., "Svg.Lazy") is a submodule of typename_buf (e.g., "Svg")
                                 * and typename_buf is directly imported */
                                size_t alias_len = strlen(typename_buf);
                                if (strncmp(full_module, typename_buf, alias_len) == 0 &&
                                    full_module[alias_len] == '.' &&
                                    is_directly_imported(direct_imports, typename_buf)) {
                                    is_aliased_submodule_of_direct_parent = true;
                                }
                            }

                            bool should_expand = (full_module != NULL) && !is_aliased_submodule_of_direct_parent;

                            if (should_expand) {
                                /* Expand the alias to the full module name */
                                size_t flen = strlen(full_module);
                                memcpy(result + pos, full_module, flen);
                                pos += flen;
                            } else {
                                /* Keep as-is - either not an alias, or also directly imported */
                                memcpy(result + pos, start, len);
                                pos += len;
                            }
                        }
                    } else {
                        /* Check if it's a local type first - local types take precedence over imports */
                        bool is_local = false;
                        for (int i = 0; i < local_types_count; i++) {
                            if (strcmp(typename_buf, local_types[i]) == 0) {
                                is_local = true;
                                break;
                            }
                        }

                        if (is_local) {
                            /* Qualify with current module */
                            pos += snprintf(result + pos, buf_size - pos, "%s.%s", module_name, typename_buf);
                        } else {
                            /* Check if it's imported (including implicit imports) */
                            const char *import_module = lookup_import(import_map, typename_buf);
                            if (import_module) {
                                /* Use the imported module name */
                                pos += snprintf(result + pos, buf_size - pos, "%s.%s", import_module, typename_buf);
                            } else {
                                /* Unknown type - keep as-is (likely a type variable) */
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

/* Helper function to remove unnecessary outer parentheses from return type
 * Example: "A -> B -> (C -> D)" becomes "A -> B -> C -> D"
 * This matches Elm's canonical documentation format */
char *remove_return_type_parens(const char *type_str) {
    if (!type_str) return arena_strdup("");

    /* First, check if the entire type is wrapped in outer parentheses */
    /* This can happen when expanding type aliases that are function types */
    if (*type_str == '(') {
        const char *scan = type_str + 1;
        int depth = 1;
        bool has_comma = false;  /* Track if there's a comma inside (indicates tuple) */

        while (*scan && depth > 0) {
            if (*scan == '(') depth++;
            else if (*scan == ')') {
                depth--;
                if (depth == 0 && *(scan + 1) == '\0') {
                    /* The parens wrap the entire type */
                    /* Only unwrap if it's not a tuple (no comma at top level) */
                    if (!has_comma) {
                        /* Extract the inner type and recursively process it */
                        size_t inner_len = scan - type_str - 1;
                        /* Don't unwrap empty parens - that's the unit type () */
                        if (inner_len == 0) {
                            return arena_strdup("()");
                        }
                        char *inner = arena_malloc(inner_len + 1);
                        memcpy(inner, type_str + 1, inner_len);
                        inner[inner_len] = '\0';
                        char *result = remove_return_type_parens(inner);
                        arena_free(inner);
                        return result;
                    }
                    break;
                }
            } else if (*scan == ',' && depth == 1) {
                /* Comma at the top level inside these parens - this is a tuple */
                has_comma = true;
            }
            scan++;
        }
    }

    /* Find the last top-level arrow */
    const char *last_arrow = NULL;
    const char *p = type_str;
    int paren_depth = 0;
    int brace_depth = 0;

    while (*p) {
        if (*p == '(') {
            paren_depth++;
        } else if (*p == ')') {
            paren_depth--;
        } else if (*p == '{') {
            brace_depth++;
        } else if (*p == '}') {
            brace_depth--;
        } else if (paren_depth == 0 && brace_depth == 0 &&
                   *p == '-' && *(p + 1) == '>' &&
                   (p > type_str && *(p - 1) == ' ') && *(p + 2) == ' ') {
            last_arrow = p;
        }
        p++;
    }

    /* If there's no arrow, return as-is */
    if (!last_arrow) {
        return arena_strdup(type_str);
    }

    /* Find the start of the return type (skip " -> " and leading spaces) */
    const char *return_start = last_arrow + 3;  /* Skip " -> " */
    while (*return_start == ' ') return_start++;

    /* Check if the return type is wrapped in unnecessary outer parentheses */
    if (*return_start != '(') {
        return arena_strdup(type_str);
    }

    /* Check if these parens wrap the entire return type */
    const char *scan = return_start + 1;
    int depth = 1;
    const char *return_end = NULL;
    bool has_comma = false;  /* Track if there's a comma inside (indicates tuple) */

    while (*scan && depth > 0) {
        if (*scan == '(') depth++;
        else if (*scan == ')') {
            depth--;
            if (depth == 0) {
                return_end = scan;
            }
        } else if (*scan == ',' && depth == 1) {
            /* Comma at the top level inside these parens - this is a tuple */
            has_comma = true;
        }
        scan++;
    }

    /* Only remove parens if:
     * 1. They wrap the entire return type
     * 2. They don't contain a comma (not a tuple)
     * 3. The inner content is not empty (preserve unit type ()) */
    if (return_end && *(return_end + 1) == '\0' && !has_comma) {
        /* Build the new type string without these outer parens */
        size_t prefix_len = return_start - type_str;
        size_t inner_len = return_end - return_start - 1;  /* Skip opening '(' */

        /* Don't unwrap empty parens in return type - that's the unit type () */
        if (inner_len == 0) {
            return arena_strdup(type_str);
        }

        char *result = arena_malloc(prefix_len + inner_len + 1);
        memcpy(result, type_str, prefix_len);
        memcpy(result + prefix_len, return_start + 1, inner_len);
        result[prefix_len + inner_len] = '\0';

        return result;
    }

    return arena_strdup(type_str);
}
