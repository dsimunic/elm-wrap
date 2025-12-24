#include "frontend/ast_serialize.h"
#include "vendor/miniz.h"
#include "alloc.h"
#include "constants.h"
#include "fileutil.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ============================================================================
 * Helper macros and functions for binary serialization
 * ============================================================================ */

static AstSerializeError ast_ok(void) {
    AstSerializeError err = {0};
    return err;
}

static AstSerializeError ast_err(const char *msg) {
    AstSerializeError err;
    err.is_error = 1;
    snprintf(err.message, sizeof(err.message), "%s", msg);
    return err;
}

/* Dynamic buffer for building serialized output */
typedef struct {
    unsigned char *data;
    size_t         size;
    size_t         capacity;
} ByteBuffer;

static void buf_init(ByteBuffer *buf) {
    buf->data = NULL;
    buf->size = 0;
    buf->capacity = 0;
}

static int buf_ensure(ByteBuffer *buf, size_t additional) {
    size_t needed = buf->size + additional;
    if (needed <= buf->capacity) {
        return 0;
    }
    size_t new_cap = buf->capacity == 0 ? 256 : buf->capacity;
    while (new_cap < needed) {
        new_cap *= 2;
    }
    unsigned char *new_data = (unsigned char *)arena_realloc(buf->data, new_cap);
    if (!new_data) {
        return -1;
    }
    buf->data = new_data;
    buf->capacity = new_cap;
    return 0;
}

static int buf_write_u8(ByteBuffer *buf, unsigned char val) {
    if (buf_ensure(buf, 1) < 0) return -1;
    buf->data[buf->size++] = val;
    return 0;
}

static int buf_write_u16(ByteBuffer *buf, unsigned short val) {
    if (buf_ensure(buf, 2) < 0) return -1;
    buf->data[buf->size++] = (unsigned char)(val & 0xFF);
    buf->data[buf->size++] = (unsigned char)((val >> 8) & 0xFF);
    return 0;
}

static int buf_write_i64(ByteBuffer *buf, long val) {
    if (buf_ensure(buf, 8) < 0) return -1;
    unsigned long uval = (unsigned long)val;
    for (int i = 0; i < 8; i++) {
        buf->data[buf->size++] = (unsigned char)(uval & 0xFF);
        uval >>= 8;
    }
    return 0;
}

static int buf_write_string(ByteBuffer *buf, const char *s) {
    if (!s) {
        return buf_write_u16(buf, 0);
    }
    size_t len = strlen(s);
    if (len > 0xFFFF) {
        len = 0xFFFF;  /* Truncate if too long */
    }
    if (buf_write_u16(buf, (unsigned short)len) < 0) return -1;
    if (len > 0) {
        if (buf_ensure(buf, len) < 0) return -1;
        memcpy(buf->data + buf->size, s, len);
        buf->size += len;
    }
    return 0;
}

/* Reader state for deserialization */
typedef struct {
    const unsigned char *data;
    size_t               size;
    size_t               pos;
} ByteReader;

static int read_u8(ByteReader *r, unsigned char *out) {
    if (r->pos + 1 > r->size) return -1;
    *out = r->data[r->pos++];
    return 0;
}

static int read_u16(ByteReader *r, unsigned short *out) {
    if (r->pos + 2 > r->size) return -1;
    *out = (unsigned short)(r->data[r->pos] | (r->data[r->pos + 1] << 8));
    r->pos += 2;
    return 0;
}

static int read_i64(ByteReader *r, long *out) {
    if (r->pos + 8 > r->size) return -1;
    unsigned long uval = 0;
    for (int i = 0; i < 8; i++) {
        uval |= ((unsigned long)r->data[r->pos + i]) << (i * 8);
    }
    *out = (long)uval;
    r->pos += 8;
    return 0;
}

static int read_string(ByteReader *r, char **out) {
    unsigned short len;
    if (read_u16(r, &len) < 0) return -1;
    if (len == 0) {
        *out = NULL;
        return 0;
    }
    if (r->pos + len > r->size) return -1;
    char *s = (char *)arena_malloc(len + 1);
    if (!s) return -1;
    memcpy(s, r->data + r->pos, len);
    s[len] = '\0';
    r->pos += len;
    *out = s;
    return 0;
}

/* ============================================================================
 * Serialization functions
 * ============================================================================ */

static int serialize_term(ByteBuffer *buf, const AstTerm *term) {
    if (buf_write_u8(buf, (unsigned char)term->kind) < 0) return -1;
    switch (term->kind) {
    case TERM_VAR:
        if (buf_write_string(buf, term->u.var.name) < 0) return -1;
        break;
    case TERM_STRING:
        if (buf_write_string(buf, term->u.s) < 0) return -1;
        break;
    case TERM_INT:
        if (buf_write_i64(buf, term->u.i) < 0) return -1;
        break;
    case TERM_WILDCARD:
        /* Nothing extra needed */
        break;
    }
    return 0;
}

static int serialize_literal(ByteBuffer *buf, const AstLiteral *lit) {
    if (buf_write_u8(buf, (unsigned char)lit->kind) < 0) return -1;
    
    switch (lit->kind) {
    case LIT_POS:
    case LIT_NEG:
        if (buf_write_string(buf, lit->pred) < 0) return -1;
        if (buf_write_u8(buf, (unsigned char)lit->arity) < 0) return -1;
        for (int i = 0; i < lit->arity; i++) {
            if (serialize_term(buf, &lit->args[i]) < 0) return -1;
        }
        break;
    case LIT_EQ:
        if (serialize_term(buf, &lit->lhs) < 0) return -1;
        if (serialize_term(buf, &lit->rhs) < 0) return -1;
        break;
    case LIT_CMP:
        if (buf_write_u8(buf, (unsigned char)lit->cmp_op) < 0) return -1;
        if (serialize_term(buf, &lit->lhs) < 0) return -1;
        if (serialize_term(buf, &lit->rhs) < 0) return -1;
        break;
    case LIT_BUILTIN:
        if (buf_write_u8(buf, (unsigned char)lit->builtin) < 0) return -1;
        if (serialize_term(buf, &lit->lhs) < 0) return -1;
        if (serialize_term(buf, &lit->rhs) < 0) return -1;
        break;
    }
    return 0;
}

static int serialize_decl(ByteBuffer *buf, const AstDecl *decl) {
    if (buf_write_string(buf, decl->name) < 0) return -1;
    if (buf_write_u8(buf, (unsigned char)decl->arity) < 0) return -1;
    for (int i = 0; i < decl->arity; i++) {
        if (buf_write_string(buf, decl->arg_names ? decl->arg_names[i] : NULL) < 0) return -1;
        if (buf_write_string(buf, decl->arg_types ? decl->arg_types[i] : NULL) < 0) return -1;
    }
    return 0;
}

static int serialize_fact(ByteBuffer *buf, const AstFact *fact) {
    if (buf_write_string(buf, fact->pred) < 0) return -1;
    if (buf_write_u8(buf, (unsigned char)fact->arity) < 0) return -1;
    for (int i = 0; i < fact->arity; i++) {
        if (buf_write_u8(buf, (unsigned char)fact->arg_kind[i]) < 0) return -1;
        if (fact->arg_kind[i] == AST_ARG_STRING) {
            if (buf_write_string(buf, fact->arg_value[i].s) < 0) return -1;
        } else {
            if (buf_write_i64(buf, fact->arg_value[i].i) < 0) return -1;
        }
    }
    return 0;
}

static int serialize_rule(ByteBuffer *buf, const AstRule *rule) {
    if (buf_write_string(buf, rule->head_pred) < 0) return -1;
    if (buf_write_u8(buf, (unsigned char)rule->head_arity) < 0) return -1;
    for (int i = 0; i < rule->head_arity; i++) {
        if (serialize_term(buf, &rule->head_args[i]) < 0) return -1;
    }
    if (buf_write_u16(buf, (unsigned short)rule->num_body) < 0) return -1;
    for (int i = 0; i < rule->num_body; i++) {
        if (serialize_literal(buf, &rule->body[i]) < 0) return -1;
    }
    return 0;
}

AstSerializeError ast_serialize(const AstProgram *prog, unsigned char **out_data, size_t *out_size) {
    if (!prog || !out_data || !out_size) {
        return ast_err("Invalid arguments");
    }

    /* Build uncompressed payload */
    ByteBuffer payload;
    buf_init(&payload);

    /* Flags byte: bit 0 = clear_derived */
    unsigned char flags = prog->clear_derived ? 1 : 0;
    if (buf_write_u8(&payload, flags) < 0) {
        return ast_err("Out of memory");
    }

    /* Declarations */
    if (buf_write_u16(&payload, (unsigned short)prog->num_decls) < 0) {
        return ast_err("Out of memory");
    }
    for (int i = 0; i < prog->num_decls; i++) {
        if (serialize_decl(&payload, &prog->decls[i]) < 0) {
            return ast_err("Failed to serialize declaration");
        }
    }

    /* Facts */
    if (buf_write_u16(&payload, (unsigned short)prog->num_facts) < 0) {
        return ast_err("Out of memory");
    }
    for (int i = 0; i < prog->num_facts; i++) {
        if (serialize_fact(&payload, &prog->facts[i]) < 0) {
            return ast_err("Failed to serialize fact");
        }
    }

    /* Rules */
    if (buf_write_u16(&payload, (unsigned short)prog->num_rules) < 0) {
        return ast_err("Out of memory");
    }
    for (int i = 0; i < prog->num_rules; i++) {
        if (serialize_rule(&payload, &prog->rules[i]) < 0) {
            return ast_err("Failed to serialize rule");
        }
    }

    /* Compress the payload */
    mz_ulong compressed_bound = mz_compressBound((mz_ulong)payload.size);
    size_t output_size = AST_MAGIC_LEN + 4 + compressed_bound;  /* magic + uncompressed_size + compressed */
    unsigned char *output = (unsigned char *)arena_malloc(output_size);
    if (!output) {
        return ast_err("Out of memory for compressed output");
    }

    /* Write header */
    memcpy(output, AST_MAGIC, AST_MAGIC_LEN);
    output[AST_MAGIC_LEN + 0] = (unsigned char)(payload.size & 0xFF);
    output[AST_MAGIC_LEN + 1] = (unsigned char)((payload.size >> 8) & 0xFF);
    output[AST_MAGIC_LEN + 2] = (unsigned char)((payload.size >> 16) & 0xFF);
    output[AST_MAGIC_LEN + 3] = (unsigned char)((payload.size >> 24) & 0xFF);

    /* Compress */
    mz_ulong compressed_size = compressed_bound;
    int mz_result = mz_compress(
        output + AST_MAGIC_LEN + 4,
        &compressed_size,
        payload.data,
        (mz_ulong)payload.size
    );
    
    if (mz_result != MZ_OK) {
        return ast_err("Compression failed");
    }

    *out_data = output;
    *out_size = AST_MAGIC_LEN + 4 + compressed_size;
    return ast_ok();
}

/* ============================================================================
 * Deserialization functions
 * ============================================================================ */

static int deserialize_term(ByteReader *r, AstTerm *term) {
    unsigned char kind;
    if (read_u8(r, &kind) < 0) return -1;
    term->kind = (AstTermKind)kind;
    
    switch (term->kind) {
    case TERM_VAR:
        if (read_string(r, &term->u.var.name) < 0) return -1;
        term->u.var.id = -1;  /* Will be assigned during IR building */
        break;
    case TERM_STRING:
        if (read_string(r, &term->u.s) < 0) return -1;
        break;
    case TERM_INT:
        if (read_i64(r, &term->u.i) < 0) return -1;
        break;
    case TERM_WILDCARD:
        break;
    }
    return 0;
}

static int deserialize_literal(ByteReader *r, AstLiteral *lit) {
    unsigned char kind;
    if (read_u8(r, &kind) < 0) return -1;
    lit->kind = (AstLitKind)kind;
    
    switch (lit->kind) {
    case LIT_POS:
    case LIT_NEG:
        if (read_string(r, &lit->pred) < 0) return -1;
        {
            unsigned char arity;
            if (read_u8(r, &arity) < 0) return -1;
            lit->arity = arity;
        }
        for (int i = 0; i < lit->arity; i++) {
            if (deserialize_term(r, &lit->args[i]) < 0) return -1;
        }
        break;
    case LIT_EQ:
        if (deserialize_term(r, &lit->lhs) < 0) return -1;
        if (deserialize_term(r, &lit->rhs) < 0) return -1;
        break;
    case LIT_CMP:
        {
            unsigned char cmp_op;
            if (read_u8(r, &cmp_op) < 0) return -1;
            lit->cmp_op = (AstCmpOp)cmp_op;
        }
        if (deserialize_term(r, &lit->lhs) < 0) return -1;
        if (deserialize_term(r, &lit->rhs) < 0) return -1;
        break;
    case LIT_BUILTIN:
        {
            unsigned char builtin;
            if (read_u8(r, &builtin) < 0) return -1;
            lit->builtin = (AstBuiltinKind)builtin;
        }
        if (deserialize_term(r, &lit->lhs) < 0) return -1;
        if (deserialize_term(r, &lit->rhs) < 0) return -1;
        break;
    }
    return 0;
}

static int deserialize_decl(ByteReader *r, AstDecl *decl) {
    if (read_string(r, &decl->name) < 0) return -1;
    unsigned char arity;
    if (read_u8(r, &arity) < 0) return -1;
    decl->arity = arity;
    
    if (arity > 0) {
        decl->arg_names = (char **)arena_calloc(arity, sizeof(char *));
        decl->arg_types = (char **)arena_calloc(arity, sizeof(char *));
        if (!decl->arg_names || !decl->arg_types) return -1;
        
        for (int i = 0; i < arity; i++) {
            if (read_string(r, &decl->arg_names[i]) < 0) return -1;
            if (read_string(r, &decl->arg_types[i]) < 0) return -1;
        }
    } else {
        decl->arg_names = NULL;
        decl->arg_types = NULL;
    }
    return 0;
}

static int deserialize_fact(ByteReader *r, AstFact *fact) {
    if (read_string(r, &fact->pred) < 0) return -1;
    unsigned char arity;
    if (read_u8(r, &arity) < 0) return -1;
    fact->arity = arity;
    
    for (int i = 0; i < arity; i++) {
        unsigned char arg_kind;
        if (read_u8(r, &arg_kind) < 0) return -1;
        fact->arg_kind[i] = (AstArgKind)arg_kind;
        
        if (fact->arg_kind[i] == AST_ARG_STRING) {
            if (read_string(r, &fact->arg_value[i].s) < 0) return -1;
        } else {
            if (read_i64(r, &fact->arg_value[i].i) < 0) return -1;
        }
    }
    return 0;
}

static int deserialize_rule(ByteReader *r, AstRule *rule) {
    if (read_string(r, &rule->head_pred) < 0) return -1;
    unsigned char arity;
    if (read_u8(r, &arity) < 0) return -1;
    rule->head_arity = arity;
    
    for (int i = 0; i < arity; i++) {
        if (deserialize_term(r, &rule->head_args[i]) < 0) return -1;
    }
    
    unsigned short num_body;
    if (read_u16(r, &num_body) < 0) return -1;
    rule->num_body = num_body;
    rule->body_capacity = num_body;
    
    if (num_body > 0) {
        rule->body = (AstLiteral *)arena_calloc(num_body, sizeof(AstLiteral));
        if (!rule->body) return -1;
        
        for (int i = 0; i < num_body; i++) {
            if (deserialize_literal(r, &rule->body[i]) < 0) return -1;
        }
    } else {
        rule->body = NULL;
    }
    return 0;
}

AstSerializeError ast_deserialize(const unsigned char *data, size_t size, AstProgram *prog) {
    if (!data || !prog) {
        return ast_err("Invalid arguments");
    }
    
    /* Check magic */
    if (size < AST_MAGIC_LEN + 4) {
        return ast_err("File too small");
    }
    if (memcmp(data, AST_MAGIC, AST_MAGIC_LEN) != 0) {
        return ast_err("Invalid magic header");
    }
    
    /* Read uncompressed size */
    unsigned int uncompressed_size = 
        (unsigned int)(data[AST_MAGIC_LEN]) |
        ((unsigned int)(data[AST_MAGIC_LEN + 1]) << 8) |
        ((unsigned int)(data[AST_MAGIC_LEN + 2]) << 16) |
        ((unsigned int)(data[AST_MAGIC_LEN + 3]) << 24);
    
    /* Decompress */
    unsigned char *uncompressed = (unsigned char *)arena_malloc(uncompressed_size);
    if (!uncompressed) {
        return ast_err("Out of memory for decompression");
    }
    
    mz_ulong dest_len = uncompressed_size;
    int mz_result = mz_uncompress(
        uncompressed,
        &dest_len,
        data + AST_MAGIC_LEN + 4,
        (mz_ulong)(size - AST_MAGIC_LEN - 4)
    );
    
    if (mz_result != MZ_OK) {
        return ast_err("Decompression failed");
    }
    
    /* Parse the uncompressed data */
    ByteReader reader;
    reader.data = uncompressed;
    reader.size = dest_len;
    reader.pos = 0;
    
    /* Flags */
    unsigned char flags;
    if (read_u8(&reader, &flags) < 0) {
        return ast_err("Failed to read flags");
    }
    prog->clear_derived = (flags & 1) ? 1 : 0;
    
    /* Declarations */
    unsigned short num_decls;
    if (read_u16(&reader, &num_decls) < 0) {
        return ast_err("Failed to read declarations count");
    }
    if (num_decls > 0) {
        prog->decls = (AstDecl *)arena_calloc(num_decls, sizeof(AstDecl));
        if (!prog->decls) {
            return ast_err("Out of memory for declarations");
        }
        prog->decls_capacity = num_decls;
        prog->num_decls = num_decls;
        
        for (int i = 0; i < num_decls; i++) {
            if (deserialize_decl(&reader, &prog->decls[i]) < 0) {
                return ast_err("Failed to deserialize declaration");
            }
        }
    }
    
    /* Facts */
    unsigned short num_facts;
    if (read_u16(&reader, &num_facts) < 0) {
        return ast_err("Failed to read facts count");
    }
    if (num_facts > 0) {
        prog->facts = (AstFact *)arena_calloc(num_facts, sizeof(AstFact));
        if (!prog->facts) {
            return ast_err("Out of memory for facts");
        }
        prog->facts_capacity = num_facts;
        prog->num_facts = num_facts;
        
        for (int i = 0; i < num_facts; i++) {
            if (deserialize_fact(&reader, &prog->facts[i]) < 0) {
                return ast_err("Failed to deserialize fact");
            }
        }
    }
    
    /* Rules */
    unsigned short num_rules;
    if (read_u16(&reader, &num_rules) < 0) {
        return ast_err("Failed to read rules count");
    }
    if (num_rules > 0) {
        prog->rules = (AstRule *)arena_calloc(num_rules, sizeof(AstRule));
        if (!prog->rules) {
            return ast_err("Out of memory for rules");
        }
        prog->rules_capacity = num_rules;
        prog->num_rules = num_rules;
        
        for (int i = 0; i < num_rules; i++) {
            if (deserialize_rule(&reader, &prog->rules[i]) < 0) {
                return ast_err("Failed to deserialize rule");
            }
        }
    }
    
    return ast_ok();
}

AstSerializeError ast_serialize_to_file(const AstProgram *prog, const char *path) {
    unsigned char *data = NULL;
    size_t size = 0;
    
    AstSerializeError err = ast_serialize(prog, &data, &size);
    if (err.is_error) {
        return err;
    }
    
    FILE *f = fopen(path, "wb");
    if (!f) {
        return ast_err("Failed to open output file");
    }
    
    size_t written = fwrite(data, 1, size, f);
    fclose(f);
    
    if (written != size) {
        return ast_err("Failed to write output file");
    }
    
    return ast_ok();
}

AstSerializeError ast_deserialize_from_file(const char *path, AstProgram *prog) {
    size_t read_size = 0;
    char *data = file_read_contents_bounded(path, MAX_RULR_COMPILED_FILE_BYTES, &read_size);
    if (!data || read_size == 0) {
        arena_free(data);
        return ast_err("Failed to read file");
    }

    return ast_deserialize((const unsigned char *)data, read_size, prog);
}

/* ============================================================================
 * Pretty printing
 * ============================================================================ */

static void print_term(const AstTerm *term) {
    switch (term->kind) {
    case TERM_VAR:
        printf("%s", term->u.var.name);
        break;
    case TERM_STRING:
        printf("\"%s\"", term->u.s);
        break;
    case TERM_INT:
        printf("%ld", term->u.i);
        break;
    case TERM_WILDCARD:
        printf("_");
        break;
    }
}

static void print_literal(const AstLiteral *lit) {
    switch (lit->kind) {
    case LIT_POS:
        printf("%s(", lit->pred);
        for (int i = 0; i < lit->arity; i++) {
            if (i > 0) printf(", ");
            print_term(&lit->args[i]);
        }
        printf(")");
        break;
    case LIT_NEG:
        printf("not %s(", lit->pred);
        for (int i = 0; i < lit->arity; i++) {
            if (i > 0) printf(", ");
            print_term(&lit->args[i]);
        }
        printf(")");
        break;
    case LIT_EQ:
        print_term(&lit->lhs);
        printf(" = ");
        print_term(&lit->rhs);
        break;
    case LIT_CMP:
        print_term(&lit->lhs);
        switch (lit->cmp_op) {
        case CMP_EQ: printf(" = "); break;
        case CMP_NE: printf(" != "); break;
        case CMP_LT: printf(" < "); break;
        case CMP_LE: printf(" <= "); break;
        case CMP_GT: printf(" > "); break;
        case CMP_GE: printf(" >= "); break;
        }
        print_term(&lit->rhs);
        break;
    case LIT_BUILTIN:
        switch (lit->builtin) {
        case BUILTIN_MATCH:
            printf("match(");
            print_term(&lit->lhs);
            printf(", ");
            print_term(&lit->rhs);
            printf(")");
            break;
        }
        break;
    }
}

void ast_pretty_print(const AstProgram *prog) {
    /* Print declarations */
    for (int i = 0; i < prog->num_decls; i++) {
        const AstDecl *decl = &prog->decls[i];
        printf(".pred %s(", decl->name);
        for (int j = 0; j < decl->arity; j++) {
            if (j > 0) printf(", ");
            if (decl->arg_names && decl->arg_names[j]) {
                printf("%s", decl->arg_names[j]);
            } else {
                printf("arg%d", j);
            }
            if (decl->arg_types && decl->arg_types[j]) {
                printf(": %s", decl->arg_types[j]);
            }
        }
        printf(").\n");
    }
    
    if (prog->num_decls > 0 && (prog->num_facts > 0 || prog->num_rules > 0)) {
        printf("\n");
    }
    
    /* Print facts */
    for (int i = 0; i < prog->num_facts; i++) {
        const AstFact *fact = &prog->facts[i];
        printf("%s(", fact->pred);
        for (int j = 0; j < fact->arity; j++) {
            if (j > 0) printf(", ");
            if (fact->arg_kind[j] == AST_ARG_STRING) {
                printf("\"%s\"", fact->arg_value[j].s);
            } else {
                printf("%ld", fact->arg_value[j].i);
            }
        }
        printf(").\n");
    }
    
    if (prog->num_facts > 0 && prog->num_rules > 0) {
        printf("\n");
    }
    
    /* Print rules */
    for (int i = 0; i < prog->num_rules; i++) {
        const AstRule *rule = &prog->rules[i];
        printf("%s(", rule->head_pred);
        for (int j = 0; j < rule->head_arity; j++) {
            if (j > 0) printf(", ");
            print_term(&rule->head_args[j]);
        }
        printf(") :-\n");
        for (int j = 0; j < rule->num_body; j++) {
            printf("    ");
            print_literal(&rule->body[j]);
            if (j < rule->num_body - 1) {
                printf(",");
            } else {
                printf(".");
            }
            printf("\n");
        }
    }
    
    /* Print clear_derived directive if set */
    if (prog->clear_derived) {
        printf("\n.clear_derived()\n");
    }
}
