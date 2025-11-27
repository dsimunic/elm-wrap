#include "docs_json.h"
#include "elm_docs.h"
#include <stdio.h>

/* Print a JSON-escaped string to stdout */
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
