#ifndef VIPER_AST_H
#define VIPER_AST_H

#include "lexer.h"
#include <stdbool.h>

typedef enum {
    AST_NUMBER,
    AST_STRING,
    AST_IDENTIFIER,
    AST_BINARY_EXPR,
    AST_VAR_DECL,
    AST_ASSIGN_EXPR,
    AST_EXPR_STMT,
    AST_USE_STMT, // New
    AST_BLOCK_STMT,
    AST_IF_STMT,
    AST_WHILE_STMT,
    AST_FUNC_DECL,
    AST_STRUCT_DECL, // New
    AST_CALL_EXPR,
    AST_GET_EXPR,    // New
    AST_SET_EXPR,    // New
    AST_RETURN_STMT,
    AST_PROGRAM
} AstNodeType;

typedef struct AstNode {
    AstNodeType type;
    union {
        double number_val;
        struct {
            const char* name;
            int length;
        } str_val;
        struct {
            Token name;
            // For AST_IDENTIFIER
        } identifier;
        struct {
            struct AstNode* left;
            Token op;
            struct AstNode* right;
        } binary;
        struct {
            Token name;
            Token type_annot;
            struct AstNode* initializer;
        } var_decl;
        struct {
            Token name;
            struct AstNode* value;
        } assign;
        struct {
            struct AstNode* expr;
        } expr_stmt;
        struct {
            Token path; // For AST_USE_STMT
            Token alias;
        } use_stmt;
        struct {
            struct AstNode** statements;
            int count;
            int capacity;
        } block;
        struct {
            struct AstNode* condition;
            struct AstNode* then_branch;
            struct AstNode* else_branch;
        } if_stmt;
        struct {
            struct AstNode* condition;
            struct AstNode* body;
        } while_stmt;
        struct {
            Token name;
            Token* params;
            int param_count;
            struct AstNode* body;
            bool is_public;
        } func_decl;
        struct {
            struct AstNode* callee;
            Token name;
            struct AstNode** args;
            int arg_count;
        } call_expr;
        struct {
            struct AstNode* value;
        } return_stmt;
        struct {
            Token name;
            int field_count;
            Token* fields;
            bool is_public;
        } struct_decl;
        struct {
            struct AstNode* obj;
            Token name;
        } get_expr;
        struct {
            struct AstNode* obj;
            Token name;
            struct AstNode* value;
        } set_expr;
    } data;
} AstNode;

AstNode* ast_new_number(double val);
AstNode* ast_new_string(Token str_token);
AstNode* ast_new_identifier(Token name);
AstNode* ast_new_binary(AstNode* left, Token op, AstNode* right);
AstNode* ast_new_var_decl(Token name, Token type_annot, AstNode* initializer);
AstNode* ast_new_assign(Token name, AstNode* value);
AstNode* ast_new_expr_stmt(AstNode* expr);
AstNode* ast_new_block_stmt();
AstNode* ast_new_if_stmt(AstNode* condition, AstNode* then_branch, AstNode* else_branch);
AstNode* ast_new_while_stmt(AstNode* condition, AstNode* body);
AstNode* ast_new_func_decl(Token name, Token* params, int param_count, AstNode* body);
AstNode* ast_new_call_expr(AstNode* callee, Token name, AstNode** args, int arg_count);
AstNode* ast_new_return_stmt(AstNode* value);
AstNode* ast_new_struct_decl(Token name, Token* fields, int field_count);
AstNode* ast_new_use_stmt(Token path, Token alias);
AstNode* ast_new_get_expr(AstNode* obj, Token name);
AstNode* ast_new_set_expr(AstNode* obj, Token name, AstNode* value);
AstNode* ast_new_program();
void ast_program_add(AstNode* program, AstNode* stmt);

void ast_print(AstNode* node, int depth);

#endif // VIPER_AST_H
