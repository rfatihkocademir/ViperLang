#ifndef VIPER_VM_H
#define VIPER_VM_H

#include <stdint.h>
#include <stdbool.h>

// Early declarations to avoid circular dependencies
typedef enum {
    VAL_BOOL,
    VAL_NIL,
    VAL_NUMBER,
    VAL_OBJ
} ValueType;

typedef struct sObj Obj;

typedef struct {
    ValueType type;
    union {
        bool boolean;
        double number;
        Obj* obj;
    } as;
} Value;

#include <stdint.h>

// Register-based Opcodes
typedef enum {
    OP_LOAD_CONST,  // LOAD_CONST R_DEST, CONST_INDEX
    OP_ADD,         // ADD R_DEST, R_A, R_B
    OP_SUB,         // SUB R_DEST, R_A, R_B
    OP_MUL,         // MUL R_DEST, R_A, R_B
    OP_DIV,         // DIV R_DEST, R_A, R_B
    
    // Strict v3 operators
    OP_ADD_WRAP,    // ADD_WRAP (+~) R_DEST, R_A, R_B
    OP_ADD_SAT,     // ADD_SAT (^+) R_DEST, R_A, R_B
    
    // Boolean logic
    OP_EQUAL,
    OP_LESS,
    OP_GREATER,
    OP_NOT,
    
    // Control Flow
    OP_MOVE,            // MOVE R_DEST, R_SRC
    OP_JUMP_IF_FALSE,   // JUMP_IF_FALSE R_COND, OFFSET_HIGH, OFFSET_LOW
    OP_JUMP,            // JUMP 0, OFFSET_HIGH, OFFSET_LOW
    OP_LOOP,            // LOOP 0, OFFSET_HIGH, OFFSET_LOW (backward jump)
    
    OP_CALL,            // CALL R_FN, ARG_COUNT, R_DEST
    OP_RETURN,          // RETURN R_VAL
    
    OP_STRUCT,          // STRUCT R_DEST, CONST_INDEX (defines struct)
    OP_GET_FIELD,       // GET_FIELD R_DEST, R_OBJ, CONST_INDEX_NAME
    OP_SET_FIELD,       // SET_FIELD R_OBJ, CONST_INDEX_NAME, R_VAL
    
    OP_PRINT,       // PRINT R_A
    OP_HALT         // End of execution
} OpCode;

// Instruction is 32-bit: 8-bit Opcode, 8-bit Dest Register, 8-bit Reg A, 8-bit Reg B
typedef uint32_t Instruction;

#define ENCODE_INST(op, rA, rB, rC) \
    (((op) << 24) | ((rA) << 16) | ((rB) << 8) | (rC))
    
#define DECODE_OP(inst) ((inst) >> 24)
#define DECODE_A(inst)  (((inst) >> 16) & 0xFF)
#define DECODE_B(inst)  (((inst) >> 8) & 0xFF)
#define DECODE_C(inst)  ((inst) & 0xFF)

typedef struct {
    Instruction* code;
    int count;
    int capacity;
    
    Value* constants;
    int constant_count;
    int constant_capacity;
} Chunk;

void init_chunk(Chunk* chunk);
void write_chunk(Chunk* chunk, Instruction inst);
int add_constant(Chunk* chunk, Value value);

// ViperVM Runtime
#define REGISTER_COUNT 256
#define FRAMES_MAX     64

typedef struct {
    struct sObjFunction* function;
    uint32_t ip;
    int base;        // Base register offset for this frame in the shared register array
    int return_dest; // Absolute register index in CALLER where return value should go
} CallFrame;

typedef struct {
    CallFrame frames[FRAMES_MAX];
    int frame_count;
    Value registers[REGISTER_COUNT]; // Shared flat register space
} VM;

void init_vm(VM* vm);
void interpret(VM* vm, struct sObjFunction* main_fn);

#endif // VIPER_VM_H
