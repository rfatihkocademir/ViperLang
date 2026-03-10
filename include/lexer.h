#ifndef VIPER_LEXER_H
#define VIPER_LEXER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// Token Types for ViperLang v3
typedef enum {
    // Single-character tokens
    TOKEN_LEFT_PAREN, TOKEN_RIGHT_PAREN,
    TOKEN_LEFT_BRACE, TOKEN_RIGHT_BRACE,
    TOKEN_LEFT_BRACKET, TOKEN_RIGHT_BRACKET,
    TOKEN_COMMA, TOKEN_DOT, TOKEN_COLON,
    TOKEN_SEMICOLON, TOKEN_AT, TOKEN_HASH,
    
    // Math and Logical operators
    TOKEN_PLUS, TOKEN_MINUS, TOKEN_STAR, TOKEN_SLASH, TOKEN_PERCENT,
    TOKEN_EQUAL, TOKEN_EQUAL_EQUAL,
    TOKEN_BANG, TOKEN_BANG_EQUAL,
    TOKEN_LESS, TOKEN_LESS_EQUAL,
    TOKEN_GREATER, TOKEN_GREATER_EQUAL,
    TOKEN_AMPERSAND, TOKEN_PIPE, TOKEN_CARET,
    TOKEN_TILDE,
    
    // Strict ViperLang Operators
    TOKEN_PIPE_GREATER, // |>
    TOKEN_PIPE_QUESTION, // |?
    TOKEN_PLUS_TILDE,   // +~ (Wrapping Add)
    TOKEN_CARET_PLUS,   // ^+ (Saturating Add)
    TOKEN_QUESTION_DOT,      // ?.
    TOKEN_QUESTION_QUESTION, // ??
    TOKEN_QUESTION,          // ? (Error propagation)
    
    // Literals
    TOKEN_IDENTIFIER, TOKEN_STRING, TOKEN_NUMBER, TOKEN_REGEX,
    
    // ViperLang Keywords
    TOKEN_V, TOKEN_C, TOKEN_FN, TOKEN_R, TOKEN_I, TOKEN_EI, TOKEN_E,
    TOKEN_F, TOKEN_W, TOKEN_L, TOKEN_M, TOKEN_ST, TOKEN_EN, TOKEN_TR,
    TOKEN_IMPL, TOKEN_USE, TOKEN_TP, TOKEN_PUB, TOKEN_SPAWN,
    TOKEN_AS, TOKEN_ASYNC, TOKEN_AWAIT, TOKEN_MUT, TOKEN_IN,
    TOKEN_RET, TOKEN_PR,
    TOKEN_JSON, TOKEN_DB,
    TOKEN_TYPEOF, TOKEN_CLONE, TOKEN_SYNC, TOKEN_PANIC,
    TOKEN_RECOVER, TOKEN_TRY, TOKEN_ELSE, TOKEN_MATCH,
    TOKEN_EVAL, TOKEN_KEYS, TOKEN_HAS,

    // Specific Types (i, f, b, s, c, u8, any) handled as identifiers or keywords?
    // We treat them as identifiers that the parser will resolve, or as keywords.
    // Let's treat them as keywords for stricter parsing.
    TOKEN_TYPE_I, TOKEN_TYPE_F, TOKEN_TYPE_B, TOKEN_TYPE_S, 
    TOKEN_TYPE_C, TOKEN_TYPE_U8, TOKEN_TYPE_ANY,
    
    TOKEN_TRUE, TOKEN_FALSE, TOKEN_NIL,

    // End of File / Error
    TOKEN_ERROR, TOKEN_EOF
} TokenType;

typedef struct {
    TokenType type;
    const char* start;
    int length;
    int line;
} Token;

void init_lexer(const char* source);
Token scan_token();

#endif // VIPER_LEXER_H
