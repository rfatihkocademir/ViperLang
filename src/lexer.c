#include "lexer.h"
#include <string.h>

typedef struct {
    const char* start;
    const char* current;
    int line;
    TokenType last_type;
} Lexer;

Lexer lexer;

void init_lexer(const char* source) {
    lexer.start = source;
    lexer.current = source;
    lexer.line = 1;
    lexer.last_type = TOKEN_EOF;
}

static bool is_at_end() {
    return *lexer.current == '\0';
}

static char advance() {
    lexer.current++;
    return lexer.current[-1];
}

static char peek() {
    return *lexer.current;
}

static char peek_next() {
    if (is_at_end()) return '\0';
    return lexer.current[1];
}

static bool match(char expected) {
    if (is_at_end()) return false;
    if (*lexer.current != expected) return false;
    lexer.current++;
    return true;
}

static Token make_token(TokenType type) {
    lexer.last_type = type;
    Token token;
    token.type = type;
    token.start = lexer.start;
    token.length = (int)(lexer.current - lexer.start);
    token.line = lexer.line;
    return token;
}

static Token error_token(const char* message) {
    Token token;
    token.type = TOKEN_ERROR;
    token.start = message;
    token.length = (int)strlen(message);
    token.line = lexer.line;
    return token;
}

static void skip_whitespace() {
    for (;;) {
        char c = peek();
        switch (c) {
            case ' ':
            case '\r':
            case '\t':
                advance();
                break;
            case '\n':
                lexer.line++;
                advance();
                break;
            case '/':
                if (peek_next() == '/') {
                    // Single line comment
                    while (peek() != '\n' && !is_at_end()) advance();
                } else if (peek_next() == '*') {
                    // Block comment
                    advance(); advance();
                    while (!is_at_end()) {
                        if (peek() == '\n') lexer.line++;
                        if (peek() == '*' && peek_next() == '/') {
                            advance(); advance();
                            break;
                        }
                        advance();
                    }
                } else {
                    return;
                }
                break;
            default:
                return;
        }
    }
}

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

static bool is_alpha(char c) {
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
            c == '_';
}

static TokenType check_keyword(int start, int length, const char* rest, TokenType type) {
    if (lexer.current - lexer.start == start + length &&
        memcmp(lexer.start + start, rest, length) == 0) {
        return type;
    }
    return TOKEN_IDENTIFIER;
}

static TokenType identifier_type() {
    int len = lexer.current - lexer.start;
    
    // Single letter keywords
    if (len == 1) {
        switch (lexer.start[0]) {
            case 'v': return TOKEN_V;
            case 'c': return TOKEN_C;
            case 'r': return TOKEN_R;
            case 'i': return TOKEN_I;
            case 'e': return TOKEN_E;
            case 'f': return TOKEN_F;
            case 'w': return TOKEN_W;
            case 'l': return TOKEN_L;
            case 'm': return TOKEN_M;
            case 'b': return TOKEN_TYPE_B;
            case 's': return TOKEN_TYPE_S;
        }
    }
    
    // Two letter keywords
    if (len == 2) {
        if (memcmp(lexer.start, "as", 2) == 0) return TOKEN_AS;
        if (memcmp(lexer.start, "fn", 2) == 0) return TOKEN_FN;
        if (memcmp(lexer.start, "ei", 2) == 0) return TOKEN_EI;
        if (memcmp(lexer.start, "st", 2) == 0) return TOKEN_ST;
        if (memcmp(lexer.start, "en", 2) == 0) return TOKEN_EN;
        if (memcmp(lexer.start, "tr", 2) == 0) return TOKEN_TR;
        if (memcmp(lexer.start, "tp", 2) == 0) return TOKEN_TP;
        if (memcmp(lexer.start, "in", 2) == 0) return TOKEN_IN;
        if (memcmp(lexer.start, "u8", 2) == 0) return TOKEN_TYPE_U8;
        if (memcmp(lexer.start, "pr", 2) == 0) return TOKEN_PR;
        if (memcmp(lexer.start, "db", 2) == 0) return TOKEN_DB;
    }

    if (len == 3) {
        if (memcmp(lexer.start, "ret", 3) == 0) return TOKEN_RET;
    }
    
    // Multi letter keywords
    switch (lexer.start[0]) {
        case 'a': 
            if (len > 1) {
                switch(lexer.start[1]) {
                    case 'n': return check_keyword(2, 1, "y", TOKEN_TYPE_ANY);
                    case 's': return check_keyword(2, 3, "ync", TOKEN_ASYNC);
                    case 'w': return check_keyword(2, 3, "ait", TOKEN_AWAIT);
                }
            }
            break;
        case 'c': return check_keyword(1, 4, "lone", TOKEN_CLONE);
        case 'e':
            if (len > 1) {
                switch(lexer.start[1]) {
                    case 'l': return check_keyword(2, 2, "se", TOKEN_ELSE);
                    case 'v': return check_keyword(2, 2, "al", TOKEN_EVAL);
                }
            }
            break;
        case 'f':
            if (len > 1) {
                switch(lexer.start[1]) {
                    case 'a': return check_keyword(2, 3, "lse", TOKEN_FALSE);
                    case 'n': return check_keyword(2, 0, "", TOKEN_FN);
                }
            }
            break;
        case 'h':
            return check_keyword(1, 2, "as", TOKEN_HAS);
        case 'i':
            if (len > 1) {
                switch(lexer.start[1]) {
                    case 'm': return check_keyword(2, 2, "pl", TOKEN_IMPL);
                    case 'n': return check_keyword(2, 0, "", TOKEN_IN);
                }
            }
            break;
        case 'j': return check_keyword(1, 3, "son", TOKEN_JSON);
        case 'k': return check_keyword(1, 3, "eys", TOKEN_KEYS);
        case 'm': 
            if (len > 1) {
                switch(lexer.start[1]) {
                    case 'u': return check_keyword(2, 1, "t", TOKEN_MUT);
                    case 'a': return check_keyword(2, 3, "tch", TOKEN_MATCH);
                }
            }
            break;
        case 'n': return check_keyword(1, 2, "il", TOKEN_NIL);
        case 'p': 
            if (len > 1) {
                switch(lexer.start[1]) {
                    case 'u': return check_keyword(2, 1, "b", TOKEN_PUB);
                    case 'a': return check_keyword(2, 3, "nic", TOKEN_PANIC);
                }
            }
            break;
        case 'q': break;
        case 'r':
            if (len > 1) {
                if (lexer.start[1] == 'e') {
                    if (len == 3) return check_keyword(2, 1, "t", TOKEN_RET);
                    return check_keyword(2, 5, "cover", TOKEN_RECOVER);
                }
            }
            break;
        case 's':
            if (len > 1) {
                switch(lexer.start[1]) {
                    case 'p': return check_keyword(2, 3, "awn", TOKEN_SPAWN);
                    case 'y': return check_keyword(2, 2, "nc", TOKEN_SYNC);
                }
            }
            break;
        case 't': 
            if (len > 1) {
                switch(lexer.start[1]) {
                    case 'r': 
                        if (len == 3) return check_keyword(2, 1, "y", TOKEN_TRY);
                        if (len == 4) return check_keyword(2, 2, "ue", TOKEN_TRUE);
                        break;
                    case 'y': return check_keyword(2, 4, "peof", TOKEN_TYPEOF);
                }
            }
            break;
        case 'u': return check_keyword(1, 2, "se", TOKEN_USE);
    }
    return TOKEN_IDENTIFIER;
}

static Token identifier() {
    while (is_alpha(peek()) || is_digit(peek())) advance();
    return make_token(identifier_type());
}

static Token number() {
    while (is_digit(peek())) advance();

    // Look for a fractional part.
    if (peek() == '.' && is_digit(peek_next())) {
        // Consume the "."
        advance();
        while (is_digit(peek())) advance();
    }

    return make_token(TOKEN_NUMBER);
}

static Token string() {
    while ((peek() != '"' || (lexer.current > lexer.start && *(lexer.current - 1) == '\\')) && !is_at_end()) {
        if (peek() == '\n') lexer.line++;
        advance();
    }

    if (is_at_end()) return error_token("Unterminated string.");
    advance(); // The closing quote.
    return make_token(TOKEN_STRING);
}

static Token regex() {
    while (peek() != '/' && !is_at_end()) {
        if (peek() == '\\') advance(); // skip escaped char
        if (peek() == '\n') lexer.line++;
        advance();
    }

    if (is_at_end()) return error_token("Unterminated regex.");
    advance(); // The closing slash.
    return make_token(TOKEN_REGEX);
}

Token scan_token() {
    skip_whitespace();
    lexer.start = lexer.current;

    if (is_at_end()) return make_token(TOKEN_EOF);

    char c = advance();

    if (is_alpha(c)) return identifier();
    if (is_digit(c)) return number();

    switch (c) {
        case '(': return make_token(TOKEN_LEFT_PAREN);
        case ')': return make_token(TOKEN_RIGHT_PAREN);
        case '{': return make_token(TOKEN_LEFT_BRACE);
        case '}': return make_token(TOKEN_RIGHT_BRACE);
        case '[': return make_token(TOKEN_LEFT_BRACKET);
        case ']': return make_token(TOKEN_RIGHT_BRACKET);
        case ';': return make_token(TOKEN_SEMICOLON);
        case ',': return make_token(TOKEN_COMMA);
        case '?':
            if (match('?')) return make_token(TOKEN_QUESTION_QUESTION);
            if (match('.')) return make_token(TOKEN_QUESTION_DOT);
            return make_token(TOKEN_QUESTION);
        case ':': return make_token(TOKEN_COLON);
        case '@': return make_token(TOKEN_AT);
        case '#': return make_token(TOKEN_HASH);
        case '.': return make_token(TOKEN_DOT);
        
        // Multi-character operators
        case '-': return make_token(TOKEN_MINUS);
        case '/':
            // Heuristic for regex: if it's after an operator or at start of expression
            if (lexer.last_type == TOKEN_EQUAL || lexer.last_type == TOKEN_LEFT_PAREN  ||
                lexer.last_type == TOKEN_COMMA || lexer.last_type == TOKEN_COLON      ||
                lexer.last_type == TOKEN_BANG  || lexer.last_type == TOKEN_AMPERSAND  ||
                lexer.last_type == TOKEN_PIPE  || lexer.last_type == TOKEN_EOF        ||
                lexer.last_type == TOKEN_MATCH) {
                return regex();
            }
            return make_token(TOKEN_SLASH);
        case '%': return make_token(TOKEN_PERCENT);
        
        case '*': return make_token(TOKEN_STAR);
        
        case '^':
            return make_token(match('+') ? TOKEN_CARET_PLUS : TOKEN_CARET);
        case '~':
            return make_token(TOKEN_TILDE);

        case '+':
            return make_token(match('~') ? TOKEN_PLUS_TILDE : TOKEN_PLUS);

        case '!':
            return make_token(match('=') ? TOKEN_BANG_EQUAL : TOKEN_BANG);

        case '=':
            return make_token(match('=') ? TOKEN_EQUAL_EQUAL : TOKEN_EQUAL);

        case '<':
            return make_token(match('=') ? TOKEN_LESS_EQUAL : TOKEN_LESS);

        case '>':
            return make_token(match('=') ? TOKEN_GREATER_EQUAL : TOKEN_GREATER);

        case '&':
            return make_token(match('&') ? TOKEN_AMPERSAND : TOKEN_ERROR); // && or bitwise & unsupported for now maybe

        case '|':
            if (match('>')) return make_token(TOKEN_PIPE_GREATER);
            if (match('?')) return make_token(TOKEN_PIPE_QUESTION);
            if (match('|')) return make_token(TOKEN_PIPE); // ||
            return make_token(TOKEN_PIPE);

        case '"': return string();
    }

    return error_token("Unexpected character.");
}
