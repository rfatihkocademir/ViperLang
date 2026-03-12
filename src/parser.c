#define _GNU_SOURCE
#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    Token current;
    Token previous;
    bool hadError;
    bool panicMode;
} Parser;

Parser parser;

static void parser_fatal_oom() {
    printf("Parser Error: Out of memory.\n");
    exit(1);
}

static bool ensure_capacity(void** ptr, int* cap, int needed, size_t elem_size) {
    if (*cap >= needed) return true;
    int next = (*cap == 0) ? 8 : *cap;
    while (next < needed) next *= 2;
    void* grown = realloc(*ptr, (size_t)next * elem_size);
    if (!grown) return false;
    *ptr = grown;
    *cap = next;
    return true;
}

static void advance_parser() {
    parser.previous = parser.current;
    for (;;) {
        parser.current = scan_token();
        if (parser.current.type != TOKEN_ERROR) break;
        printf("Syntax Error at line %d: %.*s\n", parser.current.line, parser.current.length, parser.current.start);
        parser.hadError = true;
    }
}

static bool check(TokenType type) {
    return parser.current.type == type;
}

static bool match_token(TokenType type) {
    if (!check(type)) return false;
    advance_parser();
    return true;
}

static void consume(TokenType type, const char* message) {
    if (parser.current.type == type) {
        advance_parser();
        return;
    }
    printf("Syntax Error on line %d: %s\n", parser.current.line, message);
    parser.hadError = true;
}

static AstNode* statement(); // Forward declaration

static AstNode* block() {
    AstNode* blockNode = ast_new_block_stmt();
    while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
        ast_program_add(blockNode, statement());
    }
    consume(TOKEN_RIGHT_BRACE, "Expected '}' at end of block.");
    return blockNode;
}

static AstNode* expression();
static AstNode* binary();

static bool is_field_name_token(TokenType type) {
    switch (type) {
        case TOKEN_IDENTIFIER:
        case TOKEN_KEYS:
        case TOKEN_HAS:
        case TOKEN_JSON:
        case TOKEN_TYPEOF:
        case TOKEN_CLONE:
        case TOKEN_EVAL:
        case TOKEN_MATCH:
        case TOKEN_SYNC:
            return true;
        default:
            return false;
    }
}

static bool is_decl_name_token(TokenType type) {
    switch (type) {
        case TOKEN_IDENTIFIER:
        case TOKEN_C:
        case TOKEN_R:
        case TOKEN_TYPE_B:
        case TOKEN_TYPE_S:
        case TOKEN_TYPE_I:
        case TOKEN_TYPE_F:
        case TOKEN_TYPE_U8:
        case TOKEN_TYPE_ANY:
        case TOKEN_KEYS:
        case TOKEN_HAS:
        case TOKEN_DB:
        case TOKEN_SYNC:
            return true;
        default:
            return false;
    }
}

static bool consume_field_name(Token* out_name, const char* message) {
    if (!is_field_name_token(parser.current.type)) {
        printf("Syntax Error on line %d: %s\n", parser.current.line, message);
        parser.hadError = true;
        return false;
    }
    advance_parser();
    if (out_name) *out_name = parser.previous;
    return true;
}

static bool consume_decl_name(Token* out_name, const char* message) {
    if (!is_decl_name_token(parser.current.type)) {
        printf("Syntax Error on line %d: %s\n", parser.current.line, message);
        parser.hadError = true;
        return false;
    }
    advance_parser();
    if (out_name) *out_name = parser.previous;
    return true;
}

static AstNode* if_statement() {
    AstNode* condition = expression();
    
    consume(TOKEN_LEFT_BRACE, "Expected '{' after if condition.");
    AstNode* thenBranch = block();
    
    AstNode* elseBranch = NULL;
    if (match_token(TOKEN_EI)) {
        elseBranch = if_statement(); // else if chain
    } else if (match_token(TOKEN_ELSE) || match_token(TOKEN_E)) {
        if (match_token(TOKEN_I)) {
            elseBranch = if_statement();
        } else {
            consume(TOKEN_LEFT_BRACE, "Expected '{' after 'else'.");
            elseBranch = block();
        }
    }
    
    return ast_new_if_stmt(condition, thenBranch, elseBranch);
}

static AstNode* primary() {
    if (match_token(TOKEN_NUMBER)) {
        double val = strtod(parser.previous.start, NULL);
        return ast_new_number(val);
    }
    if (match_token(TOKEN_STRING)) {
        return ast_new_string(parser.previous);
    }
    if (match_token(TOKEN_REGEX)) {
        return ast_new_regex(parser.previous);
    }

    if (match_token(TOKEN_LEFT_BRACKET)) {
        AstNode** elements = NULL;
        int count = 0;
        int cap = 0;

        if (!check(TOKEN_RIGHT_BRACKET)) {
            do {
                if (!ensure_capacity((void**)&elements, &cap, count + 1, sizeof(AstNode*))) parser_fatal_oom();
                elements[count++] = expression();
            } while (match_token(TOKEN_COMMA));
        }

        consume(TOKEN_RIGHT_BRACKET, "Expected ']' after array elements.");
        return ast_new_array_expr(elements, count);
    }

    if (match_token(TOKEN_LEFT_BRACE)) {
        return ast_new_string(parser.previous);
    }
    if (match_token(TOKEN_TRUE))  return ast_new_number(1.0); // Simple for now
    if (match_token(TOKEN_FALSE)) return ast_new_number(0.0);
    if (match_token(TOKEN_NIL))   return ast_new_nil();

    if (match_token(TOKEN_LEFT_PAREN)) {
        AstNode* expr = expression();
        consume(TOKEN_RIGHT_PAREN, "Expected ')' after expression.");
        return expr;
    }

    if (match_token(TOKEN_PR)) {
        Token prTok = parser.previous;
        consume(TOKEN_LEFT_PAREN, "Expected '(' after 'pr'.");
        AstNode** args = NULL;
        int arg_count = 0;
        int arg_cap = 0;
        if (!check(TOKEN_RIGHT_PAREN)) {
            do {
                if (!ensure_capacity((void**)&args, &arg_cap, arg_count + 1, sizeof(AstNode*))) {
                    free(args);
                    parser_fatal_oom();
                }
                args[arg_count++] = expression();
            } while (match_token(TOKEN_COMMA));
        }
        consume(TOKEN_RIGHT_PAREN, "Expected ')' after arguments.");
        AstNode* callee = ast_new_identifier(prTok);
        return ast_new_call_expr(callee, prTok, args, arg_count);
    }


    if (match_token(TOKEN_JSON)) {
        Token tok = parser.previous;
        AstNode** args = malloc(1 * sizeof(AstNode*));
        if (!args) parser_fatal_oom();
        args[0] = expression();
        AstNode* callee = ast_new_identifier(tok);
        AstNode* node = ast_new_call_expr(callee, tok, args, 1);
        return node;
    }


    if (match_token(TOKEN_SPAWN)) {
        AstNode* expr = expression();
        return ast_new_spawn_expr(expr);
    }

    if (match_token(TOKEN_AWAIT)) {
        AstNode* expr = expression();
        return ast_new_await_expr(expr);
    }

    if (match_token(TOKEN_TYPEOF)) {
        AstNode* expr = expression();
        return ast_new_typeof_expr(expr);
    }

    if (match_token(TOKEN_CLONE)) {
        AstNode* expr = expression();
        return ast_new_clone_expr(expr);
    }

    if (match_token(TOKEN_EVAL)) {
        AstNode* expr = NULL;
        if (match_token(TOKEN_LEFT_PAREN)) {
            expr = expression();
            consume(TOKEN_RIGHT_PAREN, "Expected ')' after eval argument.");
        } else {
            expr = expression();
        }
        return ast_new_eval_expr(expr);
    }

    if (match_token(TOKEN_KEYS)) {
        AstNode* expr = NULL;
        if (match_token(TOKEN_LEFT_PAREN)) {
            expr = expression();
            consume(TOKEN_RIGHT_PAREN, "Expected ')' after keys argument.");
        } else {
            expr = expression();
        }
        return ast_new_keys_expr(expr);
    }

    if (match_token(TOKEN_HAS)) {
        AstNode* obj = NULL;
        AstNode* prop = NULL;
        if (match_token(TOKEN_LEFT_PAREN)) {
            obj = expression();
            consume(TOKEN_COMMA, "Expected ',' after has object.");
            prop = expression();
            consume(TOKEN_RIGHT_PAREN, "Expected ')' after has arguments.");
        } else {
            obj = expression();
            prop = expression();
        }
        return ast_new_has_expr(obj, prop);
    }

    if (match_token(TOKEN_PANIC)) {
        Token tok = parser.previous;
        AstNode** args = malloc(1 * sizeof(AstNode*));
        if (!args) parser_fatal_oom();
        if (match_token(TOKEN_LEFT_PAREN)) {
            args[0] = expression();
            consume(TOKEN_RIGHT_PAREN, "Expected ')' after panic argument.");
        } else {
            args[0] = expression();
        }
        AstNode* callee = ast_new_identifier(tok);
        return ast_new_call_expr(callee, tok, args, 1);
    }
    
    if (match_token(TOKEN_RECOVER)) {
        Token tok = parser.previous;
        consume(TOKEN_LEFT_PAREN, "Expected '(' after 'recover'.");
        consume(TOKEN_RIGHT_PAREN, "Expected ')' after 'recover('.");
        AstNode* callee = ast_new_identifier(tok);
        return ast_new_call_expr(callee, tok, NULL, 0);
    }

    if (match_token(TOKEN_TRY)) {
        AstNode* try_block = expression();
        consume(TOKEN_ELSE, "Expected 'else' after try expression.");
        AstNode* catch_block = expression();
        return ast_new_try_expr(try_block, catch_block);
    }

    if (match_token(TOKEN_DB)) {
        Token db_kw = parser.previous;
        consume(TOKEN_DOT, "Expected '.' after 'db'.");
        Token op = (Token){0};
        if (!consume_decl_name(&op, "Expected database operation after 'db.'.")) {
            parser.hadError = true;
            return ast_new_nil();
        }
        
        char vdb_name[64];
        snprintf(vdb_name, sizeof(vdb_name), "vdb_%.*s", op.length, op.start);
        Token vdb_tok = op;
        vdb_tok.start = strdup(vdb_name);
        vdb_tok.length = strlen(vdb_name);

        AstNode** args = NULL;
        int count = 0;
        int cap = 0;

        if (match_token(TOKEN_LEFT_PAREN)) {
            if (!check(TOKEN_RIGHT_PAREN)) {
                do {
                    if (!ensure_capacity((void**)&args, &cap, count + 1, sizeof(AstNode*))) parser_fatal_oom();
                    args[count++] = expression();
                } while (match_token(TOKEN_COMMA));
            }
            consume(TOKEN_RIGHT_PAREN, "Expected ')' after db call arguments.");
        } else {
            while (parser.current.line == db_kw.line && !check(TOKEN_SEMICOLON) && !check(TOKEN_EOF) &&
                   !check(TOKEN_RIGHT_PAREN) && !check(TOKEN_RIGHT_BRACE) && !check(TOKEN_PIPE_GREATER)) {
                if (!ensure_capacity((void**)&args, &cap, count + 1, sizeof(AstNode*))) parser_fatal_oom();
                args[count++] = expression();
                if (check(TOKEN_COMMA)) match_token(TOKEN_COMMA);
            }
        }
        
        AstNode* callee = ast_new_identifier(vdb_tok);
        return ast_new_call_expr(callee, vdb_tok, args, count);
    }

    if (is_decl_name_token(parser.current.type)) {
        advance_parser();
        return ast_new_identifier(parser.previous);
    }

    printf("Syntax Error at line %d: Expected expression, got '%.*s'\n", 
           parser.current.line, parser.current.length, parser.current.start);
    parser.hadError = true;
    advance_parser();
    return NULL;
}

static AstNode* call() {
    AstNode* expr = primary();
    if (expr == NULL) {
        // Keep parser resilient after syntax errors in primary expressions.
        return ast_new_nil();
    }

    for (;;) {
        if (match_token(TOKEN_LEFT_PAREN)) {
            Token name;
            if (expr->type == AST_IDENTIFIER) {
                name = expr->data.identifier.name;
            } else if (expr->type == AST_GET_EXPR) {
                name = expr->data.get_expr.name;
            } else {
                name = (Token){TOKEN_ERROR, "anonymous", 9, parser.current.line};
            }

            AstNode** args = NULL;
            int arg_count = 0;
            int arg_cap = 0;
            if (!check(TOKEN_RIGHT_PAREN)) {
                do {
                    if (!ensure_capacity((void**)&args, &arg_cap, arg_count + 1, sizeof(AstNode*))) {
                        free(args);
                        parser_fatal_oom();
                    }
                    args[arg_count++] = expression();
                } while (match_token(TOKEN_COMMA));
            }
            consume(TOKEN_RIGHT_PAREN, "Expected ')' after function arguments.");
            expr = ast_new_call_expr(expr, name, args, arg_count);
        } else if (match_token(TOKEN_DOT)) {
            Token field = (Token){0};
            if (consume_field_name(&field, "Expected field name after '.'.")) {
                expr = ast_new_get_expr(expr, field);
            }
        } else if (match_token(TOKEN_LEFT_BRACKET)) {
            AstNode* index = expression();
            consume(TOKEN_RIGHT_BRACKET, "Expected ']' after index.");
            expr = ast_new_index_expr(expr, index);
        } else if (match_token(TOKEN_QUESTION_DOT)) {
            Token field = (Token){0};
            if (consume_field_name(&field, "Expected field name after '?.'.")) {
                expr = ast_new_safe_get_expr(expr, field);
            }
        } else if (match_token(TOKEN_QUESTION)) {
            expr = ast_new_error_propagate_expr(expr);
        } else {
            break;
        }
    }
    return expr;
}

static AstNode* assignment() {
    AstNode* expr = binary();

    if (match_token(TOKEN_EQUAL)) {
        AstNode* value = assignment();
        if (expr->type == AST_IDENTIFIER) {
            return ast_new_assign(expr->data.identifier.name, value);
        } else if (expr->type == AST_GET_EXPR) {
            return ast_new_set_expr(expr->data.get_expr.obj, expr->data.get_expr.name, value);
        } else if (expr->type == AST_INDEX_EXPR) {
            return ast_new_set_index_expr(expr->data.index_expr.target, expr->data.index_expr.index, value);
        }
        printf("Syntax Error on line %d: Invalid assignment target.\n", parser.current.line);
        parser.hadError = true;
    }
    return expr;
}

static AstNode* factor() {
    AstNode* expr = call();
    while (match_token(TOKEN_STAR) || match_token(TOKEN_SLASH)) {
        Token op = parser.previous;
        AstNode* right = call();
        expr = ast_new_binary(expr, op, right);
    }
    return expr;
}

static AstNode* term() {
    AstNode* expr = factor();
    while (match_token(TOKEN_PLUS) || match_token(TOKEN_MINUS) ||
           match_token(TOKEN_PLUS_TILDE) || match_token(TOKEN_CARET_PLUS) ||
           match_token(TOKEN_PIPE_GREATER) || match_token(TOKEN_PIPE_QUESTION)) {
        Token op = parser.previous;
        AstNode* right = factor();
        expr = ast_new_binary(expr, op, right);
    }
    return expr;
}

static AstNode* comparison() {
    AstNode* expr = term();
    while (match_token(TOKEN_EQUAL_EQUAL) || match_token(TOKEN_BANG_EQUAL) ||
           match_token(TOKEN_LESS) || match_token(TOKEN_GREATER) || 
           match_token(TOKEN_MATCH)) {
        Token op = parser.previous;
        AstNode* right = term();
        if (op.type == TOKEN_MATCH) {
            expr = ast_new_match_expr(expr, right);
        } else {
            expr = ast_new_binary(expr, op, right);
        }
    }
    return expr;
}

static AstNode* binary() {
    AstNode* expr = comparison();
    while (match_token(TOKEN_QUESTION_QUESTION)) {
        Token op = parser.previous;
        AstNode* right = comparison();
        expr = ast_new_binary(expr, op, right);
    }
    return expr;
}

static AstNode* expression() {
    return assignment();
}

static AstNode* var_declaration() {
    Token name = {0};
    (void)consume_decl_name(&name, "Expected variable name.");
    Token type_annot = {0};
    if (match_token(TOKEN_COLON)) {
        if (check(TOKEN_IDENTIFIER) ||
            check(TOKEN_TYPE_I) ||
            check(TOKEN_TYPE_F) ||
            check(TOKEN_TYPE_B) ||
            check(TOKEN_TYPE_S) ||
            check(TOKEN_TYPE_C) ||
            check(TOKEN_TYPE_U8) ||
            check(TOKEN_TYPE_ANY)) {
            advance_parser();
            type_annot = parser.previous;
        }
    }
    consume(TOKEN_EQUAL, "Expected '=' after variable name.");
    AstNode* initializer = expression();
    return ast_new_var_decl(name, type_annot, initializer);
}

static AstNode* use_statement() {
    consume(TOKEN_STRING, "Expected module path (string) after 'use'.");
    Token path = parser.previous;
    Token alias = (Token){0};
    if (match_token(TOKEN_AS)) {
        (void)consume_decl_name(&alias, "Expected namespace alias after 'as'.");
    }
    return ast_new_use_stmt(path, alias);
}

static AstNode* while_statement() {
    AstNode* condition = expression();
    consume(TOKEN_LEFT_BRACE, "Expected '{' after loop condition.");
    AstNode* body = block();
    return ast_new_while_stmt(condition, body);
}

static bool parse_effect_decorators(Token** out_effects, int* out_count) {
    Token* effects = NULL;
    int effect_count = 0;
    int effect_cap = 0;

    while (match_token(TOKEN_AT)) {
        Token decorator = {0};
        if (!consume_decl_name(&decorator, "Expected decorator name after '@'.")) {
            free(effects);
            return false;
        }

        if (!(decorator.length == 6 && memcmp(decorator.start, "effect", 6) == 0)) {
            printf("Syntax Error on line %d: Unknown decorator '@%.*s'.\n",
                   decorator.line, decorator.length, decorator.start);
            parser.hadError = true;
            free(effects);
            return false;
        }

        consume(TOKEN_LEFT_PAREN, "Expected '(' after '@effect'.");
        if (!check(TOKEN_RIGHT_PAREN)) {
            do {
                Token effect = {0};
                if (!consume_decl_name(&effect, "Expected effect name inside '@effect(...)'.")) {
                    free(effects);
                    return false;
                }
                if (!ensure_capacity((void**)&effects, &effect_cap,
                                     effect_count + 1, sizeof(Token))) {
                    free(effects);
                    parser_fatal_oom();
                }
                effects[effect_count++] = effect;
            } while (match_token(TOKEN_COMMA));
        }
        consume(TOKEN_RIGHT_PAREN, "Expected ')' after effect list.");
    }

    *out_effects = effects;
    *out_count = effect_count;
    return true;
}

static AstNode* func_declaration_with_effects(Token* effects, int effect_count) {
    Token name = {0};
    (void)consume_decl_name(&name, "Expected function name.");
    consume(TOKEN_LEFT_PAREN, "Expected '(' after function name.");
    Token* params = NULL;
    int param_count = 0;
    int param_cap = 0;
    if (!check(TOKEN_RIGHT_PAREN)) {
        do {
            Token param = {0};
            (void)consume_decl_name(&param, "Expected parameter name.");
            if (!ensure_capacity((void**)&params, &param_cap, param_count + 1, sizeof(Token))) {
                free(params);
                parser_fatal_oom();
            }
            params[param_count++] = param;
        } while (match_token(TOKEN_COMMA));
    }
    consume(TOKEN_RIGHT_PAREN, "Expected ')' after parameters.");
    
    // Optional return type annotations: `-> int`
    Token return_type = {0};
    if (match_token(TOKEN_MINUS)) {
        consume(TOKEN_GREATER, "Expected '>' after '-' for return type annotation (->).");
        if (check(TOKEN_IDENTIFIER) ||
            check(TOKEN_TYPE_I) || check(TOKEN_TYPE_F) ||
            check(TOKEN_TYPE_B) || check(TOKEN_TYPE_S) ||
            check(TOKEN_TYPE_C) || check(TOKEN_TYPE_U8) ||
            check(TOKEN_TYPE_ANY)) {
            advance_parser();
            return_type = parser.previous;
        } else {
            printf("Syntax Error on line %d: Expected return type after '->'.\n", parser.current.line);
            parser.hadError = true;
        }
    }
    
    consume(TOKEN_LEFT_BRACE, "Expected '{' before function body.");
    AstNode* body = block();
    return ast_new_func_decl(name, params, param_count, effects, effect_count, return_type, body);
}

static AstNode* func_declaration() {
    return func_declaration_with_effects(NULL, 0);
}

static AstNode* struct_declaration() {
    Token name = {0};
    (void)consume_decl_name(&name, "Expected struct name.");
    consume(TOKEN_LEFT_BRACE, "Expected '{' after struct name.");
    Token* fields = NULL;
    int field_count = 0;
    int field_cap = 0;
    while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
        Token field = {0};
        (void)consume_decl_name(&field, "Expected field name.");
        if (!ensure_capacity((void**)&fields, &field_cap, field_count + 1, sizeof(Token))) {
            free(fields);
            parser_fatal_oom();
        }
        fields[field_count++] = field;
    }
    consume(TOKEN_RIGHT_BRACE, "Expected '}' at end of struct declaration.");
    return ast_new_struct_decl(name, fields, field_count);
}

static AstNode* sync_statement() {
    consume(TOKEN_LEFT_BRACE, "Expected '{' to begin sync block.");
    AstNode* body = block();
    return ast_new_sync_stmt(body);
}

static AstNode* statement() {
    Token* effects = NULL;
    int effect_count = 0;
    if (check(TOKEN_AT)) {
        if (!parse_effect_decorators(&effects, &effect_count)) {
            return ast_new_expr_stmt(ast_new_nil());
        }
    }

    if (effect_count > 0) {
        bool is_public = match_token(TOKEN_PUB);
        if (match_token(TOKEN_FN)) {
            AstNode* fn = func_declaration_with_effects(effects, effect_count);
            fn->data.func_decl.is_public = is_public;
            return fn;
        }
        printf("Syntax Error on line %d: '@effect' can only be applied to function declarations.\n",
               parser.current.line);
        parser.hadError = true;
        free(effects);
        return ast_new_expr_stmt(ast_new_nil());
    }

    if (match_token(TOKEN_PUB)) {
        if (match_token(TOKEN_FN)) {
            AstNode* fn = func_declaration();
            fn->data.func_decl.is_public = true;
            return fn;
        }
        if (match_token(TOKEN_ST)) {
            AstNode* st = struct_declaration();
            st->data.struct_decl.is_public = true;
            return st;
        }
        printf("Syntax Error on line %d: Expected 'fn' or 'st' after 'pub'.\n", parser.current.line);
        parser.hadError = true;
    }
    if (match_token(TOKEN_LEFT_BRACE)) {
        return block();
    }
    if (match_token(TOKEN_I)) {
        return if_statement();
    }
    if (match_token(TOKEN_M)) {
        return while_statement();
    }
    if (match_token(TOKEN_TYPEOF)) {
        return ast_new_typeof_expr(expression());
    }
    if (match_token(TOKEN_CLONE)) {
        return ast_new_clone_expr(expression());
    }
    if (match_token(TOKEN_EVAL)) {
        return ast_new_eval_expr(expression());
    }
    if (match_token(TOKEN_KEYS)) {
        return ast_new_keys_expr(expression());
    }
    if (match_token(TOKEN_HAS)) {
        AstNode* obj = expression();
        AstNode* prop = expression();
        return ast_new_has_expr(obj, prop);
    }
    if (match_token(TOKEN_FN)) {
        return func_declaration();
    }
    if (match_token(TOKEN_ST)) {
        return struct_declaration();
    }
    if (match_token(TOKEN_USE)) {
        return use_statement();
    }
    if (match_token(TOKEN_SYNC)) {
        return sync_statement();
    }
    if (match_token(TOKEN_RET)) {
        AstNode* val = NULL;
        if (!check(TOKEN_EOF) && !check(TOKEN_RIGHT_BRACE)) {
            val = expression();
        }
        return ast_new_return_stmt(val);
    }
    if (match_token(TOKEN_V)) {
        return var_declaration();
    }
    AstNode* expr = expression();
    return ast_new_expr_stmt(expr);
}

AstNode* parse(const char* source) {
    init_lexer(source);
    parser.hadError = false;
    parser.panicMode = false;
    
    advance_parser();
    
    AstNode* program = ast_new_program();
    
    while (!match_token(TOKEN_EOF)) {
        ast_program_add(program, statement());
    }
    
    return program;
}

bool parser_had_error() {
    return parser.hadError;
}
