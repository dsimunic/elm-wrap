#ifndef MINI_DATALOG_LEXER_H
#define MINI_DATALOG_LEXER_H

#include <stddef.h>

typedef enum {
    TOK_INVALID = 0,
    TOK_DOT,
    TOK_COMMA,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_COLON,
    TOK_PRED,
    TOK_CLEAR_DERIVED,
    TOK_NOT,
    TOK_ARROW,
    TOK_EQ,
    TOK_NE,         /* != or <> */
    TOK_LT,         /* < */
    TOK_LE,         /* <= */
    TOK_GT,         /* > */
    TOK_GE,         /* >= */
    TOK_IDENT,
    TOK_STRING,
    TOK_INT,
    TOK_WILDCARD,
    TOK_EOF
} TokenKind;

typedef struct {
    TokenKind kind;
    const char *lexeme;
    int length;
    long int_value;
    char *string_value;
    int line;
    int column;
} Token;

typedef struct {
    const char *input;
    size_t length;
    size_t pos;
    int line;
    int column;
    Token current;
    Token lookahead;
} Lexer;

void lexer_init(Lexer *lx, const char *input);
Token lexer_next(Lexer *lx);

#endif /* MINI_DATALOG_LEXER_H */
