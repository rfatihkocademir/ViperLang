#include "vm.h"
#include "native.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ffi.h"
#include <pthread.h>
#include "compiler.h"
#include "parser.h"
#include "capabilities.h"
#include <regex.h>
#include <time.h>
#include <unistd.h>
#include "scheduler.h"
#include "jit.h"
#include "profiler.h"

VM* current_vm = NULL;

static void runtime_error(VM* vm, const char* format, ...);
static bool ensure_frame_capacity(VM* vm, int needed);
static bool ensure_register_capacity(VM* vm, int needed);
static bool ensure_register_capacity(VM* vm, int needed);
static void vm_fatal_oom(void);
static Value deep_clone_value(Value value);


Value call_viper_function(ObjFunction* fn, int argCount, Value* args) {
    if (!current_vm) return (Value){VAL_NIL, {.number = 0}};
    
    // Push new frame
    if (!ensure_frame_capacity(current_vm, current_vm->frame_count + 1)) {
         vm_fatal_oom();
    }
    
    int base = 0;
    if (current_vm->frame_count > 0) {
        CallFrame* last_frame = &current_vm->frames[current_vm->frame_count - 1];
        base = last_frame->base + REGISTER_WINDOW;
    }

    if (!ensure_register_capacity(current_vm, base + REGISTER_WINDOW)) {
        vm_fatal_oom();
    }
    
    // Result dest
    int dest = base + 255; 

    // Put fn itself in base
    current_vm->registers[base] = (Value){VAL_OBJ, {.obj = (Obj*)fn}};
    // Put args in base + 1...
    for (int i=0; i<argCount; i++) {
        current_vm->registers[base + 1 + i] = args[i];
    }
    
    CallFrame* frame = &current_vm->frames[current_vm->frame_count++];
    frame->function = fn;
    frame->ip = 0;
    frame->base = base;
    frame->return_dest = dest;

    // Run VM until this frame is popped (no yielding for C-ABI calls for now)
    interpret(current_vm, NULL, -1); // -1 means infinite steps
    
    return current_vm->registers[dest];
}

static void vm_fatal_oom(void) {
    printf("VM Panic: Out of memory.\n");
    exit(1);
}

extern __thread const char* last_panic_msg;

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
    profiler_init();
    vm->frame_capacity = 64;
    vm->frames = malloc(sizeof(CallFrame) * vm->frame_capacity);
    vm->frame_count = 0;
    
    vm->register_capacity = 1024;
    vm->registers = malloc(sizeof(Value) * vm->register_capacity);
    
    vm->global_capacity = 64;
    vm->global_names = malloc(sizeof(ObjString*) * vm->global_capacity);
    vm->global_values = malloc(sizeof(Value) * vm->global_capacity);
    vm->global_count = 0;
    
    vm->catch_count = 0;
    vm->global_mutex = (pthread_mutex_t*)malloc(sizeof(pthread_mutex_t));
    pthread_mutex_init(vm->global_mutex, NULL);
    
    vm->thread_obj = NULL;
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

static Value deep_clone_value(Value value) {
    if (!IS_OBJ(value)) return value;

    Obj* obj = AS_OBJ(value);
    switch (obj->type) {
        case OBJ_STRING: {
            ObjString* s = (ObjString*)obj;
            ObjString* copy = copy_string(s->chars, s->length);
            return (Value){VAL_OBJ, {.obj = (Obj*)copy}};
        }
        case OBJ_ARRAY: {
            ObjArray* src = (ObjArray*)obj;
            ObjArray* out = new_array();
            for (int i = 0; i < src->count; i++) {
                array_append(out, deep_clone_value(src->elements[i]));
            }
            return (Value){VAL_OBJ, {.obj = (Obj*)out}};
        }
        case OBJ_INSTANCE: {
            ObjInstance* src = (ObjInstance*)obj;
            ObjInstance* out = new_instance(src->klass);
            for (int i = 0; i < src->klass->field_count; i++) {
                out->fields[i] = deep_clone_value(src->fields[i]);
            }
            return (Value){VAL_OBJ, {.obj = (Obj*)out}};
        }
        default:
            // Non-data objects are shared by reference.
            return value;
    }
}

InterpretResult interpret(VM* vm, struct sObjFunction* main_fn, int max_steps) {
    current_vm = vm;
    int stop_count = vm->frame_count > 0 ? vm->frame_count - 1 : 0;

    if (main_fn) {
        vm->frame_count = 0;
        stop_count = 0;
        if (!ensure_frame_capacity(vm, 1) || !ensure_register_capacity(vm, REGISTER_WINDOW)) {
             vm_fatal_oom();
        }
        CallFrame* frame = &vm->frames[vm->frame_count++];
        frame->function = main_fn;
        frame->ip = 0;
        frame->base = 0;
        frame->return_dest = 0;
        
        // If max_steps is strictly 0, we just wanted to set up the frame
        if (max_steps == 0) {
            return INTERPRET_YIELD;
        }
    }

    if (vm->frame_count == 0) return INTERPRET_OK;
    CallFrame* frame = &vm->frames[vm->frame_count - 1];

    int steps = 0;
    for (;;) {
        profiler_track_instructions(1);
        if (max_steps > 0 && steps >= max_steps) {
            return INTERPRET_YIELD;
        }
        steps++;
        
        if (vm->frame_count <= stop_count) return INTERPRET_OK; // Callback finished
        frame = &vm->frames[vm->frame_count - 1]; // Refresh frame in case of stack changes
        
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
            case OP_ADD: {
                Value b = FRAME_REG(frame, rB);
                Value c = FRAME_REG(frame, rC);
                if (b.type == VAL_NUMBER && c.type == VAL_NUMBER) {
                    FRAME_REG(frame, rA) = (Value){VAL_NUMBER, {.number = b.as.number + c.as.number}};
                } else if (IS_OBJ(b) && AS_OBJ(b)->type == OBJ_STRING) {
                    ObjString* sB = AS_STRING(b);
                    char buf[1024];
                    int len = 0;
                    if (IS_OBJ(c) && AS_OBJ(c)->type == OBJ_STRING) {
                        ObjString* sC = AS_STRING(c);
                        len = snprintf(buf, sizeof(buf), "%s%s", sB->chars, sC->chars);
                    } else if (c.type == VAL_NUMBER) {
                        len = snprintf(buf, sizeof(buf), "%s%g", sB->chars, c.as.number);
                    } else {
                        len = snprintf(buf, sizeof(buf), "%s<obj>", sB->chars);
                    }
                    FRAME_REG(frame, rA) = (Value){VAL_OBJ, {.obj = (Obj*)copy_string(buf, len)}};
                } else if (IS_OBJ(c) && AS_OBJ(c)->type == OBJ_STRING) {
                    ObjString* sC = AS_STRING(c);
                    char buf[1024];
                    int len = 0;
                    if (b.type == VAL_NUMBER) {
                        len = snprintf(buf, sizeof(buf), "%g%s", b.as.number, sC->chars);
                    } else {
                        len = snprintf(buf, sizeof(buf), "<obj>%s", sC->chars);
                    }
                    FRAME_REG(frame, rA) = (Value){VAL_OBJ, {.obj = (Obj*)copy_string(buf, len)}};
                }
                break;
            }
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
            case OP_EQUAL: {
                Value b = FRAME_REG(frame, rB);
                Value c = FRAME_REG(frame, rC);
                bool result = false;
                if (b.type == c.type) {
                    switch (b.type) {
                        case VAL_NIL:    result = true; break;
                        case VAL_BOOL:   result = b.as.boolean == c.as.boolean; break;
                        case VAL_NUMBER: result = b.as.number == c.as.number; break;
                        case VAL_OBJ: {
                            Obj* objB = AS_OBJ(b);
                            Obj* objC = AS_OBJ(c);
                            if (objB->type == objC->type) {
                                if (objB->type == OBJ_STRING) {
                                    ObjString* sB = (ObjString*)objB;
                                    ObjString* sC = (ObjString*)objC;
                                    result = (sB->length == sC->length && 
                                              memcmp(sB->chars, sC->chars, sB->length) == 0);
                                } else {
                                    result = (objB == objC);
                                }
                            }
                            break;
                        }
                    }
                }
                FRAME_REG(frame, rA) = (Value){VAL_BOOL, {.boolean = result}};
                break;
            }
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
            case OP_JUMP_IF_NIL: {
                Value val = FRAME_REG(frame, rA);
                if (val.type == VAL_NIL) { uint16_t offset = (rB << 8) | rC; frame->ip += offset; }
                break;
            }
            case OP_JUMP_IF_NOT_NIL: {
                Value val = FRAME_REG(frame, rA);
                if (val.type != VAL_NIL) { uint16_t offset = (rB << 8) | rC; frame->ip += offset; }
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
                
                // Profile-Guided Optimization: Track hot loops
                // If a function isn't called often but loops internally heavily,
                // compile it to JIT so next time it's invoked, it's native.
                if (frame->function->hot_count++ > 100 && !frame->function->jit_fn) {
                    jit_compile_function(frame->function);
                }
                
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
                        // Tier-2 PGO JIT Compilation
                        if (fn->hot_count++ > 100 && !fn->jit_fn) {
                            jit_compile_function(fn);
                        }
                        
                        // Tier-3 Direct Native Execution
                        if (fn->jit_fn) {
                            // Extract arguments 
                            Value* jit_args = NULL;
                            if (rB > 0) {
                                jit_args = malloc(sizeof(Value) * rB);
                                for (int i = 0; i < rB; i++) {
                                    jit_args[i] = vm->registers[frame->base + rA + 1 + i];
                                }
                            }
                            
                            // Define function signature ptr (matching jit.c signature)
                            typedef Value (*CompiledFn)(int argCount, Value* args, Value* closure_env);
                            CompiledFn jit_exec = (CompiledFn)fn->jit_fn;
                            
                            Value res = jit_exec(rB, jit_args, NULL);
                            
                            if (jit_args) free(jit_args);
                            
                            // Store the result directly without creating CallFrame
                            int next_return = frame->base + rC;
                            if (!ensure_register_capacity(vm, next_return + 1)) vm_fatal_oom();
                            vm->registers[next_return] = res;
                            
                            break; // Skip bytecode processing 
                        }
                        
                        // Default Bytecode Interpreting Path
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
                        if (rB > st->field_count) {
                            printf("Runtime Error: Expected at most %d fields for struct %s, got %d.\n", st->field_count, st->name, rB);
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
                if (!native_is_enabled(rA)) {
                    printf("Runtime Error: Native '%s' is disabled (capability=%s, profile=%s).\n",
                           get_native_name(rA), native_capability(rA), VIPER_PROFILE_NAME);
                    exit(1);
                }
                NativeFn native = get_native_by_index(rA);
                if (native == NULL) {
                    printf("Runtime Error: Unknown native function index %d.\n", rA);
                    exit(1);
                }
                // Arguments are packed right before destination register.
                Value* args = &vm->registers[frame->base + rC - rB];
                Value result = native(rB, args);
                
                if (last_panic_msg != NULL) {
                    if (vm->catch_count > 0) {
                        CatchHandler* handler = &vm->catch_stack[--vm->catch_count];
                        vm->frame_count = handler->frame_count;
                        frame = &vm->frames[vm->frame_count - 1]; // Restore caught frame
                        frame->ip = handler->catch_ip;            // Jump exactly to "else" branch
                        continue; // Jump loop!
                    } else {
                        printf("Panic: %s\n", last_panic_msg);
                        exit(1);
                    }
                }
                
                FRAME_REG(frame, rC) = result;
                break;
            }
            case OP_GET_INDEX: {
                Value target = FRAME_REG(frame, rB);
                Value index = FRAME_REG(frame, rC);
                if (IS_OBJ(target) && AS_OBJ(target)->type == OBJ_ARRAY) {
                    if (!IS_NUMBER(index)) {
                        printf("Runtime Error: Array index must be a number.\n");
                        exit(1);
                    }
                    ObjArray* array = (ObjArray*)AS_OBJ(target);
                    int idx = (int)index.as.number;
                    if (idx < 0 || idx >= array->count) {
                        printf("Runtime Error: Array index out of bounds.\n");
                        exit(1);
                    }
                    FRAME_REG(frame, rA) = array->elements[idx];
                } else if (IS_OBJ(target) && AS_OBJ(target)->type == OBJ_INSTANCE) {
                    if (!IS_OBJ(index) || AS_OBJ(index)->type != OBJ_STRING) {
                        printf("Runtime Error: Instance property index must be a string.\n");
                        exit(1);
                    }
                    ObjInstance* instance = (ObjInstance*)AS_OBJ(target);
                    ObjString* fieldName = AS_STRING(index);
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
                } else {
                    printf("Runtime Error: Only arrays and instances support indexing.\n");
                    exit(1);
                }
                break;
            }
            case OP_SET_INDEX: {
                Value target = FRAME_REG(frame, rA);
                Value index = FRAME_REG(frame, rB);
                Value value = FRAME_REG(frame, rC);
                if (IS_OBJ(target) && AS_OBJ(target)->type == OBJ_ARRAY) {
                    if (!IS_NUMBER(index)) {
                        printf("Runtime Error: Array index must be a number.\n");
                        exit(1);
                    }
                    ObjArray* array = (ObjArray*)AS_OBJ(target);
                    int idx = (int)index.as.number;
                    if (idx < 0 || idx >= array->count) {
                        printf("Runtime Error: Array index out of bounds.\n");
                        exit(1);
                    }
                    array->elements[idx] = value;
                } else if (IS_OBJ(target) && AS_OBJ(target)->type == OBJ_INSTANCE) {
                    if (!IS_OBJ(index) || AS_OBJ(index)->type != OBJ_STRING) {
                        printf("Runtime Error: Instance property index must be a string.\n");
                        exit(1);
                    }
                    ObjInstance* instance = (ObjInstance*)AS_OBJ(target);
                    ObjString* fieldName = AS_STRING(index);
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
                    instance->fields[idx] = value;
                } else {
                    printf("Runtime Error: Only arrays and instances support indexing.\n");
                    exit(1);
                }
                break;
            }
            case OP_MATCH: {
                Value target = FRAME_REG(frame, rB);
                Value pattern = FRAME_REG(frame, rC);
                if (!IS_OBJ(target) || AS_OBJ(target)->type != OBJ_STRING ||
                    !IS_OBJ(pattern) || AS_OBJ(pattern)->type != OBJ_STRING) {
                    FRAME_REG(frame, rA) = (Value){VAL_BOOL, {.boolean = false}};
                    break;
                }
                
                regex_t regex;
                int ret = regcomp(&regex, AS_STRING(pattern)->chars, REG_EXTENDED);
                if (ret) {
                    FRAME_REG(frame, rA) = (Value){VAL_BOOL, {.boolean = false}};
                    break;
                }
                
                ret = regexec(&regex, AS_STRING(target)->chars, 0, NULL, 0);
                regfree(&regex);
                
                FRAME_REG(frame, rA) = (Value){VAL_BOOL, {.boolean = (ret == 0)}};
                break;
            }
            case OP_SPAWN: {
                Value callee = FRAME_REG(frame, rB);
                if (!IS_OBJ(callee) || AS_OBJ(callee)->type != OBJ_FUNCTION) {
                    printf("Runtime Error: Can only spawn functions.\n");
                    exit(1);
                }
                
                ObjFunction* fn = (ObjFunction*)AS_OBJ(callee);
                if (rC != fn->arity) {
                    printf("Runtime Error: Spawn expected %d args, got %d.\n", fn->arity, rC);
                    exit(1);
                }
                
                VM* child_vm = malloc(sizeof(VM));
                init_vm(child_vm);
                
                // Share globals and mutex
                child_vm->global_names = vm->global_names;
                child_vm->global_values = vm->global_values;
                child_vm->global_count = vm->global_count;
                child_vm->global_capacity = vm->global_capacity;
                if (child_vm->global_mutex) free(child_vm->global_mutex);
                child_vm->global_mutex = vm->global_mutex;

                // Setup the frame for the function
                if (!ensure_frame_capacity(child_vm, 1) || !ensure_register_capacity(child_vm, REGISTER_WINDOW)) {
                    printf("Runtime Error: Out of memory spawning fiber.\n");
                    exit(1);
                }
                
                // fn is at base
                child_vm->registers[0] = (Value){VAL_OBJ, {.obj = (Obj*)fn}};
                for (int i=0; i<rC; i++) {
                    child_vm->registers[1 + i] = vm->registers[frame->base + rB + 1 + i];
                }
                
                CallFrame* cframe = &child_vm->frames[child_vm->frame_count++];
                cframe->function = fn;
                cframe->ip = 0;
                cframe->base = 0;
                cframe->return_dest = 255;
                
                ObjThread* t = new_thread(0); 
                t->finished = false;
                child_vm->thread_obj = t;
                
                schedule_fiber(child_vm);
                
                FRAME_REG(frame, rA) = (Value){VAL_OBJ, {.obj = (Obj*)t}};
                break;
            }
            case OP_AWAIT: {
                Value th = FRAME_REG(frame, rB);
                if (!IS_OBJ(th) || AS_OBJ(th)->type != OBJ_THREAD) {
                    printf("Runtime Error: Can only await Thread objects.\n");
                    exit(1);
                }
                ObjThread* t = (ObjThread*)AS_OBJ(th);
                if (!t->finished) {
                    // Preempt the current VM so the spawned tasks can execute
                    frame->ip--; // retry this instruction next time
                    return INTERPRET_YIELD;
                }
                if (!t->joined) {
                    t->joined = true;
                }
                FRAME_REG(frame, rA) = t->result;
                break;
            }
            case OP_RETURN: {
                Value retVal = FRAME_REG(frame, rA);
                int dest     = frame->return_dest;

                vm->frame_count--;
                
                // Ensure the return destination exists and store the result
                if (!ensure_register_capacity(vm, dest + 1)) {
                    vm_fatal_oom();
                }
                vm->registers[dest] = retVal;

                if (vm->frame_count == 0) return INTERPRET_OK; // End of execution or top level return
                frame = &vm->frames[vm->frame_count - 1];
                break;
            }
            case OP_TYPEOF: {
                Value val = FRAME_REG(frame, rB);
                const char* type_str = "unknown";
                
                switch (val.type) {
                    case VAL_NUMBER: type_str = "number"; break;
                    case VAL_BOOL:   type_str = "bool"; break;
                    case VAL_NIL:    type_str = "nil"; break;
                    case VAL_OBJ:
                        switch (AS_OBJ(val)->type) {
                            case OBJ_STRING:       type_str = "string"; break;
                            case OBJ_ARRAY:        type_str = "array"; break;
                            case OBJ_FUNCTION:     type_str = "function"; break;
                            case OBJ_STRUCT:       type_str = "struct"; break;
                            case OBJ_INSTANCE:     type_str = "instance"; break;
                            case OBJ_NATIVE:       type_str = "native"; break;
                            case OBJ_DL_HANDLE:    type_str = "dl_handle"; break;
                            case OBJ_DYNAMIC_FUNC: type_str = "dynamic_fn"; break;
                            case OBJ_POINTER:      type_str = "pointer"; break;
                            case OBJ_THREAD:       type_str = "thread"; break;
                        }
                        break;
                }
                
                ObjString* strObj = copy_string(type_str, strlen(type_str));
                FRAME_REG(frame, rA) = (Value){VAL_OBJ, {.obj = (Obj*)strObj}};
                break;
            }
            case OP_CLONE: {
                FRAME_REG(frame, rA) = deep_clone_value(FRAME_REG(frame, rB));
                break;
            }
            case OP_ARRAY: {
                int count = rB;
                int startReg = rC;
                
                ObjArray* arr = new_array();
                for (int i = 0; i < count; i++) {
                    array_append(arr, FRAME_REG(frame, startReg + i));
                }
                
                FRAME_REG(frame, rA) = (Value){VAL_OBJ, {.obj = (Obj*)arr}};
                break;
            }
            case OP_SETUP_TRY: {
                uint32_t current_ip = (frame->ip - 1); // Point back to start of instruction
                uint16_t offset = (DECODE_B(inst) << 8) | DECODE_C(inst);
                
                if (vm->catch_count >= MAX_CATCH_HANDLERS) {
                    printf("Runtime Error: Exceeded maximum nested try blocks.\n");
                    exit(1);
                }
                
                CatchHandler* handler = &vm->catch_stack[vm->catch_count++];
                handler->catch_ip = current_ip + offset;
                handler->target_vp = frame->base + rA;
                handler->frame_count = vm->frame_count;
                break;
            }
            case OP_TEARDOWN_TRY: {
                vm->catch_count--;
                break;
            }
            case OP_HAS: {
                Value valObj = FRAME_REG(frame, rB);
                Value valProp = FRAME_REG(frame, rC);
                if (!IS_OBJ(valObj) || AS_OBJ(valObj)->type != OBJ_INSTANCE) {
                    FRAME_REG(frame, rA) = (Value){VAL_BOOL, {.boolean = false}};
                    break;
                }
                if (!IS_OBJ(valProp) || AS_OBJ(valProp)->type != OBJ_STRING) {
                    FRAME_REG(frame, rA) = (Value){VAL_BOOL, {.boolean = false}};
                    break;
                }
                ObjInstance* instance = (ObjInstance*)AS_OBJ(valObj);
                ObjString* propName = AS_STRING(valProp);
                bool found = false;
                for (int i = 0; i < instance->klass->field_count; i++) {
                    if (instance->klass->field_name_lens[i] == propName->length &&
                        memcmp(instance->klass->field_names[i], propName->chars, propName->length) == 0) {
                        found = true;
                        break;
                    }
                }
                FRAME_REG(frame, rA) = (Value){VAL_BOOL, {.boolean = found}};
                break;
            }
            case OP_KEYS: {
                Value valObj = FRAME_REG(frame, rB);
                if (!IS_OBJ(valObj) || AS_OBJ(valObj)->type != OBJ_INSTANCE) {
                    FRAME_REG(frame, rA) = (Value){VAL_OBJ, {.obj = (Obj*)new_array()}};
                    break;
                }
                ObjInstance* instance = (ObjInstance*)AS_OBJ(valObj);
                ObjArray* keys = new_array();
                for (int i = 0; i < instance->klass->field_count; i++) {
                    ObjString* s = copy_string(instance->klass->field_names[i], instance->klass->field_name_lens[i]);
                    array_append(keys, (Value){VAL_OBJ, {.obj = (Obj*)s}});
                }
                FRAME_REG(frame, rA) = (Value){VAL_OBJ, {.obj = (Obj*)keys}};
                break;
            }
            case OP_EVAL: {
                Value valCode = FRAME_REG(frame, rB);
                if (!IS_OBJ(valCode) || AS_OBJ(valCode)->type != OBJ_STRING) {
                    printf("Runtime Error: eval() expects string.\n");
                    exit(1);
                }
                ObjString* codeStr = AS_STRING(valCode);
                AstNode* ast = parse(codeStr->chars);
                if (!ast) {
                    printf("Runtime Error: eval() failed to parse code.\n");
                    exit(1);
                }
                
                // We want eval to return a value, so force compiler to emit RETURN instead of HALT
                compiler_set_emit_halt(false);
                ObjFunction* fn = compile(ast);
                compiler_set_emit_halt(true); // Restore default
                
                if (!fn) {
                    printf("Runtime Error: eval() failed to compile code.\n");
                    exit(1);
                }
                
                Value result = call_viper_function(fn, 0, NULL);
                FRAME_REG(frame, rA) = result;
                break;
            }
            case OP_SYNC_START: {
                pthread_mutex_lock(vm->global_mutex);
                break;
            }
            case OP_SYNC_END: {
                pthread_mutex_unlock(vm->global_mutex);
                break;
            }
            case OP_ASSERT_TYPE: {
                // OP_ASSERT_TYPE rA=value_reg, rB=const_index_of_type_str, rC=unused
                Value val = FRAME_REG(frame, rA);
                ObjString* expected = AS_STRING(frame->function->chunk.constants[rB]);
                
                const char* actual_type = NULL;
                switch (val.type) {
                    case VAL_NUMBER:  actual_type = "int";    break; // Viper uses 'int' for numeric
                    case VAL_BOOL:    actual_type = "bool";   break;
                    case VAL_NIL:     actual_type = "nil";    break;
                    case VAL_OBJ: {
                        Obj* obj = AS_OBJ(val);
                        switch (obj->type) {
                            case OBJ_STRING:   actual_type = "str";    break;
                            case OBJ_ARRAY:    actual_type = "array";  break;
                            case OBJ_FUNCTION: actual_type = "fn";     break;
                            case OBJ_INSTANCE: actual_type = "struct"; break;
                            default:           actual_type = "obj";    break;
                        }
                        break;
                    }
                }
                
                // Also accept "float" as alias for numeric
                bool type_ok = false;
                if (actual_type && (int)strlen(actual_type) == expected->length &&
                    memcmp(actual_type, expected->chars, expected->length) == 0) {
                    type_ok = true;
                }
                // Additional alias: "float" and "f" match numbers too
                if (!type_ok && val.type == VAL_NUMBER) {
                    if ((expected->length == 5 && memcmp(expected->chars, "float", 5) == 0) ||
                        (expected->length == 1 && expected->chars[0] == 'f') ||
                        (expected->length == 1 && expected->chars[0] == 'i')) {
                        type_ok = true;
                    }
                }
                // "any" always passes
                if (!type_ok && expected->length == 3 && memcmp(expected->chars, "any", 3) == 0) {
                    type_ok = true;
                }
                // "s" is alias for "str"
                if (!type_ok && val.type == VAL_OBJ && AS_OBJ(val)->type == OBJ_STRING) {
                    if (expected->length == 1 && expected->chars[0] == 's') {
                        type_ok = true;
                    }
                }
                // "b" is alias for "bool"
                if (!type_ok && val.type == VAL_BOOL) {
                    if (expected->length == 1 && expected->chars[0] == 'b') {
                        type_ok = true;
                    }
                }
                
                if (!type_ok) {
                    printf("TypeError: Expected type '%.*s', got '%s'\n",
                           expected->length, expected->chars,
                           actual_type ? actual_type : "unknown");
                    exit(1);
                }
                break;
            }
            case OP_GET_GLOBAL: {
                ObjString* name = AS_STRING(frame->function->chunk.constants[rB]);
                Value val = (Value){VAL_NIL, {.number = 0}};
                for (int i = 0; i < vm->global_count; i++) {
                    if (vm->global_names[i]->length == name->length &&
                        memcmp(vm->global_names[i]->chars, name->chars, name->length) == 0) {
                        val = vm->global_values[i];
                        break;
                    }
                }
                FRAME_REG(frame, rA) = val;
                break;
            }
            case OP_SET_GLOBAL: {
                ObjString* name = AS_STRING(frame->function->chunk.constants[rB]);
                Value val = FRAME_REG(frame, rA);
                int found = -1;
                for (int i = 0; i < vm->global_count; i++) {
                    if (vm->global_names[i]->length == name->length &&
                        memcmp(vm->global_names[i]->chars, name->chars, name->length) == 0) {
                        found = i; break;
                    }
                }
                if (found != -1) {
                    vm->global_values[found] = val;
                } else {
                    if (vm->global_count >= vm->global_capacity) {
                        vm->global_capacity = vm->global_capacity < 8 ? 8 : vm->global_capacity * 2;
                        vm->global_names = realloc(vm->global_names, sizeof(ObjString*) * vm->global_capacity);
                        vm->global_values = realloc(vm->global_values, sizeof(Value) * vm->global_capacity);
                    }
                    vm->global_names[vm->global_count] = name;
                    vm->global_values[vm->global_count] = val;
                    vm->global_count++;
                }
                break;
            }
            case OP_HALT:
                return INTERPRET_OK;
            default:
                printf("VM Error: Unknown opcode %d.\n", op);
                exit(1);
        }
    }
}
