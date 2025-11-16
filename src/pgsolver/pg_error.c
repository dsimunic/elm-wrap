#include "pg_error.h"
#include "pg_core.h"
#include "../alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/*
 * This implementation follows the error reporting algorithm from the PubGrub
 * specification (see doc/pubgrub-solver.md, section "Error Reporting").
 *
 * The algorithm performs a depth-first traversal of the derivation graph,
 * generating explanations for each incompatibility. Line numbers are assigned
 * to incompatibilities that need to be referenced later.
 */

/* Forward declarations for internal types from pg_core.c */
typedef struct {
    int pkg;
    void *range;
    bool positive;
} PgTermInternal;

typedef enum {
    PG_REASON_DEPENDENCY,
    PG_REASON_NO_VERSIONS,
    PG_REASON_ROOT,
    PG_REASON_INTERNAL
} PgIncompatibilityReasonInternal;

typedef struct PgIncompatibilityInternal {
    PgTermInternal *terms;
    size_t term_count;
    PgIncompatibilityReasonInternal reason;
    struct PgIncompatibilityInternal **causes;
    size_t cause_count;
    bool attached;
} PgIncompatibilityInternal;

/* Writer context for building the error message */
typedef struct {
    char *buffer;
    size_t buffer_size;
    size_t offset;
    bool truncated;
} ErrorWriter;

/* Line numbering state */
typedef struct {
    PgIncompatibilityInternal **incompatibilities;
    int *line_numbers;
    size_t count;
    size_t capacity;
    int next_line_number;
} LineNumbering;

static void writer_init(ErrorWriter *w, char *buffer, size_t buffer_size) {
    w->buffer = buffer;
    w->buffer_size = buffer_size;
    w->offset = 0;
    w->truncated = false;
    if (buffer && buffer_size > 0) {
        buffer[0] = '\0';
    }
}

static void writer_append(ErrorWriter *w, const char *text) {
    if (!w || !text || w->truncated) {
        return;
    }

    size_t text_len = strlen(text);
    if (w->offset + text_len + 1 > w->buffer_size) {
        w->truncated = true;
        return;
    }

    memcpy(w->buffer + w->offset, text, text_len);
    w->offset += text_len;
    w->buffer[w->offset] = '\0';
}

static void writer_appendf(ErrorWriter *w, const char *fmt, ...) {
    if (!w || w->truncated) {
        return;
    }

    char temp[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(temp, sizeof(temp), fmt, args);
    va_end(args);

    writer_append(w, temp);
}

static void line_numbering_init(LineNumbering *ln) {
    ln->incompatibilities = NULL;
    ln->line_numbers = NULL;
    ln->count = 0;
    ln->capacity = 0;
    ln->next_line_number = 1;
}

static void line_numbering_free(LineNumbering *ln) {
    arena_free(ln->incompatibilities);
    arena_free(ln->line_numbers);
}

static int line_numbering_get(LineNumbering *ln, PgIncompatibilityInternal *inc) {
    for (size_t i = 0; i < ln->count; i++) {
        if (ln->incompatibilities[i] == inc) {
            return ln->line_numbers[i];
        }
    }
    return 0;
}

static bool line_numbering_assign(LineNumbering *ln, PgIncompatibilityInternal *inc) {
    /* Check if already assigned */
    if (line_numbering_get(ln, inc) != 0) {
        return true;
    }

    /* Expand capacity if needed */
    if (ln->count >= ln->capacity) {
        size_t new_capacity = ln->capacity == 0 ? 8 : ln->capacity * 2;
        PgIncompatibilityInternal **new_incs = arena_realloc(
            ln->incompatibilities,
            new_capacity * sizeof(PgIncompatibilityInternal *)
        );
        int *new_lines = arena_realloc(
            ln->line_numbers,
            new_capacity * sizeof(int)
        );
        if (!new_incs || !new_lines) {
            arena_free(new_incs);
            arena_free(new_lines);
            return false;
        }
        ln->incompatibilities = new_incs;
        ln->line_numbers = new_lines;
        ln->capacity = new_capacity;
    }

    ln->incompatibilities[ln->count] = inc;
    ln->line_numbers[ln->count] = ln->next_line_number;
    ln->count++;
    ln->next_line_number++;

    return true;
}

/* Count outgoing edges (how many incompatibilities are caused by this one) */
static int count_outgoing_edges(
    PgIncompatibilityInternal *root,
    PgIncompatibilityInternal *target
) {
    /* This is a simplified implementation. A full implementation would
     * traverse the entire graph and count references. For now, we use
     * a heuristic: derived incompatibilities with multiple causes are
     * likely to be referenced. */
    if (target->cause_count >= 2) {
        return 2; /* Likely to be referenced */
    }
    return 1;
}

/* Format a version range as a string */
static void format_range(
    void *range_ptr,
    char *out,
    size_t out_size,
    bool *out_is_any
) {
    /* Access range fields directly - this is a hack since we don't have
     * the full PgVersionRange definition here. In a real implementation,
     * we'd pass proper types. */
    PgVersionRange *range = (PgVersionRange *)range_ptr;

    if (!range) {
        snprintf(out, out_size, "any");
        if (out_is_any) *out_is_any = true;
        return;
    }

    /* Check if it's "any" (unbounded on both ends) */
    if (range->lower.unbounded && range->upper.unbounded) {
        snprintf(out, out_size, "any");
        if (out_is_any) *out_is_any = true;
        return;
    }

    if (out_is_any) *out_is_any = false;

    /* Check if it's an exact version */
    if (!range->lower.unbounded && !range->upper.unbounded &&
        range->lower.inclusive && range->upper.inclusive &&
        range->lower.v.major == range->upper.v.major &&
        range->lower.v.minor == range->upper.v.minor &&
        range->lower.v.patch == range->upper.v.patch) {
        snprintf(out, out_size, "%d.%d.%d",
                 range->lower.v.major,
                 range->lower.v.minor,
                 range->lower.v.patch);
        return;
    }

    /* Check for caret ranges (^X.Y.Z = >=X.Y.Z <X+1.0.0) */
    if (!range->lower.unbounded && !range->upper.unbounded &&
        range->lower.inclusive && !range->upper.inclusive &&
        range->upper.v.minor == 0 && range->upper.v.patch == 0 &&
        range->upper.v.major == range->lower.v.major + 1) {
        snprintf(out, out_size, "^%d.%d.%d",
                 range->lower.v.major,
                 range->lower.v.minor,
                 range->lower.v.patch);
        return;
    }

    /* Generic range */
    char lower_str[64] = "";
    char upper_str[64] = "";

    if (!range->lower.unbounded) {
        snprintf(lower_str, sizeof(lower_str), "%s%d.%d.%d",
                 range->lower.inclusive ? ">=" : ">",
                 range->lower.v.major,
                 range->lower.v.minor,
                 range->lower.v.patch);
    }

    if (!range->upper.unbounded) {
        snprintf(upper_str, sizeof(upper_str), "%s%d.%d.%d",
                 range->upper.inclusive ? "<=" : "<",
                 range->upper.v.major,
                 range->upper.v.minor,
                 range->upper.v.patch);
    }

    if (lower_str[0] && upper_str[0]) {
        snprintf(out, out_size, "%s %s", lower_str, upper_str);
    } else if (lower_str[0]) {
        snprintf(out, out_size, "%s", lower_str);
    } else if (upper_str[0]) {
        snprintf(out, out_size, "%s", upper_str);
    } else {
        snprintf(out, out_size, "any");
    }
}

/* Format a term as a string */
static void format_term(
    PgTermInternal *term,
    PgPackageNameResolver name_resolver,
    void *name_ctx,
    char *out,
    size_t out_size
) {
    const char *pkg_name = name_resolver(name_ctx, term->pkg);
    char range_str[128];
    bool is_any;
    format_range(term->range, range_str, sizeof(range_str), &is_any);

    if (is_any && term->positive) {
        snprintf(out, out_size, "%s", pkg_name);
    } else if (is_any && !term->positive) {
        snprintf(out, out_size, "not %s", pkg_name);
    } else if (term->positive) {
        snprintf(out, out_size, "%s %s", pkg_name, range_str);
    } else {
        snprintf(out, out_size, "not %s %s", pkg_name, range_str);
    }
}

/* Forward declaration */
static bool explain_incompatibility(
    PgIncompatibilityInternal *inc,
    ErrorWriter *writer,
    LineNumbering *ln,
    PgPackageNameResolver name_resolver,
    void *name_ctx,
    bool is_root
);

/* Explain a dependency-based incompatibility */
static void explain_dependency(
    PgIncompatibilityInternal *inc,
    ErrorWriter *writer,
    PgPackageNameResolver name_resolver,
    void *name_ctx
) {
    /* Format: "pkg range depends on dep_pkg dep_range" */
    if (inc->term_count != 2) {
        writer_append(writer, "[malformed dependency]");
        return;
    }

    char pkg_str[256];
    char dep_str[256];

    /* First term is the depender (positive), second is dependency (negative) */
    PgTermInternal depender = inc->terms[0];
    PgTermInternal dependency = inc->terms[1];

    format_term(&depender, name_resolver, name_ctx, pkg_str, sizeof(pkg_str));

    /* Flip the negative dependency term to positive for display */
    PgTermInternal dep_positive = dependency;
    dep_positive.positive = !dep_positive.positive;
    format_term(&dep_positive, name_resolver, name_ctx, dep_str, sizeof(dep_str));

    writer_appendf(writer, "%s depends on %s", pkg_str, dep_str);
}

/* Explain a derived incompatibility with two external causes */
static void explain_two_external(
    PgIncompatibilityInternal *inc,
    ErrorWriter *writer,
    PgPackageNameResolver name_resolver,
    void *name_ctx,
    const char *conjunction
) {
    if (inc->cause_count != 2) {
        writer_append(writer, "[malformed incompatibility]");
        return;
    }

    PgIncompatibilityInternal *cause1 = inc->causes[0];
    PgIncompatibilityInternal *cause2 = inc->causes[1];

    /* Both causes should be external (dependencies) */
    if (cause1->reason != PG_REASON_DEPENDENCY ||
        cause2->reason != PG_REASON_DEPENDENCY) {
        writer_append(writer, "[unexpected cause type]");
        return;
    }

    /* Get the depender from the first cause */
    char depender_str[256];
    format_term(&cause1->terms[0], name_resolver, name_ctx,
                depender_str, sizeof(depender_str));

    /* Get the transitive dependency from the second cause */
    PgTermInternal trans_dep = cause2->terms[1];
    trans_dep.positive = !trans_dep.positive;
    char trans_dep_str[256];
    format_term(&trans_dep, name_resolver, name_ctx,
                trans_dep_str, sizeof(trans_dep_str));

    writer_appendf(writer, "%s %s depends on %s which depends on %s",
                   conjunction, depender_str,
                   "...", /* intermediate package */
                   trans_dep_str);
}

/* Main recursive explanation function */
static bool explain_incompatibility(
    PgIncompatibilityInternal *inc,
    ErrorWriter *writer,
    LineNumbering *ln,
    PgPackageNameResolver name_resolver,
    void *name_ctx,
    bool is_root
) {
    if (!inc || !writer) {
        return false;
    }

    /* Case 1: Two derived causes */
    if (inc->cause_count == 2 &&
        inc->causes[0]->cause_count > 0 &&
        inc->causes[1]->cause_count > 0) {

        int line1 = line_numbering_get(ln, inc->causes[0]);
        int line2 = line_numbering_get(ln, inc->causes[1]);

        /* Case 1.1: Both already have line numbers */
        if (line1 > 0 && line2 > 0) {
            writer_appendf(writer, "Because (%d) and (%d), ...\n", line1, line2);
            return true;
        }

        /* Case 1.2: Only one has a line number */
        if (line1 > 0 || line2 > 0) {
            PgIncompatibilityInternal *numbered = (line1 > 0) ? inc->causes[0] : inc->causes[1];
            PgIncompatibilityInternal *unnumbered = (line1 > 0) ? inc->causes[1] : inc->causes[0];
            int line = (line1 > 0) ? line1 : line2;

            explain_incompatibility(unnumbered, writer, ln, name_resolver, name_ctx, false);
            writer_appendf(writer, "And because (%d), ...\n", line);
            return true;
        }

        /* Case 1.3: Neither has a line number */
        /* Check if one is simpler (both external causes) */
        bool cause0_simple = (inc->causes[0]->cause_count == 2 &&
                              inc->causes[0]->causes[0]->cause_count == 0 &&
                              inc->causes[0]->causes[1]->cause_count == 0);
        bool cause1_simple = (inc->causes[1]->cause_count == 2 &&
                              inc->causes[1]->causes[0]->cause_count == 0 &&
                              inc->causes[1]->causes[1]->cause_count == 0);

        if (cause0_simple || cause1_simple) {
            PgIncompatibilityInternal *simple = cause0_simple ? inc->causes[0] : inc->causes[1];
            PgIncompatibilityInternal *complex = cause0_simple ? inc->causes[1] : inc->causes[0];

            explain_incompatibility(complex, writer, ln, name_resolver, name_ctx, false);
            explain_incompatibility(simple, writer, ln, name_resolver, name_ctx, false);
            writer_append(writer, "Thus, ...\n");
            return true;
        }

        /* Both complex: number them */
        explain_incompatibility(inc->causes[0], writer, ln, name_resolver, name_ctx, false);
        if (count_outgoing_edges(inc, inc->causes[0]) > 1) {
            line_numbering_assign(ln, inc->causes[0]);
        }

        writer_append(writer, "\n");

        explain_incompatibility(inc->causes[1], writer, ln, name_resolver, name_ctx, false);
        line_numbering_assign(ln, inc->causes[1]);

        int assigned_line = line_numbering_get(ln, inc->causes[1]);
        writer_appendf(writer, "And because (%d), ...\n", assigned_line);
        return true;
    }

    /* Case 2: One derived, one external */
    if (inc->cause_count == 2) {
        PgIncompatibilityInternal *derived = NULL;
        PgIncompatibilityInternal *external = NULL;

        if (inc->causes[0]->cause_count > 0) {
            derived = inc->causes[0];
            external = inc->causes[1];
        } else if (inc->causes[1]->cause_count > 0) {
            derived = inc->causes[1];
            external = inc->causes[0];
        }

        if (derived && external) {
            int derived_line = line_numbering_get(ln, derived);

            /* Case 2.1: Derived already has line number */
            if (derived_line > 0) {
                char ext_str[512];
                ErrorWriter temp_writer;
                writer_init(&temp_writer, ext_str, sizeof(ext_str));
                if (external->reason == PG_REASON_DEPENDENCY) {
                    explain_dependency(external, &temp_writer, name_resolver, name_ctx);
                }
                writer_appendf(writer, "Because %s and (%d), ...\n",
                               ext_str, derived_line);
                return true;
            }

            /* Case 2.2: Derived has one derived cause with no line number */
            if (derived->cause_count == 2 &&
                ((derived->causes[0]->cause_count > 0 &&
                  line_numbering_get(ln, derived->causes[0]) == 0) ||
                 (derived->causes[1]->cause_count > 0 &&
                  line_numbering_get(ln, derived->causes[1]) == 0))) {

                PgIncompatibilityInternal *prior_derived = NULL;
                PgIncompatibilityInternal *prior_external = NULL;

                if (derived->causes[0]->cause_count > 0) {
                    prior_derived = derived->causes[0];
                    prior_external = derived->causes[1];
                } else {
                    prior_derived = derived->causes[1];
                    prior_external = derived->causes[0];
                }

                explain_incompatibility(prior_derived, writer, ln,
                                        name_resolver, name_ctx, false);

                /* Format both external causes */
                writer_append(writer, "And because ");
                if (prior_external->reason == PG_REASON_DEPENDENCY) {
                    explain_dependency(prior_external, writer, name_resolver, name_ctx);
                }
                writer_append(writer, " and ");
                if (external->reason == PG_REASON_DEPENDENCY) {
                    explain_dependency(external, writer, name_resolver, name_ctx);
                }
                writer_append(writer, ", ...\n");
                return true;
            }

            /* Case 2.3: General case */
            explain_incompatibility(derived, writer, ln, name_resolver, name_ctx, false);
            writer_append(writer, "And because ");
            if (external->reason == PG_REASON_DEPENDENCY) {
                explain_dependency(external, writer, name_resolver, name_ctx);
            }
            writer_append(writer, ", ...\n");
            return true;
        }
    }

    /* Case 3: Both external */
    if (inc->cause_count == 2 &&
        inc->causes[0]->cause_count == 0 &&
        inc->causes[1]->cause_count == 0) {

        writer_append(writer, "Because ");
        if (inc->causes[0]->reason == PG_REASON_DEPENDENCY) {
            explain_dependency(inc->causes[0], writer, name_resolver, name_ctx);
        }
        writer_append(writer, " and ");
        if (inc->causes[1]->reason == PG_REASON_DEPENDENCY) {
            explain_dependency(inc->causes[1], writer, name_resolver, name_ctx);
        }
        writer_append(writer, ", ...\n");
        return true;
    }

    /* Fallback: single cause or external */
    if (inc->reason == PG_REASON_DEPENDENCY) {
        explain_dependency(inc, writer, name_resolver, name_ctx);
        writer_append(writer, "\n");
        return true;
    }

    writer_append(writer, "[incompatibility]\n");
    return true;
}

bool pg_error_report(
    void *solver_ptr,
    void *root_incompatibility_ptr,
    PgPackageNameResolver name_resolver,
    void *name_ctx,
    char *out_buffer,
    size_t buffer_size
) {
    if (!root_incompatibility_ptr || !name_resolver || !out_buffer || buffer_size == 0) {
        return false;
    }

    PgIncompatibilityInternal *root = (PgIncompatibilityInternal *)root_incompatibility_ptr;

    ErrorWriter writer;
    writer_init(&writer, out_buffer, buffer_size);

    LineNumbering ln;
    line_numbering_init(&ln);

    writer_append(&writer, "Version solving failed:\n\n");

    bool success = explain_incompatibility(root, &writer, &ln,
                                           name_resolver, name_ctx, true);

    if (!success || writer.truncated) {
        writer_append(&writer, "\n[Error message truncated or incomplete]");
    }

    line_numbering_free(&ln);

    return !writer.truncated;
}
