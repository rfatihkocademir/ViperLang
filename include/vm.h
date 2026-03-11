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
typedef struct sObjString ObjString;

typedef struct {
    ValueType type;
    union {
        bool boolean;
        double number;
        Obj* obj;
    } as;
} Value;

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

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
    OP_JUMP_IF_NIL,     // JUMP_IF_NIL R_VAL, OFFSET_HIGH, OFFSET_LOW
    OP_JUMP_IF_NOT_NIL, // JUMP_IF_NOT_NIL R_VAL, OFFSET_HIGH, OFFSET_LOW
    OP_JUMP,            // JUMP 0, OFFSET_HIGH, OFFSET_LOW
    OP_LOOP,            // LOOP 0, OFFSET_HIGH, OFFSET_LOW (backward jump)
    
    OP_CALL,            // CALL R_FN, ARG_COUNT, R_DEST
    OP_CALL_NATIVE,     // CALL_NATIVE R_NATIVE_INDEX, ARG_COUNT, R_DEST
    OP_RETURN,          // RETURN R_VAL
    
    OP_STRUCT,          // STRUCT R_DEST, CONST_INDEX (defines struct)
    OP_GET_FIELD,       // GET_FIELD R_DEST, R_OBJ, CONST_INDEX_NAME
    OP_SET_FIELD,       // SET_FIELD R_OBJ, CONST_INDEX_NAME, R_VAL
    
    OP_PRINT,       // PRINT R_A
    OP_HALT,        // End of execution
    OP_GET_GLOBAL,  // GET_GLOBAL R_DEST, CONST_INDEX_NAME
    OP_SET_GLOBAL,      // SET_GLOBAL 0, CONST_INDEX_NAME, R_VAL
    
    // Core Async
    OP_SPAWN,           // SPAWN R_DEST, R_FN, ARG_COUNT
    OP_AWAIT,           // AWAIT R_DEST, R_HANDLE, 0
    
    // Core Types & Memory
    OP_TYPEOF,          // TYPEOF R_DEST, R_VAL, 0
    OP_CLONE,           // CLONE R_DEST, R_VAL, 0
    OP_ARRAY,           // ARRAY R_DEST, ELEM_COUNT, R_START
    
    // Core Safety
    OP_SETUP_TRY,       // SETUP_TRY R_DEST, OFFSET_HIGH, OFFSET_LOW
    OP_TEARDOWN_TRY,    // TEARDOWN_TRY
    
    // Core Dynamic
    OP_EVAL,            // EVAL R_DEST, R_CODE_STR
    OP_KEYS,            // KEYS R_DEST, R_OBJ
    OP_HAS,             // HAS R_DEST, R_OBJ, R_PROP
    OP_GET_INDEX,       // GET_INDEX R_DEST, R_OBJ, R_INDEX
    OP_SET_INDEX,       // SET_INDEX R_OBJ, R_INDEX, R_VAL
    OP_MATCH,           // MATCH R_DEST, R_STR, R_REGEX
    OP_SYNC_START,      // SYNC_START
    OP_SYNC_END,        // SYNC_END
    
    // Gradual Typing
    OP_ASSERT_TYPE      // ASSERT_TYPE R_VAL, CONST_INDEX_TYPE_STR, 0
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
#define REGISTER_WINDOW 256

typedef struct {
    struct sObjFunction* function;
    uint32_t ip;
    int base;        // Base register offset for this frame in the shared register array
    int return_dest; // Absolute register index in CALLER where return value should go
} CallFrame;

typedef struct {
    uint32_t catch_ip; // Instruction pointer to jump to upon panic
    int target_vp;     // The destination register for the try block evaluation
    int frame_count;   // Target frame depth to unwind to securely
} CatchHandler;

#define MAX_CATCH_HANDLERS 64

typedef struct {
    CallFrame* frames;
    int frame_count;
    int frame_capacity;
    Value* registers; // Shared flat register space, dynamically grown
    int register_capacity;

    struct sObjThread* thread_obj; // The Thread Object representing this Fiber (if spawned)
    
    CatchHandler catch_stack[MAX_CATCH_HANDLERS];
    int catch_count;

    // Simple Global Store
    ObjString** global_names;
    Value* global_values;
    int global_count;
    int global_capacity;

    pthread_mutex_t* global_mutex;
} VM;

typedef enum {
    INTERPRET_OK,
    INTERPRET_YIELD,
    INTERPRET_ERROR
} InterpretResult;

void init_vm(VM* vm);
InterpretResult interpret(VM* vm, struct sObjFunction* main_fn, int max_steps);
Value call_viper_function(struct sObjFunction* fn, int argCount, Value* args);

#endif // VIPER_VM_H
