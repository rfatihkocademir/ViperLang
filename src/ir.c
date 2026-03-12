#include "ir.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

IrBlock* ir_from_chunk(Chunk* chunk) {
    IrBlock* block = malloc(sizeof(IrBlock));
    block->count = 0;
    block->capacity = chunk->count;
    block->insts = malloc(sizeof(IrInst) * block->capacity);

    for (int i = 0; i < chunk->count; i++) {
        Instruction inst = chunk->code[i];
        OpCode op = DECODE_OP(inst);
        int rA = DECODE_A(inst);
        int rB = DECODE_B(inst);
        int rC = DECODE_C(inst);

        IrInst ir;
        ir.a = rA; ir.b = rB; ir.c = rC; ir.extra = NULL;
        
        switch (op) {
            case OP_LOAD_CONST: ir.op = IR_LOAD_CONST; break;
            case OP_MOVE:       ir.op = IR_MOVE; break;
            case OP_ADD:        ir.op = IR_ADD; break;
            case OP_SUB:        ir.op = IR_SUB; break;
            case OP_MUL:        ir.op = IR_MUL; break;
            case OP_DIV:        ir.op = IR_DIV; break;
            case OP_RETURN:     ir.op = IR_RETURN; break;
            default:            ir.op = IR_NOP; break;
        }
        block->insts[block->count++] = ir;
    }
    return block;
}

void free_ir_block(IrBlock* block) {
    if (!block) return;
    free(block->insts);
    free(block);
}

char* ir_to_c_source(IrBlock* block, ObjFunction* fn) {
    // Generate a C function that maps ViperLang state
    // For simplicity, we assume fixed register names R0, R1, ...
    char* buf = malloc(16384);
    int pos = snprintf(buf, 16384, 
        "#include \"vm.h\"\n"
        "#include \"native.h\"\n\n"
        "Value %.*s_jit(int argCount, Value* args, Value* closure) {\n"
        "  Value R[256];\n"
        "  for(int i=0; i<argCount && i<256; i++) R[i] = args[i];\n",
        fn->name_len, fn->name);

    for (int i = 0; i < block->count; i++) {
        IrInst ir = block->insts[i];
        switch (ir.op) {
            case IR_LOAD_CONST:
                pos += snprintf(buf + pos, 16384 - pos, "  R[%d] = (Value){0}; // Load const placeholder\n", ir.a);
                break;
            case IR_MOVE:
                pos += snprintf(buf + pos, 16384 - pos, "  R[%d] = R[%d];\n", ir.a, ir.b);
                break;
            case IR_ADD:
                pos += snprintf(buf + pos, 16384 - pos, "  R[%d].as.number = R[%d].as.number + R[%d].as.number; R[%d].type = VAL_NUMBER;\n", ir.a, ir.b, ir.c, ir.a);
                break;
            case IR_RETURN:
                pos += snprintf(buf + pos, 16384 - pos, "  return R[%d];\n", ir.a);
                break;
            default: break;
        }
    }
    
    pos += snprintf(buf + pos, 16384 - pos, "  return (Value){VAL_NIL, {.number = 0}};\n}\n");
    return buf;
}
