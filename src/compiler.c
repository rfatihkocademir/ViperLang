#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parser.h"
#include "compiler.h"
#include "vm.h"
#include "native.h"

// ---- Local Variable Table -----------------------------------------------

typedef struct {
    const char* name;
    int length;
    int reg;
    int depth;
} Local;

#define MAX_LOCALS 256
#define MAX_FUNCTIONS 64

// Global registries
static ObjFunction* fn_registry[MAX_FUNCTIONS];
static int fn_count = 0;

static ObjStruct* st_registry[MAX_FUNCTIONS];
static int st_count = 0;

typedef struct {
    const char* name;
    int length;
} GlobalVar;

static GlobalVar global_vars[256];
static int global_var_count = 0;

typedef struct {
    Local locals[MAX_LOCALS];
    int local_count;
    int next_reg;
    int scope_depth;
    ObjFunction* current_fn; // The function we are currently compiling into
} CompilerState;

static CompilerState cst;

static void init_compiler(ObjFunction* fn) {
    cst.local_count = 0;
    cst.next_reg = 1;       // R0 is reserved for call bookkeeping
    cst.scope_depth = 0;
    cst.current_fn = fn;
}

static Chunk* current_chunk() {
    return &cst.current_fn->chunk;
}

static int resolve_local(const char* name, int length) {
    for (int i = cst.local_count - 1; i >= 0; i--) {
        if (cst.locals[i].length == length &&
            memcmp(cst.locals[i].name, name, length) == 0) {
            return cst.locals[i].reg;
        }
    }
    return -1;
}

static ObjFunction* resolve_function(const char* name, int length) {
    for (int i = 0; i < fn_count; i++) {
        if (fn_registry[i]->name_len == length &&
            memcmp(fn_registry[i]->name, name, length) == 0) {
            return fn_registry[i];
        }
    }
    return NULL;
}

static ObjStruct* resolve_struct(const char* name, int length) {
    for (int i = 0; i < st_count; i++) {
        if (st_registry[i]->name_len == length &&
            memcmp(st_registry[i]->name, name, length) == 0) {
            return st_registry[i];
        }
    }
    return NULL;
}

// ---- Jump helpers --------------------------------------------------------

static int emit_jump(uint8_t jmpType, int regCond) {
    write_chunk(current_chunk(), ENCODE_INST(jmpType, regCond, 0, 0));
    return current_chunk()->count - 1;
}

static void patch_jump(int offset) {
    int jump = current_chunk()->count - offset - 1;
    uint32_t inst  = current_chunk()->code[offset];
    uint8_t  op    = DECODE_OP(inst);
    uint8_t  rA    = DECODE_A(inst);
    current_chunk()->code[offset] = ENCODE_INST(op, rA, (jump >> 8) & 0xFF, jump & 0xFF);
}

static void begin_scope() { cst.scope_depth++; }
static void end_scope() {
    cst.scope_depth--;
    while (cst.local_count > 0 &&
           cst.locals[cst.local_count - 1].depth > cst.scope_depth) {
        cst.local_count--;
    }
}

// ---- Expression Compiler -------------------------------------------------

static int compile_expr(AstNode* expr);
static void compile_stmt(AstNode* stmt);

static int compile_expr(AstNode* expr) {
    if (!expr) return -1;

    if (expr->type == AST_NUMBER) {
        int reg = cst.next_reg++;
        Value val = {VAL_NUMBER, {.number = expr->data.number_val}};
        int ci = add_constant(current_chunk(), val);
        write_chunk(current_chunk(), ENCODE_INST(OP_LOAD_CONST, reg, ci, 0));
        return reg;
    }

    if (expr->type == AST_STRING) {
        int reg = cst.next_reg++;
        ObjString* s = copy_string(expr->data.str_val.name, expr->data.str_val.length);
        Value val = {VAL_OBJ, {.obj = (Obj*)s}};
        int ci = add_constant(current_chunk(), val);
        write_chunk(current_chunk(), ENCODE_INST(OP_LOAD_CONST, reg, ci, 0));
        return reg;
    }

    if (expr->type == AST_IDENTIFIER) {
        int reg = resolve_local(expr->data.identifier.name.start, expr->data.identifier.name.length);
        if (reg == -1) {
            printf("Compiler Error: Undefined variable '%.*s'\n",
                expr->data.identifier.name.length, expr->data.identifier.name.start);
            exit(1);
        }
        return reg;
    }

    if (expr->type == AST_ASSIGN_EXPR) {
        int src = compile_expr(expr->data.assign.value);
        int dst = resolve_local(expr->data.assign.name.start, expr->data.assign.name.length);
        if (dst == -1) { printf("Compiler Error: Undefined variable for assignment\n"); exit(1); }
        write_chunk(current_chunk(), ENCODE_INST(OP_MOVE, dst, src, 0));
        return dst;
    }

    if (expr->type == AST_BINARY_EXPR) {
        int rL = compile_expr(expr->data.binary.left);
        int rR = compile_expr(expr->data.binary.right);
        int rD = cst.next_reg++;
        uint8_t op = OP_ADD;
        switch (expr->data.binary.op.type) {
            case TOKEN_PLUS:        op = OP_ADD;     break;
            case TOKEN_MINUS:       op = OP_SUB;     break;
            case TOKEN_STAR:        op = OP_MUL;     break;
            case TOKEN_SLASH:       op = OP_DIV;     break;
            case TOKEN_LESS:        op = OP_LESS;    break;
            case TOKEN_GREATER:     op = OP_GREATER; break;
            case TOKEN_EQUAL_EQUAL: op = OP_EQUAL;   break;
            case TOKEN_PLUS_TILDE:  op = OP_ADD_WRAP;break;
            case TOKEN_CARET_PLUS:  op = OP_ADD_SAT; break;
            default: break;
        }
        write_chunk(current_chunk(), ENCODE_INST(op, rD, rL, rR));
        return rD;
    }

    if (expr->type == AST_CALL_EXPR) {
        const char* name = expr->data.call_expr.name.start;
        int namelen      = expr->data.call_expr.name.length;
        int arg_count    = expr->data.call_expr.arg_count;

        // Built-in: pr(...)
        if (namelen == 2 && memcmp(name, "pr", 2) == 0) {
            for (int i = 0; i < arg_count; i++) {
                int reg = compile_expr(expr->data.call_expr.args[i]);
                write_chunk(current_chunk(), ENCODE_INST(OP_PRINT, reg, 0, 0));
            }
            return -1;
        }

        // User-defined function or Struct (for instantiation)
        // printf("Resolving callable: '%.*s' (len=%d)\n", namelen, name, namelen);
        ObjFunction* callee_fn = resolve_function(name, namelen);
        ObjStruct*   callee_st = callee_fn ? NULL : resolve_struct(name, namelen);

        if (!callee_fn && !callee_st) {
            printf("Compiler Error: Unknown callable '%.*s' (len=%d)\n", namelen, name, namelen);
            exit(1);
        }

        int fnReg = cst.next_reg++;
        Value val = {VAL_OBJ, {.obj = callee_fn ? (Obj*)callee_fn : (Obj*)callee_st}};
        int ci = add_constant(current_chunk(), val);
        write_chunk(current_chunk(), ENCODE_INST(OP_LOAD_CONST, fnReg, ci, 0));

        for (int i = 0; i < arg_count; i++) {
            int argReg = cst.next_reg++;
            int srcReg = compile_expr(expr->data.call_expr.args[i]);
            write_chunk(current_chunk(), ENCODE_INST(OP_MOVE, argReg, srcReg, 0));
        }

        int destReg = cst.next_reg++;
        write_chunk(current_chunk(), ENCODE_INST(OP_CALL, fnReg, (uint8_t)arg_count, (uint8_t)destReg));
        return destReg;
    }

    if (expr->type == AST_GET_EXPR) {
        int rObj = compile_expr(expr->data.get_expr.obj);
        int rDest = cst.next_reg++;
        // Add field name as a string constant
        ObjString* fieldName = copy_string(expr->data.get_expr.name.start, expr->data.get_expr.name.length);
        int ci = add_constant(current_chunk(), (Value){VAL_OBJ, {.obj = (Obj*)fieldName}});
        write_chunk(current_chunk(), ENCODE_INST(OP_GET_FIELD, rDest, rObj, ci));
        return rDest;
    }

    if (expr->type == AST_SET_EXPR) {
        int rObj = compile_expr(expr->data.set_expr.obj);
        int rVal = compile_expr(expr->data.set_expr.value);
        ObjString* fieldName = copy_string(expr->data.set_expr.name.start, expr->data.set_expr.name.length);
        int ci = add_constant(current_chunk(), (Value){VAL_OBJ, {.obj = (Obj*)fieldName}});
        write_chunk(current_chunk(), ENCODE_INST(OP_SET_FIELD, rObj, ci, rVal));
        return rVal;
    }

    return -1;
}

// ---- Statement Compiler --------------------------------------------------

static void compile_stmt(AstNode* stmt) {
    if (!stmt) return;

    if (stmt->type == AST_VAR_DECL) {
        int reg = compile_expr(stmt->data.var_decl.initializer);
        Local* local = &cst.locals[cst.local_count++];
        local->name  = stmt->data.var_decl.name.start;
        local->length= stmt->data.var_decl.name.length;
        local->reg   = reg;
        local->depth = cst.scope_depth;

        if (cst.scope_depth == 0) {
            global_vars[global_var_count].name = local->name;
            global_vars[global_var_count].length = local->length;
            global_var_count++;
        }
    }
    else if (stmt->type == AST_BLOCK_STMT) {
        begin_scope();
        for (int i = 0; i < stmt->data.block.count; i++) compile_stmt(stmt->data.block.statements[i]);
        end_scope();
    }
    else if (stmt->type == AST_IF_STMT) {
        int rCond      = compile_expr(stmt->data.if_stmt.condition);
        int jiFalse    = emit_jump(OP_JUMP_IF_FALSE, rCond);
        compile_stmt(stmt->data.if_stmt.then_branch);
        int jEnd       = emit_jump(OP_JUMP, 0);
        patch_jump(jiFalse);
        if (stmt->data.if_stmt.else_branch) compile_stmt(stmt->data.if_stmt.else_branch);
        patch_jump(jEnd);
    }
    else if (stmt->type == AST_WHILE_STMT) {
        int loopStart = current_chunk()->count;
        int rCond     = compile_expr(stmt->data.while_stmt.condition);
        int exitJump  = emit_jump(OP_JUMP_IF_FALSE, rCond);
        compile_stmt(stmt->data.while_stmt.body);
        int offset    = current_chunk()->count - loopStart + 1;
        write_chunk(current_chunk(), ENCODE_INST(OP_LOOP, 0, (offset >> 8) & 0xFF, offset & 0xFF));
        patch_jump(exitJump);
    }
    else if (stmt->type == AST_RETURN_STMT) {
        if (stmt->data.return_stmt.value) {
            int reg = compile_expr(stmt->data.return_stmt.value);
            write_chunk(current_chunk(), ENCODE_INST(OP_RETURN, reg, 0, 0));
        } else {
            write_chunk(current_chunk(), ENCODE_INST(OP_RETURN, 0, 0, 0));
        }
    }
    else if (stmt->type == AST_FUNC_DECL) {
        // Create a new ObjFunction and compile the body into it
        ObjFunction* fn = new_function(
            stmt->data.func_decl.name.start,
            stmt->data.func_decl.name.length,
            stmt->data.func_decl.param_count
        );

        // Register globally so call expressions can find it
        if (fn_count < MAX_FUNCTIONS) fn_registry[fn_count++] = fn;

        // Save outer compiler state and start compiling the function body
        CompilerState outer = cst;
        init_compiler(fn);

        // Bind parameters as locals at register 1..N (R0 is bookkeeping)
        for (int i = 0; i < stmt->data.func_decl.param_count; i++) {
            Local* p = &cst.locals[cst.local_count++];
            p->name  = stmt->data.func_decl.params[i].start;
            p->length= stmt->data.func_decl.params[i].length;
            p->reg   = cst.next_reg++;
            p->depth = 0;
        }

        // Compile function body
        compile_stmt(stmt->data.func_decl.body);

        // Implicit return nil if no explicit ret
        write_chunk(current_chunk(), ENCODE_INST(OP_RETURN, 0, 0, 0));

        // Restore outer state
        cst = outer;
    }
    else if (stmt->type == AST_STRUCT_DECL) {
        // Prepare arrays of field names and lengths
        const char** field_names = malloc(sizeof(char*) * stmt->data.struct_decl.field_count);
        int* field_lens = malloc(sizeof(int) * stmt->data.struct_decl.field_count);
        for (int i = 0; i < stmt->data.struct_decl.field_count; i++) {
            field_names[i] = stmt->data.struct_decl.fields[i].start;
            field_lens[i] = stmt->data.struct_decl.fields[i].length;
        }

        ObjStruct* st = new_struct(
            stmt->data.struct_decl.name.start,
            stmt->data.struct_decl.name.length,
            stmt->data.struct_decl.field_count,
            field_names,
            field_lens
        );

        if (st_count < MAX_FUNCTIONS) st_registry[st_count++] = st;

        int reg = cst.next_reg++;
        int ci = add_constant(current_chunk(), (Value){VAL_OBJ, {.obj = (Obj*)st}});
        write_chunk(current_chunk(), ENCODE_INST(OP_STRUCT, reg, ci, 0));
        
        // Register in local table if at top level so it acts like a global type name
        if (cst.scope_depth == 0) {
            Local* local = &cst.locals[cst.local_count++];
            local->name = stmt->data.struct_decl.name.start;
            local->length = stmt->data.struct_decl.name.length;
            local->reg = reg;
            local->depth = 0;
        }
    }
    else if (stmt->type == AST_USE_STMT) {
        // Module system: Read and compile the file
        char path[256];
        snprintf(path, sizeof(path), "%.*s", stmt->data.use_stmt.path.length-2, stmt->data.use_stmt.path.start+1);
        
        FILE* file = fopen(path, "r");
        if (!file) { printf("Compiler Error: Could not open module '%s'\n", path); exit(1); }
        fseek(file, 0, SEEK_END);
        long size = ftell(file);
        rewind(file);
        char* source = malloc(size + 1);
        fread(source, 1, size, file);
        source[size] = '\0';
        fclose(file);

        AstNode* mod_ast = parse(source);
        if (mod_ast->type == AST_PROGRAM) {
            for (int i = 0; i < mod_ast->data.block.count; i++) {
                compile_stmt(mod_ast->data.block.statements[i]);
            }
        }
        // Note: We do NOT free(source) here because names of functions/structs 
        // compiled from this module point directly into this source string.
    }
    else if (stmt->type == AST_EXPR_STMT) {
        AstNode* node = stmt->data.expr_stmt.expr;
        int reg       = compile_expr(node);
        bool isAssign = node && node->type == AST_ASSIGN_EXPR;
        bool isCall   = node && node->type == AST_CALL_EXPR;
        if (reg >= 0 && !isAssign && !isCall) {
            write_chunk(current_chunk(), ENCODE_INST(OP_PRINT, reg, 0, 0));
        }
    }
}

// ---- Top-Level Entry Point -----------------------------------------------

void generate_contract() {
    printf("\n--- @contract ---\n");
    printf("// @contract\n");
    
    if (global_var_count > 0) {
        printf("// export_v: ");
        for (int i = 0; i < global_var_count; i++) {
            printf("%.*s%s", global_vars[i].length, global_vars[i].name, (i == global_var_count - 1) ? "" : ", ");
        }
        printf("\n");
    }

    if (fn_count > 0) {
        printf("// export_fn: ");
        for (int i = 0; i < fn_count; i++) {
            printf("%.*s(%d)%s", fn_registry[i]->name_len, fn_registry[i]->name, fn_registry[i]->arity, (i == fn_count - 1) ? "" : ", ");
        }
        printf("\n");
    }
    printf("-----------------\n\n");
}

ObjFunction* compile(AstNode* ast) {
    fn_count = 0;
    st_count = 0;
    global_var_count = 0;

    // Wrap all top-level code in an implicit "main" function
    ObjFunction* main_fn = new_function("__main__", 8, 0);
    init_compiler(main_fn);

    if (ast->type == AST_PROGRAM) {
        for (int i = 0; i < ast->data.block.count; i++) {
            compile_stmt(ast->data.block.statements[i]);
        }
    } else {
        compile_stmt(ast);
    }

    write_chunk(current_chunk(), ENCODE_INST(OP_HALT, 0, 0, 0));
    
    generate_contract();
    
    return main_fn;
}
