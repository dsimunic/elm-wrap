#include <ctype.h>
#include <string.h>
#include "frontend/lexer.h"
#include "alloc.h"

#define STRING_INIT_CAPACITY 32

static int lexer_peek(Lexer *lx) {
    if (lx->pos >= lx->length) {
        return -1;
    }
    return (unsigned char)lx->input[lx->pos];
}

static int lexer_advance(Lexer *lx) {
    if (lx->pos >= lx->length) {
        return -1;
    }
    int ch = (unsigned char)lx->input[lx->pos++];
    if (ch == '\n') {
        lx->line += 1;
        lx->column = 1;
    } else {
        lx->column += 1;
    }
    return ch;
}

static void lexer_skip_ws(Lexer *lx) {
    while (1) {
        int ch = lexer_peek(lx);
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
            lexer_advance(lx);
        } else if (ch == '%') {
            /* Skip line comment until end of line or EOF */
            lexer_advance(lx);
            while ((ch = lexer_peek(lx)) >= 0 && ch != '\n') {
                lexer_advance(lx);
            }
        } else {
            break;
        }
    }
}

static int is_ident_start(int ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_';
}

static int is_ident_body(int ch) {
    return is_ident_start(ch) || (ch >= '0' && ch <= '9');
}

static Token make_token(Lexer *lx, TokenKind kind, const char *start, int len) {
    Token t;
    t.kind = kind;
    t.lexeme = start;
    t.length = len;
    t.int_value = 0;
    t.string_value = NULL;
    t.line = lx->line;
    t.column = lx->column;
    return t;
}

static Token lex_number(Lexer *lx, const char *start) {
    size_t begin_pos = lx->pos - 1;
    while (isdigit(lexer_peek(lx))) {
        lexer_advance(lx);
    }
    int len = (int)(lx->pos - begin_pos);
    Token t = make_token(lx, TOK_INT, start, len);

    long value = 0;
    for (int i = 0; i < len; ++i) {
        int d = start[i] - '0';
        value = value * 10 + d;
    }
    t.int_value = value;
    return t;
}

static Token lex_ident(Lexer *lx, const char *start) {
    size_t begin_pos = lx->pos - 1;
    while (is_ident_body(lexer_peek(lx))) {
        lexer_advance(lx);
    }
    int len = (int)(lx->pos - begin_pos);
    
    /* Check for wildcard: single underscore */
    if (len == 1 && start[0] == '_') {
        return make_token(lx, TOK_WILDCARD, start, len);
    }
    
    Token t = make_token(lx, TOK_IDENT, start, len);

    if (len == 3 && strncmp(start, "not", 3) == 0) {
        t.kind = TOK_NOT;
    }
    return t;
}

static Token lex_string(Lexer *lx) {
    const char *start = lx->input + lx->pos - 1; /* includes opening quote */
    size_t buffer_cap = STRING_INIT_CAPACITY;
    size_t buffer_len = 0;
    char *buffer = (char *)arena_malloc(buffer_cap);
    if (!buffer) {
        Token t = make_token(lx, TOK_INVALID, start, 0);
        return t;
    }

    while (1) {
        int next = lexer_peek(lx);
        if (next < 0) {
            Token t = make_token(lx, TOK_INVALID, start, (int)(lx->pos - (start - lx->input)));
            return t;
        }
        lexer_advance(lx);
        if (next == '"') {
            break;
        }
        if (next == '\\') {
            int esc = lexer_advance(lx);
            switch (esc) {
            case 'n':
                next = '\n';
                break;
            case 't':
                next = '\t';
                break;
            case '\\':
                next = '\\';
                break;
            case '"':
                next = '"';
                break;
            default:
                next = esc;
                break;
            }
        }
        if (buffer_len + 1 >= buffer_cap) {
            size_t new_cap = buffer_cap * 2;
            char *new_buf = (char *)arena_realloc(buffer, new_cap);
            if (!new_buf) {
                Token t = make_token(lx, TOK_INVALID, start, (int)(lx->pos - (start - lx->input)));
                return t;
            }
            buffer = new_buf;
            buffer_cap = new_cap;
        }
        buffer[buffer_len++] = (char)next;
    }
    buffer[buffer_len] = '\0';
    Token t = make_token(lx, TOK_STRING, start, (int)(lx->pos - (start - lx->input)));
    t.string_value = buffer;
    return t;
}

void lexer_init(Lexer *lx, const char *input) {
    lx->input = input;
    lx->length = strlen(input);
    lx->pos = 0;
    lx->line = 1;
    lx->column = 1;
    lx->current = make_token(lx, TOK_INVALID, NULL, 0);
    lx->lookahead = make_token(lx, TOK_INVALID, NULL, 0);
}

Token lexer_next(Lexer *lx) {
    lexer_skip_ws(lx);

    int ch = lexer_peek(lx);
    if (ch < 0) {
        return make_token(lx, TOK_EOF, lx->input + lx->pos, 0);
    }

    lexer_advance(lx);

    switch (ch) {
    case '.':
        if (lx->pos + 12 < lx->length) {
            const char *p = lx->input + lx->pos;
            if (strncmp(p, "clear_derived", 13) == 0) {
                lx->pos += 13;
                lx->column += 13;
                return make_token(lx, TOK_CLEAR_DERIVED, lx->input + lx->pos - 14, 14);
            }
        }
        if (lx->pos + 3 < lx->length) {
            const char *p = lx->input + lx->pos;
            if (p[0] == 'p' && p[1] == 'r' && p[2] == 'e' && p[3] == 'd') {
                lx->pos += 4;
                lx->column += 4;
                return make_token(lx, TOK_PRED, lx->input + lx->pos - 5, 5);
            }
        }
        return make_token(lx, TOK_DOT, lx->input + lx->pos - 1, 1);
    case ',':
        return make_token(lx, TOK_COMMA, lx->input + lx->pos - 1, 1);
    case '(':
        return make_token(lx, TOK_LPAREN, lx->input + lx->pos - 1, 1);
    case ')':
        return make_token(lx, TOK_RPAREN, lx->input + lx->pos - 1, 1);
    case ':':
        if (lexer_peek(lx) == '-') {
            lexer_advance(lx);
            return make_token(lx, TOK_ARROW, lx->input + lx->pos - 2, 2);
        }
        return make_token(lx, TOK_COLON, lx->input + lx->pos - 1, 1);
    case '=':
        return make_token(lx, TOK_EQ, lx->input + lx->pos - 1, 1);
    case '!':
        if (lexer_peek(lx) == '=') {
            lexer_advance(lx);
            return make_token(lx, TOK_NE, lx->input + lx->pos - 2, 2);
        }
        return make_token(lx, TOK_INVALID, lx->input + lx->pos - 1, 1);
    case '<':
        if (lexer_peek(lx) == '=') {
            lexer_advance(lx);
            return make_token(lx, TOK_LE, lx->input + lx->pos - 2, 2);
        }
        if (lexer_peek(lx) == '>') {
            lexer_advance(lx);
            return make_token(lx, TOK_NE, lx->input + lx->pos - 2, 2);
        }
        return make_token(lx, TOK_LT, lx->input + lx->pos - 1, 1);
    case '>':
        if (lexer_peek(lx) == '=') {
            lexer_advance(lx);
            return make_token(lx, TOK_GE, lx->input + lx->pos - 2, 2);
        }
        return make_token(lx, TOK_GT, lx->input + lx->pos - 1, 1);
    case '"':
        return lex_string(lx);
    default:
        break;
    }

    if (isdigit(ch)) {
        return lex_number(lx, lx->input + lx->pos - 1);
    }
    if (is_ident_start(ch)) {
        return lex_ident(lx, lx->input + lx->pos - 1);
    }

    return make_token(lx, TOK_INVALID, lx->input + lx->pos - 1, 1);
}
