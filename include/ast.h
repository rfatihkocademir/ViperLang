#ifndef VIPER_AST_H
#define VIPER_AST_H

#include "lexer.h"
#include <stdbool.h>

typedef enum {
    AST_NUMBER,
    AST_STRING,
    AST_NIL,
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
    AST_SYNC_STMT,   // New: Core Async
    AST_CALL_EXPR,
    AST_GET_EXPR,    // New
    AST_SET_EXPR,    // New
    AST_SAFE_GET_EXPR, // New: ?. operator
    AST_ARRAY_EXPR,  // Array primitives
    AST_SPAWN_EXPR,  // New: Core Async
    AST_AWAIT_EXPR,  // New: Core Async
    AST_TRY_EXPR,    // New: Core Safety
    AST_TYPEOF_EXPR, // New: Core Types
    AST_CLONE_EXPR,  // New: Core Types
    AST_EVAL_EXPR,   // New: Core Dynamic
    AST_KEYS_EXPR,   // New: Core Dynamic
    AST_HAS_EXPR,    // New: Core Dynamic
    AST_INDEX_EXPR,  // New: arr[i]
    AST_REGEX,       // New: /abc/
    AST_MATCH_EXPR,  // New: s match /abc/
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
            struct AstNode* body;
        } sync_stmt;
        struct {
            Token name;
            Token* params;
            int param_count;
            Token return_type; // New: Gradual Typing -> type
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
        struct {
            struct AstNode* expr; // Typically CALL_EXPR
        } spawn_expr;
        struct {
            struct AstNode* expr; // Thread handle
        } await_expr;
        struct {
            struct AstNode** elements;
            int count;
        } array_expr;
        struct {
            struct AstNode* try_block;
            struct AstNode* catch_block;
        } try_expr;
        struct {
            struct AstNode* expr; // Target value
        } typeof_expr;
        struct {
            struct AstNode* expr; // Target value
        } clone_expr;
        struct {
            struct AstNode* obj;
            struct AstNode* prop;
        } has_expr;
        struct {
            struct AstNode* target;
            struct AstNode* index;
            struct AstNode* value; // For set_index
        } index_expr;
        struct {
            const char* pattern;
            int length;
        } regex;
        struct {
            struct AstNode* left;
            struct AstNode* right;
        } match_expr;
    } data;
} AstNode;

AstNode* ast_new_number(double val);
AstNode* ast_new_string(Token str_token);
AstNode* ast_new_nil();
AstNode* ast_new_identifier(Token name);
AstNode* ast_new_binary(AstNode* left, Token op, AstNode* right);
AstNode* ast_new_var_decl(Token name, Token type_annot, AstNode* initializer);
AstNode* ast_new_assign(Token name, AstNode* value);
AstNode* ast_new_expr_stmt(AstNode* expr);
AstNode* ast_new_block_stmt();
AstNode* ast_new_if_stmt(AstNode* condition, AstNode* then_branch, AstNode* else_branch);
AstNode* ast_new_while_stmt(AstNode* condition, AstNode* body);
AstNode* ast_new_sync_stmt(AstNode* body);
AstNode* ast_new_func_decl(Token name, Token* params, int param_count, Token return_type, AstNode* body);
AstNode* ast_new_call_expr(AstNode* callee, Token name, AstNode** args, int arg_count);
AstNode* ast_new_return_stmt(AstNode* value);
AstNode* ast_new_struct_decl(Token name, Token* fields, int field_count);
AstNode* ast_new_use_stmt(Token path, Token alias);
AstNode* ast_new_get_expr(AstNode* obj, Token name);
AstNode* ast_new_safe_get_expr(AstNode* obj, Token name);
AstNode* ast_new_set_expr(AstNode* obj, Token name, AstNode* value);
AstNode* ast_new_spawn_expr(AstNode* expr);
AstNode* ast_new_await_expr(AstNode* expr);
AstNode* ast_new_try_expr(AstNode* try_block, AstNode* catch_block);
AstNode* ast_new_array_expr(AstNode** elements, int count);
AstNode* ast_new_typeof_expr(AstNode* expr);
AstNode* ast_new_clone_expr(AstNode* expr);
AstNode* ast_new_eval_expr(AstNode* expr);
AstNode* ast_new_keys_expr(AstNode* expr);
AstNode* ast_new_has_expr(AstNode* obj, AstNode* prop);
AstNode* ast_new_index_expr(AstNode* target, AstNode* index);
AstNode* ast_new_set_index_expr(AstNode* target, AstNode* index, AstNode* value);
AstNode* ast_new_regex(Token token);
AstNode* ast_new_match_expr(AstNode* left, AstNode* right);
AstNode* ast_new_program();
void ast_program_add(AstNode* program, AstNode* stmt);

void ast_print(AstNode* node, int depth);

#endif // VIPER_AST_H
