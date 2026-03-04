#include "ast.h"
#include <stdlib.h>
#include <stdio.h>

AstNode* ast_alloc(AstNodeType type) {
    AstNode* node = malloc(sizeof(AstNode));
    node->type = type;
    return node;
}

AstNode* ast_new_number(double val) {
    AstNode* node = ast_alloc(AST_NUMBER);
    node->data.number_val = val;
    return node;
}

AstNode* ast_new_string(Token str_token) {
    AstNode* node = ast_alloc(AST_STRING);
    // remove quotes
    node->data.str_val.name = str_token.start + 1;
    node->data.str_val.length = str_token.length - 2;
    return node;
}

AstNode* ast_new_identifier(Token name) {
    AstNode* node = ast_alloc(AST_IDENTIFIER);
    node->data.identifier.name = name;
    return node;
}

AstNode* ast_new_binary(AstNode* left, Token op, AstNode* right) {
    AstNode* node = ast_alloc(AST_BINARY_EXPR);
    node->data.binary.left = left;
    node->data.binary.op = op;
    node->data.binary.right = right;
    return node;
}

AstNode* ast_new_var_decl(Token name, Token type_annot, AstNode* initializer) {
    AstNode* node = ast_alloc(AST_VAR_DECL);
    node->data.var_decl.name = name;
    node->data.var_decl.type_annot = type_annot;
    node->data.var_decl.initializer = initializer;
    return node;
}

AstNode* ast_new_assign(Token name, AstNode* value) {
    AstNode* node = ast_alloc(AST_ASSIGN_EXPR);
    node->data.assign.name = name;
    node->data.assign.value = value;
    return node;
}

AstNode* ast_new_expr_stmt(AstNode* expr) {
    AstNode* node = ast_alloc(AST_EXPR_STMT);
    node->data.expr_stmt.expr = expr;
    return node;
}

AstNode* ast_new_block_stmt() {
    AstNode* node = ast_alloc(AST_BLOCK_STMT);
    node->data.block.statements = NULL;
    node->data.block.count = 0;
    node->data.block.capacity = 0;
    return node;
}

AstNode* ast_new_if_stmt(AstNode* condition, AstNode* then_branch, AstNode* else_branch) {
    AstNode* node = ast_alloc(AST_IF_STMT);
    node->data.if_stmt.condition = condition;
    node->data.if_stmt.then_branch = then_branch;
    node->data.if_stmt.else_branch = else_branch;
    return node;
}

AstNode* ast_new_while_stmt(AstNode* condition, AstNode* body) {
    AstNode* node = ast_alloc(AST_WHILE_STMT);
    node->data.while_stmt.condition = condition;
    node->data.while_stmt.body = body;
    return node;
}

AstNode* ast_new_func_decl(Token name, Token* params, int param_count, AstNode* body) {
    AstNode* node = ast_alloc(AST_FUNC_DECL);
    node->data.func_decl.name = name;
    node->data.func_decl.params = params;
    node->data.func_decl.param_count = param_count;
    node->data.func_decl.body = body;
    return node;
}

AstNode* ast_new_call_expr(Token name, AstNode** args, int arg_count) {
    AstNode* node = ast_alloc(AST_CALL_EXPR);
    node->data.call_expr.name = name;
    node->data.call_expr.args = args;
    node->data.call_expr.arg_count = arg_count;
    return node;
}

AstNode* ast_new_return_stmt(AstNode* value) {
    AstNode* node = ast_alloc(AST_RETURN_STMT);
    node->data.return_stmt.value = value;
    return node;
}

AstNode* ast_new_struct_decl(Token name, Token* fields, int field_count) {
    AstNode* node = ast_alloc(AST_STRUCT_DECL);
    node->data.struct_decl.name = name;
    node->data.struct_decl.fields = fields;
    node->data.struct_decl.field_count = field_count;
    return node;
}

AstNode* ast_new_use_stmt(Token path) {
    AstNode* node = ast_alloc(AST_USE_STMT);
    node->data.use_stmt.path = path;
    return node;
}

AstNode* ast_new_get_expr(AstNode* obj, Token name) {
    AstNode* node = ast_alloc(AST_GET_EXPR);
    node->data.get_expr.obj = obj;
    node->data.get_expr.name = name;
    return node;
}

AstNode* ast_new_set_expr(AstNode* obj, Token name, AstNode* value) {
    AstNode* node = ast_alloc(AST_SET_EXPR);
    node->data.set_expr.obj = obj;
    node->data.set_expr.name = name;
    node->data.set_expr.value = value;
    return node;
}

AstNode* ast_new_program() {
    AstNode* node = ast_alloc(AST_PROGRAM);
    node->data.block.statements = NULL;
    node->data.block.count = 0;
    node->data.block.capacity = 0;
    return node;
}

void ast_program_add(AstNode* program, AstNode* stmt) {
    if (program->data.block.count + 1 > program->data.block.capacity) {
        program->data.block.capacity = program->data.block.capacity < 8 ? 8 : program->data.block.capacity * 2;
        program->data.block.statements = realloc(program->data.block.statements, sizeof(AstNode*) * program->data.block.capacity);
    }
    program->data.block.statements[program->data.block.count++] = stmt;
}

void ast_print(AstNode* node, int depth) {
    if (!node) return;
    for(int i=0; i<depth; i++) printf("  ");
    
    switch(node->type) {
        case AST_PROGRAM:
            printf("Program:\n");
            for(int i=0; i<node->data.block.count; i++) {
                ast_print(node->data.block.statements[i], depth + 1);
            }
            break;
        case AST_VAR_DECL:
            printf("VarDecl(%.*s", node->data.var_decl.name.length, node->data.var_decl.name.start);
            if (node->data.var_decl.type_annot.length > 0) {
                printf(": %.*s", node->data.var_decl.type_annot.length, node->data.var_decl.type_annot.start);
            }
            printf("):\n");
            ast_print(node->data.var_decl.initializer, depth + 1);
            break;
        case AST_FUNC_DECL:
            printf("Function '%.*s' (%d params)\n", node->data.func_decl.name.length, node->data.func_decl.name.start, node->data.func_decl.param_count);
            ast_print(node->data.func_decl.body, depth + 1);
            break;
        case AST_STRUCT_DECL:
            printf("Struct '%.*s' (%d fields)\n", node->data.struct_decl.name.length, node->data.struct_decl.name.start, node->data.struct_decl.field_count);
            break;
        case AST_USE_STMT:
            printf("Use '%.*s'\n", node->data.use_stmt.path.length, node->data.use_stmt.path.start);
            break;
        case AST_GET_EXPR:
            printf("Get Field '%.*s'\n", node->data.get_expr.name.length, node->data.get_expr.name.start);
            ast_print(node->data.get_expr.obj, depth + 1);
            break;
        case AST_SET_EXPR:
            printf("Set Field '%.*s'\n", node->data.set_expr.name.length, node->data.set_expr.name.start);
            ast_print(node->data.set_expr.obj, depth + 1);
            ast_print(node->data.set_expr.value, depth + 1);
            break;
        case AST_ASSIGN_EXPR:
            printf("Assign(%.*s):\n", node->data.assign.name.length, node->data.assign.name.start);
            ast_print(node->data.assign.value, depth + 1);
            break;
        case AST_BLOCK_STMT:
            printf("BlockScope:\n");
            for(int i=0; i<node->data.block.count; i++) {
                ast_print(node->data.block.statements[i], depth + 1);
            }
            break;
        case AST_IF_STMT:
            printf("IfStmt:\n");
            ast_print(node->data.if_stmt.condition, depth + 1);
            printf("Then:\n");
            ast_print(node->data.if_stmt.then_branch, depth + 1);
            if (node->data.if_stmt.else_branch) {
                printf("Else:\n");
                ast_print(node->data.if_stmt.else_branch, depth + 1);
            }
            break;
        case AST_EXPR_STMT:
            printf("ExprStmt:\n");
            ast_print(node->data.expr_stmt.expr, depth + 1);
            break;
        case AST_BINARY_EXPR:
            printf("Binary(%.*s):\n", node->data.binary.op.length, node->data.binary.op.start);
            ast_print(node->data.binary.left, depth + 1);
            ast_print(node->data.binary.right, depth + 1);
            break;
        case AST_NUMBER:
            printf("Number(%.2f)\n", node->data.number_val);
            break;
        case AST_STRING:
            printf("String(\"%.*s\")\n", node->data.str_val.length, node->data.str_val.name);
            break;
        case AST_IDENTIFIER:
            printf("Id(%.*s)\n", node->data.identifier.name.length, node->data.identifier.name.start);
            break;
        default:
            printf("Unknown Node\n");
    }
}
