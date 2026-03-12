#ifndef VIPER_IR_H
#define VIPER_IR_H

#include "vm.h"
#include "native.h"

// IR Instruction types
typedef enum {
    IR_NOP,
    IR_LOAD_CONST,
    IR_MOVE,
    IR_ADD,
    IR_SUB,
    IR_MUL,
    IR_DIV,
    IR_LABEL,
    IR_JUMP,
    IR_JUMP_IF_FALSE,
    IR_CALL,
    IR_RETURN
} IrOp;

typedef struct {
    IrOp op;
    int a, b, c;
    void* extra;
} IrInst;

typedef struct {
    IrInst* insts;
    int count;
    int capacity;
} IrBlock;

// Generate IR from Bytecode Chunk
IrBlock* ir_from_chunk(Chunk* chunk);
void free_ir_block(IrBlock* block);

// Transpile IR to C source string
char* ir_to_c_source(IrBlock* block, ObjFunction* fn);

#endif // VIPER_IR_H
