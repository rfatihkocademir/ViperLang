#include "vm.h"
#include "native.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ffi.h>

static void vm_fatal_oom(void) {
    printf("VM Panic: Out of memory.\n");
    exit(1);
}

static bool ensure_frame_capacity(VM* vm, int needed) {
    if (vm->frame_capacity >= needed) return true;
    int next = (vm->frame_capacity == 0) ? 64 : vm->frame_capacity;
    while (next < needed) next *= 2;
    CallFrame* grown = (CallFrame*)realloc(vm->frames, sizeof(CallFrame) * (size_t)next);
    if (!grown) return false;
    vm->frames = grown;
    vm->frame_capacity = next;
    return true;
}

static bool ensure_register_capacity(VM* vm, int needed) {
    if (vm->register_capacity >= needed) return true;
    int next = (vm->register_capacity == 0) ? REGISTER_WINDOW : vm->register_capacity;
    while (next < needed) next *= 2;
    Value* grown = (Value*)realloc(vm->registers, sizeof(Value) * (size_t)next);
    if (!grown) return false;

    for (int i = vm->register_capacity; i < next; i++) {
        grown[i] = (Value){VAL_NIL, {.number = 0}};
    }

    vm->registers = grown;
    vm->register_capacity = next;
    return true;
}

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
    vm->frames = NULL;
    vm->registers = NULL;
    vm->frame_count = 0;
    vm->frame_capacity = 0;
    vm->register_capacity = 0;

    if (!ensure_frame_capacity(vm, 64) ||
        !ensure_register_capacity(vm, REGISTER_WINDOW)) {
        vm_fatal_oom();
    }
}

// Helper: get register from current frame's base
#define FRAME_REG(frame, r) (vm->registers[(frame)->base + (r)])

static void write_ffi_value(void* dest, ffi_type* f_type, Value v) {
    if (f_type->type == FFI_TYPE_STRUCT) {
        if (!IS_OBJ(v) || AS_OBJ(v)->type != OBJ_INSTANCE) {
            printf("Runtime Error: Expected ObjInstance for FFI struct argument.\n");
            exit(1);
        }
        ObjInstance* inst = (ObjInstance*)AS_OBJ(v);
        size_t offset = 0;
        for (int i = 0; f_type->elements[i] != NULL; i++) {
            ffi_type* etype = f_type->elements[i];
            size_t align = etype->alignment;
            if (offset % align != 0) offset += align - (offset % align);
            
            if (i >= inst->klass->field_count) {
                printf("Runtime Error: FFI Struct has more fields than Viper ObjInstance.\n");
                exit(1);
            }
            write_ffi_value((char*)dest + offset, etype, inst->fields[i]);
            offset += etype->size;
        }
    } else {
        if (f_type == &ffi_type_sint32) *(int32_t*)dest = (int32_t)v.as.number;
        else if (f_type == &ffi_type_sint64) *(int64_t*)dest = (int64_t)v.as.number;
        else if (f_type == &ffi_type_float) *(float*)dest = (float)v.as.number;
        else if (f_type == &ffi_type_double) *(double*)dest = (double)v.as.number;
        else if (f_type == &ffi_type_pointer) {
             if (v.type == VAL_OBJ && AS_OBJ(v)->type == OBJ_STRING) *(void**)dest = AS_STRING(v)->chars;
             else if (v.type == VAL_OBJ && AS_OBJ(v)->type == OBJ_DL_HANDLE) *(void**)dest = ((ObjDlHandle*)AS_OBJ(v))->handle;
             else if (v.type == VAL_OBJ && AS_OBJ(v)->type == OBJ_POINTER) *(void**)dest = ((ObjPointer*)AS_OBJ(v))->ptr;
             else if (v.type == VAL_NIL) *(void**)dest = NULL;
             else { printf("Runtime Error: Unsupported pointer arg conversion.\n"); exit(1); }
        } else { printf("Runtime Error: Unsupported FFI arg type.\n"); exit(1); }
    }
}

static Value read_ffi_value(void* src, ffi_type* f_type, char ret_char) {
    if (f_type->type == FFI_TYPE_STRUCT) {
        int num_elements = 0;
        while (f_type->elements[num_elements] != NULL) num_elements++;
        
        char* st_name = malloc(16);
        snprintf(st_name, 16, "_ffi_struct");
        const char** field_names = malloc(sizeof(char*) * num_elements);
        int* field_lens = malloc(sizeof(int) * num_elements);
        for(int i=0; i<num_elements; i++) {
            char buf[16]; snprintf(buf, 16, "f%d", i);
            field_names[i] = copy_string(buf, strlen(buf))->chars;
            field_lens[i] = (int)strlen(buf);
        }
        ObjStruct* st = new_struct(st_name, strlen(st_name), num_elements, field_names, field_lens);
        ObjInstance* inst = new_instance(st);
        
        size_t offset = 0;
        for (int i = 0; i < num_elements; i++) {
            ffi_type* etype = f_type->elements[i];
            size_t align = etype->alignment;
            if (offset % align != 0) offset += align - (offset % align);
            
            inst->fields[i] = read_ffi_value((char*)src + offset, etype, 'n');
            offset += etype->size;
        }
        return (Value){VAL_OBJ, {.obj = (Obj*)inst}};
    } else {
        if (f_type == &ffi_type_sint32) return (Value){VAL_NUMBER, {.number = (double)(*(int32_t*)src)}};
        else if (f_type == &ffi_type_sint64) return (Value){VAL_NUMBER, {.number = (double)(*(int64_t*)src)}};
        else if (f_type == &ffi_type_float) return (Value){VAL_NUMBER, {.number = (double)(*(float*)src)}};
        else if (f_type == &ffi_type_double) return (Value){VAL_NUMBER, {.number = (double)(*(double*)src)}};
        else if (f_type == &ffi_type_pointer) {
            void* ret_ptr = *(void**)src;
            if (ret_ptr) {
                if (ret_char == 's') return (Value){VAL_OBJ, {.obj = (Obj*)copy_string((const char*)ret_ptr, strlen((const char*)ret_ptr))}};
                else return (Value){VAL_OBJ, {.obj = (Obj*)new_pointer(ret_ptr)}};
            } else return (Value){VAL_NIL, {.number=0}};
        }
        return (Value){VAL_NIL, {.number=0}};
    }
}

void interpret(VM* vm, ObjFunction* main_fn) {
    if (!ensure_frame_capacity(vm, vm->frame_count + 1) ||
        !ensure_register_capacity(vm, REGISTER_WINDOW)) {
        vm_fatal_oom();
    }

    CallFrame* frame = &vm->frames[vm->frame_count++];
    frame->function = main_fn;
    frame->ip = 0;
    frame->base = 0; // top-level uses registers from 0
    frame->return_dest = 0;

    for (;;) {
        if (frame->ip >= (uint32_t)frame->function->chunk.count) {
            printf("VM Panic: IP out of bounds.\n");
            exit(1);
        }
        if (!ensure_register_capacity(vm, frame->base + REGISTER_WINDOW)) {
            vm_fatal_oom();
        }

        Instruction inst = frame->function->chunk.code[frame->ip++];

        uint8_t op = DECODE_OP(inst);
        uint8_t rA = DECODE_A(inst);
        uint8_t rB = DECODE_B(inst);
        uint8_t rC = DECODE_C(inst);

        // DEBUG TRACE
        // printf("TRACE: ip=%d OP=%d rA=%d rB=%d rC=%d\n", frame->ip-1, op, rA, rB, rC);

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
                        
                        // New frame setup
                        int next_base = frame->base + rA;
                        int next_return = frame->base + rC;
                        if (!ensure_frame_capacity(vm, vm->frame_count + 1) ||
                            !ensure_register_capacity(vm, next_base + REGISTER_WINDOW) ||
                            !ensure_register_capacity(vm, next_return + 1)) {
                            vm_fatal_oom();
                        }
                        
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
                        if (rB != st->field_count) {
                            printf("Runtime Error: Expected %d fields for struct %s, got %d.\n", st->field_count, st->name, rB);
                            exit(1);
                        }
                        ObjInstance* instance = new_instance(st);
                        for (int i = 0; i < rB; i++) {
                            instance->fields[i] = vm->registers[frame->base + rA + 1 + i];
                        }
                        FRAME_REG(frame, rC) = (Value){VAL_OBJ, {.obj = (Obj*)instance}};
                    } else if (AS_OBJ(callee)->type == OBJ_DYNAMIC_FUNC) {
                        ObjDynamicFunction* dyn = (ObjDynamicFunction*)AS_OBJ(callee);
                        ffi_cif* cif = (ffi_cif*)dyn->cif;
                        if (rB != cif->nargs) {
                            printf("Runtime Error: Dynamic FFI expected %d args, got %d.\n", cif->nargs, rB);
                            exit(1);
                        }
                        
                        void** arg_values = malloc(sizeof(void*) * (rB > 0 ? rB : 1));
                        
                        // Parse values from registers to pointers for libffi
                        for (int i = 0; i < rB; i++) {
                            Value* v = &vm->registers[frame->base + rA + 1 + i];
                            ffi_type* f_type = dyn->arg_types[i];
                            
                            void* arg_alloc = malloc(f_type->size > 0 ? f_type->size : 64);
                            write_ffi_value(arg_alloc, f_type, *v);
                            arg_values[i] = arg_alloc;
                        }

                        // Return value allocation
                        void* rvalue = malloc(((ffi_type*)dyn->return_type)->size > 0 ? ((ffi_type*)dyn->return_type)->size : 8);
                        
                        // Call the C function!
                        ffi_call(cif, FFI_FN(dyn->fn_ptr), rvalue, arg_values);

                        // Back to Viper value
                        char ret_char = dyn->signature[dyn->sig_len - 1];
                        Value result = read_ffi_value(rvalue, (ffi_type*)dyn->return_type, ret_char);

                        // Free ffi buffers
                        for(int i=0; i<rB; i++) free(arg_values[i]);
                        free(arg_values);
                        free(rvalue);

                        FRAME_REG(frame, rC) = result;
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
            case OP_CALL_NATIVE: {
                // A = native_index, B = arg_count, C = dest_reg
                NativeFn native = get_native_by_index(rA);
                if (native == NULL) {
                    printf("Runtime Error: Unknown native function index %d.\n", rA);
                    exit(1);
                }
                // Arguments are packed right before destination register.
                Value* args = &vm->registers[frame->base + rC - rB];
                Value result = native(rB, args);
                FRAME_REG(frame, rC) = result;
                break;
            }
            case OP_RETURN: {
                Value retVal = FRAME_REG(frame, rA);
                int dest     = frame->return_dest;

                vm->frame_count--;
                if (vm->frame_count == 0) return; // main returned

                frame = &vm->frames[vm->frame_count - 1];
                if (!ensure_register_capacity(vm, dest + 1)) {
                    vm_fatal_oom();
                }
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
