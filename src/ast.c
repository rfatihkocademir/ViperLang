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
    node->data.str_val.name = str_token.start + 1;
    node->data.str_val.length = str_token.length - 2;
    return node;
}

AstNode* ast_new_nil() {
    return ast_alloc(AST_NIL);
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

AstNode* ast_new_sync_stmt(AstNode* body) {
    AstNode* node = ast_alloc(AST_SYNC_STMT);
    node->data.sync_stmt.body = body;
    return node;
}

AstNode* ast_new_func_decl(Token name, Token* params, int param_count,
                           Token* effects, int effect_count,
                           Token return_type, AstNode* body) {
    AstNode* node = ast_alloc(AST_FUNC_DECL);
    node->data.func_decl.name = name;
    node->data.func_decl.params = params;
    node->data.func_decl.param_count = param_count;
    node->data.func_decl.effects = effects;
    node->data.func_decl.effect_count = effect_count;
    node->data.func_decl.return_type = return_type;
    node->data.func_decl.body = body;
    node->data.func_decl.is_public = false;
    return node;
}

AstNode* ast_new_call_expr(AstNode* callee, Token name, AstNode** args, int arg_count) {
    AstNode* node = ast_alloc(AST_CALL_EXPR);
    node->data.call_expr.callee = callee;
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
    node->data.struct_decl.is_public = false;
    return node;
}

AstNode* ast_new_use_stmt(Token path, Token alias) {
    AstNode* node = ast_alloc(AST_USE_STMT);
    node->data.use_stmt.path = path;
    node->data.use_stmt.alias = alias;
    return node;
}

AstNode* ast_new_get_expr(AstNode* obj, Token name) {
    AstNode* node = ast_alloc(AST_GET_EXPR);
    node->data.get_expr.obj = obj;
    node->data.get_expr.name = name;
    return node;
}

AstNode* ast_new_safe_get_expr(AstNode* obj, Token name) {
    AstNode* node = ast_alloc(AST_SAFE_GET_EXPR);
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

AstNode* ast_new_spawn_expr(AstNode* expr) {
    AstNode* node = ast_alloc(AST_SPAWN_EXPR);
    node->data.spawn_expr.expr = expr;
    return node;
}

AstNode* ast_new_await_expr(AstNode* expr) {
    AstNode* node = ast_alloc(AST_AWAIT_EXPR);
    node->data.await_expr.expr = expr;
    return node;
}

AstNode* ast_new_typeof_expr(AstNode* expr) {
    AstNode* node = ast_alloc(AST_TYPEOF_EXPR);
    node->data.typeof_expr.expr = expr;
    return node;
}

AstNode* ast_new_array_expr(AstNode** elements, int count) {
    AstNode* node = ast_alloc(AST_ARRAY_EXPR);
    node->data.array_expr.elements = elements;
    node->data.array_expr.count = count;
    return node;
}

AstNode* ast_new_has_expr(AstNode* obj, AstNode* prop) {
    AstNode* node = ast_alloc(AST_HAS_EXPR);
    node->data.has_expr.obj = obj;
    node->data.has_expr.prop = prop;
    return node;
}

AstNode* ast_new_index_expr(AstNode* target, AstNode* index) {
    AstNode* node = ast_alloc(AST_INDEX_EXPR);
    node->data.index_expr.target = target;
    node->data.index_expr.index = index;
    node->data.index_expr.value = NULL;
    return node;
}

AstNode* ast_new_set_index_expr(AstNode* target, AstNode* index, AstNode* value) {
    AstNode* node = ast_alloc(AST_INDEX_EXPR);
    node->data.index_expr.target = target;
    node->data.index_expr.index = index;
    node->data.index_expr.value = value;
    return node;
}

AstNode* ast_new_regex(Token token) {
    AstNode* node = ast_alloc(AST_REGEX);
    node->data.regex.pattern = token.start + 1;
    node->data.regex.length = token.length - 2;
    return node;
}

AstNode* ast_new_match_expr(AstNode* left, AstNode* right) {
    AstNode* node = ast_alloc(AST_MATCH_EXPR);
    node->data.match_expr.left = left;
    node->data.match_expr.right = right;
    return node;
}

AstNode* ast_new_clone_expr(AstNode* expr) {
    AstNode* node = ast_alloc(AST_CLONE_EXPR);
    node->data.clone_expr.expr = expr;
    return node;
}

AstNode* ast_new_keys_expr(AstNode* expr) {
    AstNode* node = ast_alloc(AST_KEYS_EXPR);
    node->data.clone_expr.expr = expr;
    return node;
}

AstNode* ast_new_eval_expr(AstNode* expr) {
    AstNode* node = ast_alloc(AST_EVAL_EXPR);
    node->data.clone_expr.expr = expr;
    return node;
}

AstNode* ast_new_try_expr(AstNode* try_block, AstNode* catch_block) {
    AstNode* node = ast_alloc(AST_TRY_EXPR);
    node->data.try_expr.try_block = try_block;
    node->data.try_expr.catch_block = catch_block;
    return node;
}

AstNode* ast_new_error_propagate_expr(AstNode* expr) {
    AstNode* node = ast_alloc(AST_ERROR_PROPAGATE_EXPR);
    node->data.error_propagate.expr = expr;
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
            printf("%sFunction '%.*s' (%d params",
                   node->data.func_decl.is_public ? "Pub " : "",
                   node->data.func_decl.name.length, node->data.func_decl.name.start,
                   node->data.func_decl.param_count);
            if (node->data.func_decl.effect_count > 0) {
                printf(", effects=");
                for (int i = 0; i < node->data.func_decl.effect_count; i++) {
                    printf("%s%.*s",
                           (i == 0) ? "" : "|",
                           node->data.func_decl.effects[i].length,
                           node->data.func_decl.effects[i].start);
                }
            }
            printf(")\n");
            ast_print(node->data.func_decl.body, depth + 1);
            break;
        case AST_STRUCT_DECL:
            printf("%sStruct '%.*s' (%d fields)\n",
                   node->data.struct_decl.is_public ? "Pub " : "",
                   node->data.struct_decl.name.length, node->data.struct_decl.name.start,
                   node->data.struct_decl.field_count);
            break;
        case AST_USE_STMT:
            if (node->data.use_stmt.alias.length > 0) {
                printf("Use '%.*s' as '%.*s'\n",
                       node->data.use_stmt.path.length, node->data.use_stmt.path.start,
                       node->data.use_stmt.alias.length, node->data.use_stmt.alias.start);
            } else {
                printf("Use '%.*s'\n", node->data.use_stmt.path.length, node->data.use_stmt.path.start);
            }
            break;
        case AST_GET_EXPR:
            printf("Get (%.*s):\n", node->data.get_expr.name.length, node->data.get_expr.name.start);
            ast_print(node->data.get_expr.obj, depth + 1);
            break;
        case AST_SAFE_GET_EXPR:
            printf("SafeGet (%.*s):\n", node->data.get_expr.name.length, node->data.get_expr.name.start);
            ast_print(node->data.get_expr.obj, depth + 1);
            break;
        case AST_SET_EXPR:
            printf("Set Field '%.*s'\n", node->data.set_expr.name.length, node->data.set_expr.name.start);
            ast_print(node->data.set_expr.obj, depth + 1);
            ast_print(node->data.set_expr.value, depth + 1);
            break;
        case AST_SPAWN_EXPR:
            printf("Spawn:\n");
            ast_print(node->data.spawn_expr.expr, depth + 1);
            break;
        case AST_AWAIT_EXPR:
            printf("Await:\n");
            ast_print(node->data.await_expr.expr, depth + 1);
            break;
        case AST_TYPEOF_EXPR:
            printf("Typeof:\n");
            ast_print(node->data.typeof_expr.expr, depth + 1);
            break;
        case AST_ARRAY_EXPR:
            printf("Array [%d elements]:\n", node->data.array_expr.count);
            for(int i=0; i<node->data.array_expr.count; i++) {
                ast_print(node->data.array_expr.elements[i], depth + 1);
            }
            break;
        case AST_TRY_EXPR:
            printf("Try:\n");
            ast_print(node->data.try_expr.try_block, depth + 1);
            printf("Else:\n");
            ast_print(node->data.try_expr.catch_block, depth + 1);
            break;
        case AST_CLONE_EXPR:
            printf("Clone:\n");
            ast_print(node->data.clone_expr.expr, depth + 1);
            break;
        case AST_EVAL_EXPR:
            printf("Eval:\n");
            ast_print(node->data.clone_expr.expr, depth + 1);
            break;
        case AST_KEYS_EXPR:
            printf("Keys:\n");
            ast_print(node->data.clone_expr.expr, depth + 1);
            break;
        case AST_HAS_EXPR:
            printf("Has:\n");
            ast_print(node->data.has_expr.obj, depth + 1);
            ast_print(node->data.has_expr.prop, depth + 1);
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
        case AST_WHILE_STMT:
            printf("While:\n");
            ast_print(node->data.while_stmt.condition, depth + 1);
            ast_print(node->data.while_stmt.body, depth + 1);
            break;
        case AST_SYNC_STMT:
            printf("Sync:\n");
            ast_print(node->data.sync_stmt.body, depth + 1);
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
            printf("String(%.*s)\n", node->data.str_val.length, node->data.str_val.name);
            break;
        case AST_NIL:
            printf("Nil\n");
            break;
        case AST_IDENTIFIER:
            printf("Id(%.*s)\n", node->data.identifier.name.length, node->data.identifier.name.start);
            break;
        case AST_INDEX_EXPR:
            printf("Index:\n");
            ast_print(node->data.index_expr.target, depth + 1);
            ast_print(node->data.index_expr.index, depth + 1);
            if (node->data.index_expr.value) {
                printf(" (Set)\n");
                ast_print(node->data.index_expr.value, depth + 1);
            }
            break;
        case AST_REGEX:
            printf("Regex(/%.*s/)\n", node->data.regex.length, node->data.regex.pattern);
            break;
        case AST_MATCH_EXPR:
            printf("Match:\n");
            ast_print(node->data.match_expr.left, depth + 1);
            ast_print(node->data.match_expr.right, depth + 1);
            break;
        default:
            printf("Unknown Node\n");
    }
}
