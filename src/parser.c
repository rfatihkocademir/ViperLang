#include "parser.h"
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    Token current;
    Token previous;
    bool hadError;
    bool panicMode;
} Parser;

Parser parser;

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

static AstNode* if_statement() {
    AstNode* condition = expression();
    
    consume(TOKEN_LEFT_BRACE, "Expected '{' after if condition.");
    AstNode* thenBranch = block();
    
    AstNode* elseBranch = NULL;
    if (match_token(TOKEN_E)) {
        consume(TOKEN_LEFT_BRACE, "Expected '{' after 'e' (else).");
        elseBranch = block();
    } else if (match_token(TOKEN_EI)) {
        elseBranch = if_statement(); // else if chain
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
    if (match_token(TOKEN_TRUE))  return ast_new_number(1.0); // Simple for now
    if (match_token(TOKEN_FALSE)) return ast_new_number(0.0);
    if (match_token(TOKEN_NIL))   return ast_new_number(0.0);

    if (match_token(TOKEN_LEFT_PAREN)) {
        AstNode* expr = expression();
        consume(TOKEN_RIGHT_PAREN, "Expected ')' after expression.");
        return expr;
    }

    if (match_token(TOKEN_PR)) {
        Token prTok = parser.previous;
        consume(TOKEN_LEFT_PAREN, "Expected '(' after 'pr'.");
        AstNode* args[32];
        int arg_count = 0;
        if (!check(TOKEN_RIGHT_PAREN)) {
            do {
                args[arg_count++] = expression();
            } while (match_token(TOKEN_COMMA));
        }
        consume(TOKEN_RIGHT_PAREN, "Expected ')' after arguments.");
        AstNode** heapArgs = NULL;
        if (arg_count > 0) {
            heapArgs = malloc(sizeof(AstNode*) * arg_count);
            for (int i = 0; i < arg_count; i++) heapArgs[i] = args[i];
        }
        return ast_new_call_expr(prTok, heapArgs, arg_count);
    }

    if (match_token(TOKEN_IDENTIFIER)) {
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

            AstNode* args[32];
            int arg_count = 0;
            if (!check(TOKEN_RIGHT_PAREN)) {
                do {
                    args[arg_count++] = expression();
                } while (match_token(TOKEN_COMMA));
            }
            consume(TOKEN_RIGHT_PAREN, "Expected ')' after function arguments.");

            AstNode** heapArgs = NULL;
            if (arg_count > 0) {
                heapArgs = malloc(sizeof(AstNode*) * arg_count);
                for (int i = 0; i < arg_count; i++) heapArgs[i] = args[i];
            }
            expr = ast_new_call_expr(name, heapArgs, arg_count);
        } else if (match_token(TOKEN_DOT)) {
            consume(TOKEN_IDENTIFIER, "Expected field name after '.'.");
            expr = ast_new_get_expr(expr, parser.previous);
        } else {
            break;
        }
    }
    return expr;
}

static AstNode* assignment() {
    AstNode* expr = call();

    if (match_token(TOKEN_EQUAL)) {
        AstNode* value = assignment();
        if (expr->type == AST_IDENTIFIER) {
            return ast_new_assign(expr->data.identifier.name, value);
        } else if (expr->type == AST_GET_EXPR) {
            return ast_new_set_expr(expr->data.get_expr.obj, expr->data.get_expr.name, value);
        }
        printf("Syntax Error on line %d: Invalid assignment target.\n", parser.current.line);
        parser.hadError = true;
    }
    return expr;
}

static AstNode* binary() {
    AstNode* exp = assignment();
    while (match_token(TOKEN_PLUS) || match_token(TOKEN_MINUS) || 
           match_token(TOKEN_STAR) || match_token(TOKEN_SLASH) ||
           match_token(TOKEN_EQUAL_EQUAL) || match_token(TOKEN_LESS) ||
           match_token(TOKEN_GREATER) ||
           match_token(TOKEN_PLUS_TILDE) || match_token(TOKEN_CARET_PLUS) ||
           match_token(TOKEN_PIPE_GREATER) || match_token(TOKEN_PIPE_QUESTION)) {
        Token op = parser.previous;
        AstNode* right = assignment();
        exp = ast_new_binary(exp, op, right);
    }
    return exp;
}

static AstNode* expression() {
    return binary();
}

static AstNode* var_declaration() {
    consume(TOKEN_IDENTIFIER, "Expected variable name.");
    Token name = parser.previous;
    Token type_annot = {0};
    if (match_token(TOKEN_COLON)) {
        if (match_token(TOKEN_IDENTIFIER) || match_token(TOKEN_I) || 
            match_token(TOKEN_TYPE_S) || match_token(TOKEN_TYPE_F)) {
            type_annot = parser.previous;
        }
    }
    consume(TOKEN_EQUAL, "Expected '=' after variable name.");
    AstNode* initializer = expression();
    return ast_new_var_decl(name, type_annot, initializer);
}

static AstNode* use_statement() {
    consume(TOKEN_STRING, "Expected module path (string) after 'use'.");
    return ast_new_use_stmt(parser.previous);
}

static AstNode* while_statement() {
    AstNode* condition = expression();
    consume(TOKEN_LEFT_BRACE, "Expected '{' after loop condition.");
    AstNode* body = block();
    return ast_new_while_stmt(condition, body);
}

static AstNode* func_declaration() {
    consume(TOKEN_IDENTIFIER, "Expected function name.");
    Token name = parser.previous;
    consume(TOKEN_LEFT_PAREN, "Expected '(' after function name.");
    Token params[32];
    int param_count = 0;
    if (!check(TOKEN_RIGHT_PAREN)) {
        do {
            consume(TOKEN_IDENTIFIER, "Expected parameter name.");
            params[param_count++] = parser.previous;
        } while (match_token(TOKEN_COMMA));
    }
    consume(TOKEN_RIGHT_PAREN, "Expected ')' after parameters.");
    consume(TOKEN_LEFT_BRACE, "Expected '{' before function body.");
    AstNode* body = block();
    Token* heapParams = NULL;
    if (param_count > 0) {
        heapParams = malloc(sizeof(Token) * param_count);
        for (int i = 0; i < param_count; i++) heapParams[i] = params[i];
    }
    return ast_new_func_decl(name, heapParams, param_count, body);
}

static AstNode* struct_declaration() {
    consume(TOKEN_IDENTIFIER, "Expected struct name.");
    Token name = parser.previous;
    consume(TOKEN_LEFT_BRACE, "Expected '{' after struct name.");
    Token fields[64];
    int field_count = 0;
    while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
        consume(TOKEN_IDENTIFIER, "Expected field name.");
        fields[field_count++] = parser.previous;
    }
    consume(TOKEN_RIGHT_BRACE, "Expected '}' at end of struct declaration.");
    Token* heapFields = NULL;
    if (field_count > 0) {
        heapFields = malloc(sizeof(Token) * field_count);
        for (int i = 0; i < field_count; i++) heapFields[i] = fields[i];
    }
    return ast_new_struct_decl(name, heapFields, field_count);
}

static AstNode* statement() {
    if (match_token(TOKEN_LEFT_BRACE)) {
        return block();
    }
    if (match_token(TOKEN_I)) {
        return if_statement();
    }
    if (match_token(TOKEN_M)) {
        return while_statement();
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
