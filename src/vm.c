#include "vm.h"
#include "native.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void init_chunk(Chunk* chunk) {
    chunk->code = NULL;
    chunk->count = 0;
    chunk->capacity = 0;
    chunk->constants = NULL;
    chunk->constant_count = 0;
    chunk->constant_capacity = 0;
}

void write_chunk(Chunk* chunk, Instruction inst) {
    if (chunk->capacity < chunk->count + 1) {
        chunk->capacity = chunk->capacity < 8 ? 8 : chunk->capacity * 2;
        chunk->code = realloc(chunk->code, sizeof(Instruction) * chunk->capacity);
    }
    chunk->code[chunk->count] = inst;
    chunk->count++;
}

int add_constant(Chunk* chunk, Value value) {
    if (chunk->constant_capacity < chunk->constant_count + 1) {
        chunk->constant_capacity = chunk->constant_capacity < 8 ? 8 : chunk->constant_capacity * 2;
        chunk->constants = realloc(chunk->constants, sizeof(Value) * chunk->constant_capacity);
    }
    chunk->constants[chunk->constant_count] = value;
    return chunk->constant_count++;
}

void init_vm(VM* vm) {
    vm->frame_count = 0;
    for (int i = 0; i < REGISTER_COUNT; i++) {
        vm->registers[i] = (Value){VAL_NIL, {.number = 0}};
    }
}

// Helper: get register from current frame's base
#define FRAME_REG(frame, r) (vm->registers[(frame)->base + (r)])

void interpret(VM* vm, ObjFunction* main_fn) {
    if (vm->frame_count >= FRAMES_MAX) {
        printf("VM Panic: CallFrame stack overflow.\n");
        exit(1);
    }

    CallFrame* frame = &vm->frames[vm->frame_count++];
    frame->function = main_fn;
    frame->ip = 0;
    frame->base = 0; // top-level uses registers from 0

    for (;;) {
        if (frame->ip >= (uint32_t)frame->function->chunk.count) {
            printf("VM Panic: IP out of bounds.\n");
            exit(1);
        }

        Instruction inst = frame->function->chunk.code[frame->ip++];

        uint8_t op = DECODE_OP(inst);
        uint8_t rA = DECODE_A(inst);
        uint8_t rB = DECODE_B(inst);
        uint8_t rC = DECODE_C(inst);

        switch (op) {
            case OP_LOAD_CONST:
                FRAME_REG(frame, rA) = frame->function->chunk.constants[rB];
                break;
            case OP_ADD:
                if (FRAME_REG(frame, rB).type == VAL_NUMBER && FRAME_REG(frame, rC).type == VAL_NUMBER) {
                    FRAME_REG(frame, rA) = (Value){VAL_NUMBER, {.number = FRAME_REG(frame, rB).as.number + FRAME_REG(frame, rC).as.number}};
                }
                break;
            case OP_SUB:
                if (FRAME_REG(frame, rB).type == VAL_NUMBER && FRAME_REG(frame, rC).type == VAL_NUMBER) {
                    FRAME_REG(frame, rA) = (Value){VAL_NUMBER, {.number = FRAME_REG(frame, rB).as.number - FRAME_REG(frame, rC).as.number}};
                }
                break;
            case OP_MUL:
                if (FRAME_REG(frame, rB).type == VAL_NUMBER && FRAME_REG(frame, rC).type == VAL_NUMBER) {
                    FRAME_REG(frame, rA) = (Value){VAL_NUMBER, {.number = FRAME_REG(frame, rB).as.number * FRAME_REG(frame, rC).as.number}};
                }
                break;
            case OP_DIV:
                if (FRAME_REG(frame, rB).type == VAL_NUMBER && FRAME_REG(frame, rC).type == VAL_NUMBER) {
                    if (FRAME_REG(frame, rC).as.number == 0) { printf("VM Panic: Division by zero.\n"); exit(1); }
                    FRAME_REG(frame, rA) = (Value){VAL_NUMBER, {.number = FRAME_REG(frame, rB).as.number / FRAME_REG(frame, rC).as.number}};
                }
                break;
            case OP_ADD_WRAP:
                if (FRAME_REG(frame, rB).type == VAL_NUMBER && FRAME_REG(frame, rC).type == VAL_NUMBER) {
                    FRAME_REG(frame, rA) = (Value){VAL_NUMBER, {.number = (double)((int)FRAME_REG(frame, rB).as.number + (int)FRAME_REG(frame, rC).as.number)}};
                }
                break;
            case OP_ADD_SAT:
                if (FRAME_REG(frame, rB).type == VAL_NUMBER && FRAME_REG(frame, rC).type == VAL_NUMBER) {
                    FRAME_REG(frame, rA) = (Value){VAL_NUMBER, {.number = FRAME_REG(frame, rB).as.number + FRAME_REG(frame, rC).as.number}};
                }
                break;
            case OP_EQUAL:
                if (FRAME_REG(frame, rB).type == VAL_NUMBER && FRAME_REG(frame, rC).type == VAL_NUMBER) {
                    FRAME_REG(frame, rA) = (Value){VAL_BOOL, {.boolean = FRAME_REG(frame, rB).as.number == FRAME_REG(frame, rC).as.number}};
                }
                break;
            case OP_LESS:
                if (FRAME_REG(frame, rB).type == VAL_NUMBER && FRAME_REG(frame, rC).type == VAL_NUMBER) {
                    FRAME_REG(frame, rA) = (Value){VAL_BOOL, {.boolean = FRAME_REG(frame, rB).as.number < FRAME_REG(frame, rC).as.number}};
                }
                break;
            case OP_GREATER:
                if (FRAME_REG(frame, rB).type == VAL_NUMBER && FRAME_REG(frame, rC).type == VAL_NUMBER) {
                    FRAME_REG(frame, rA) = (Value){VAL_BOOL, {.boolean = FRAME_REG(frame, rB).as.number > FRAME_REG(frame, rC).as.number}};
                }
                break;
            case OP_NOT:
                FRAME_REG(frame, rA) = (Value){VAL_BOOL, {.boolean = !FRAME_REG(frame, rB).as.boolean}};
                break;
            case OP_PRINT:
                print_value(FRAME_REG(frame, rA));
                printf("\n");
                break;
            case OP_MOVE:
                FRAME_REG(frame, rA) = FRAME_REG(frame, rB);
                break;
            case OP_JUMP_IF_FALSE: {
                bool isFalse = false;
                Value cond = FRAME_REG(frame, rA);
                if (cond.type == VAL_BOOL && !cond.as.boolean) isFalse = true;
                if (cond.type == VAL_NIL) isFalse = true;
                if (cond.type == VAL_NUMBER && cond.as.number == 0) isFalse = true;
                if (isFalse) { uint16_t offset = (rB << 8) | rC; frame->ip += offset; }
                break;
            }
            case OP_JUMP: {
                uint16_t offset = (rB << 8) | rC;
                frame->ip += offset;
                break;
            }
            case OP_LOOP: {
                uint16_t offset = (rB << 8) | rC;
                frame->ip -= offset;
                break;
            }
            case OP_CALL: {
                Value callee = FRAME_REG(frame, rA);
                if (IS_OBJ(callee)) {
                    if (AS_OBJ(callee)->type == OBJ_FUNCTION) {
                        ObjFunction* fn = AS_FUNCTION(callee);
                        if (rB != fn->arity) {
                            printf("Runtime Error: Expected %d args, got %d\n", fn->arity, rB);
                            exit(1);
                        }
                        if (vm->frame_count >= FRAMES_MAX) { printf("Stack Overflow\n"); exit(1); }
                        
                        // New frame setup
                        int next_base = frame->base + rA;
                        int next_return = frame->base + rC;
                        
                        CallFrame* next = &vm->frames[vm->frame_count++];
                        next->function = fn;
                        next->ip = 0;
                        next->base = next_base;
                        next->return_dest = next_return;
                        
                        // Note: Arguments are already in the right place relative to next->base
                        // because they were placed in rA+1, rA+2... by the caller.
                        // So we don't need a copy loop here if we design correctly.
                        
                        frame = next;
                    } else if (AS_OBJ(callee)->type == OBJ_STRUCT) {
                        ObjStruct* st = (ObjStruct*)AS_OBJ(callee);
                        ObjInstance* instance = new_instance(st);
                        FRAME_REG(frame, rC) = (Value){VAL_OBJ, {.obj = (Obj*)instance}};
                    } else {
                        printf("Runtime Error: Object not callable\n");
                        exit(1);
                    }
                } else {
                    printf("Runtime Error: Value not callable\n");
                    exit(1);
                }
                break;
            }
            case OP_STRUCT: {
                FRAME_REG(frame, rA) = frame->function->chunk.constants[rB];
                break;
            }
            case OP_GET_FIELD: {
                Value valObj = FRAME_REG(frame, rB);
                if (!IS_OBJ(valObj) || AS_OBJ(valObj)->type != OBJ_INSTANCE) {
                    printf("Runtime Error: Only instances have fields.\n");
                    exit(1);
                }
                ObjInstance* instance = (ObjInstance*)AS_OBJ(valObj);
                ObjString* fieldName = AS_STRING(frame->function->chunk.constants[rC]);
                int idx = -1;
                for (int i = 0; i < instance->klass->field_count; i++) {
                    if (instance->klass->field_name_lens[i] == fieldName->length &&
                        memcmp(instance->klass->field_names[i], fieldName->chars, fieldName->length) == 0) {
                        idx = i;
                        break;
                    }
                }
                if (idx == -1) {
                    printf("Runtime Error: Undefined field '%.*s'\n", fieldName->length, fieldName->chars);
                    exit(1);
                }
                FRAME_REG(frame, rA) = instance->fields[idx];
                break;
            }
            case OP_SET_FIELD: {
                Value valObj = FRAME_REG(frame, rA);
                if (!IS_OBJ(valObj) || AS_OBJ(valObj)->type != OBJ_INSTANCE) {
                    printf("Runtime Error: Only instances have fields.\n");
                    exit(1);
                }
                ObjInstance* instance = (ObjInstance*)AS_OBJ(valObj);
                ObjString* fieldName = AS_STRING(frame->function->chunk.constants[rB]);
                int idx = -1;
                for (int i = 0; i < instance->klass->field_count; i++) {
                    if (instance->klass->field_name_lens[i] == fieldName->length &&
                        memcmp(instance->klass->field_names[i], fieldName->chars, fieldName->length) == 0) {
                        idx = i;
                        break;
                    }
                }
                if (idx == -1) {
                    printf("Runtime Error: Undefined field '%.*s'\n", fieldName->length, fieldName->chars);
                    exit(1);
                }
                instance->fields[idx] = FRAME_REG(frame, rC);
                break;
            }
            case OP_RETURN: {
                Value retVal = FRAME_REG(frame, rA);
                int dest     = frame->return_dest;

                vm->frame_count--;
                if (vm->frame_count == 0) return; // main returned

                frame = &vm->frames[vm->frame_count - 1];
                vm->registers[dest] = retVal;
                break;
            }
            case OP_HALT:
                return;
            default:
                printf("VM Error: Unknown opcode %d.\n", op);
                exit(1);
        }
    }
}
