#include <stdio.h>
#include <string.h>

#include "alloc.h"
#include "rulr.h"
#include "rulr_dl.h"
#include "runtime/runtime.h"

static void print_usage(const char *prog) {
    printf("Usage: %s --rules RULE_NAME [--facts FACT_FILE]\n", prog);
    printf("\nOptions:\n");
    printf("  --rules, -r        Rule name or path (without extension)\n");
    printf("                     Tries .dlc (compiled) first, then .dl (source)\n");
    printf("  --facts, -f        Path to fact file in source format\n");
    printf("  -h, --help         Show this help message\n");
    printf("\nIf only one positional argument is provided, it is treated as the rule file.\n");
}

static void print_value(const Rulr *rulr, const Value *v) {
    switch (v->kind) {
    case VAL_SYM: {
        const char *name = rulr_lookup_symbol(rulr, v->u.sym);
        if (name) {
            printf("%s", name);
        } else {
            printf("#%d", v->u.sym);
        }
        break;
    }
    case VAL_RANGE:
        printf("range(%ld)", v->u.i);
        break;
    case VAL_INT:
        printf("%ld", v->u.i);
        break;
    default:
        printf("?");
        break;
    }
}

static void print_relation(const char *pred_name, const Rulr *rulr, EngineRelationView view) {
    printf("Derived %s facts: %d\n", pred_name, view.num_tuples);
    const Tuple *tuples = (const Tuple *)view.tuples;
    for (int i = 0; i < view.num_tuples; ++i) {
        const Tuple *t = &tuples[i];
        printf(" - %s(", pred_name);
        for (int a = 0; a < t->arity; ++a) {
            print_value(rulr, &t->fields[a]);
            if (a + 1 < t->arity) {
                printf(", ");
            }
        }
        printf(")\n");
    }
}

int main(int argc, char *argv[]) {
    alloc_init();

    const char *rule_path = NULL;
    const char *fact_path = NULL;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            alloc_shutdown();
            return 0;
        }
        if ((strcmp(argv[i], "--rules") == 0 || strcmp(argv[i], "-r") == 0) && i + 1 < argc) {
            rule_path = argv[++i];
            continue;
        }
        if ((strcmp(argv[i], "--facts") == 0 || strcmp(argv[i], "-f") == 0) && i + 1 < argc) {
            fact_path = argv[++i];
            continue;
        }
        if (!rule_path) {
            rule_path = argv[i];
        } else if (!fact_path) {
            fact_path = argv[i];
        } else {
            fprintf(stderr, "Unexpected argument: %s\n", argv[i]);
            print_usage(argv[0]);
            alloc_shutdown();
            return 1;
        }
    }

    if (!rule_path) {
        print_usage(argv[0]);
        alloc_shutdown();
        return 1;
    }

    Rulr rulr;
    RulrError err = rulr_init(&rulr);
    if (err.is_error) {
        fprintf(stderr, "Failed to initialize engine: %s\n", err.message);
        alloc_shutdown();
        return 1;
    }

    /* Load rule file (tries .dlc first, then .dl) */
    err = rulr_load_rule_file(&rulr, rule_path);
    if (err.is_error) {
        fprintf(stderr, "Failed to load rules: %s\n", err.message);
        rulr_deinit(&rulr);
        alloc_shutdown();
        return 1;
    }

    /* Load facts file if provided (always .dl format) */
    if (fact_path) {
        err = rulr_load_dl_file(&rulr, fact_path);
        if (err.is_error) {
            fprintf(stderr, "Failed to load facts: %s\n", err.message);
            rulr_deinit(&rulr);
            alloc_shutdown();
            return 1;
        }
    }

    err = rulr_evaluate(&rulr);
    if (err.is_error) {
        fprintf(stderr, "Evaluation failed: %s\n", err.message);
        rulr_deinit(&rulr);
        alloc_shutdown();
        return 1;
    }

    EngineRelationView view = rulr_get_relation(&rulr, "error");
    if (view.pred_id < 0) {
        printf("No 'error' predicate found. Add one to your rule file to report violations.\n");
    } else {
        print_relation("error", &rulr, view);
        printf("\nTip: map symbol IDs to names during development for readability.\n");
        printf("For machine-readable output, iterate over relation tuples and emit JSON/CSV.\n");
    }

    rulr_deinit(&rulr);
    alloc_shutdown();
    return 0;
}
