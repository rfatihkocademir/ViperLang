#include "native.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Obj* objects = NULL;

// Allocate a generic object and add to memory chain
static Obj* allocate_obj(size_t size, int type) {
    Obj* object = (Obj*)malloc(size);
    object->type = type;
    object->ref_count = 1; // start with 1 reference
    object->next = objects;
    objects = object;
    return object;
}

// FNV-1a hash algorithm for fast string hashing
static uint32_t hash_string(const char* key, int length) {
    uint32_t hash = 2166136261u;
    for (int i = 0; i < length; i++) {
        hash ^= (uint8_t)key[i];
        hash *= 16777619;
    }
    return hash;
}

ObjString* copy_string(const char* chars, int length) {
    uint32_t hash = hash_string(chars, length);
    
    char* heapChars = malloc(length + 1);
    memcpy(heapChars, chars, length);
    heapChars[length] = '\0';
    
    ObjString* string = (ObjString*)allocate_obj(sizeof(ObjString), OBJ_STRING);
    string->length = length;
    string->chars = heapChars;
    string->hash = hash;
    return string;
}

ObjFunction* new_function(const char* name, int name_len, int arity) {
    ObjFunction* fn = (ObjFunction*)allocate_obj(sizeof(ObjFunction), OBJ_FUNCTION);
    fn->arity = arity;
    fn->name = name;
    fn->name_len = name_len;
    fn->base_reg = 0;
    init_chunk(&fn->chunk);
    return fn;
}

ObjStruct* new_struct(const char* name, int name_len, int field_count, const char** fields, int* field_lens) {
    ObjStruct* st = (ObjStruct*)allocate_obj(sizeof(ObjStruct), OBJ_STRUCT);
    st->name = name;
    st->name_len = name_len;
    st->field_count = field_count;
    st->field_names = fields;
    st->field_name_lens = field_lens;
    return st;
}

ObjInstance* new_instance(ObjStruct* klass) {
    ObjInstance* instance = (ObjInstance*)allocate_obj(sizeof(ObjInstance), OBJ_INSTANCE);
    instance->klass = klass;
    instance->fields = malloc(sizeof(Value) * klass->field_count);
    for (int i = 0; i < klass->field_count; i++) {
        instance->fields[i] = (Value){VAL_NIL, {.number = 0}};
    }
    return instance;
}

void retain_obj(Obj* obj) {
    if (obj == NULL) return;
    obj->ref_count++;
}

void release_obj(Obj* obj) {
    if (obj == NULL) return;
    obj->ref_count--;
    
    if (obj->ref_count <= 0) {
        if (obj->type == OBJ_STRING) {
            ObjString* string = (ObjString*)obj;
            free(string->chars);
        } else if (obj->type == OBJ_FUNCTION) {
            ObjFunction* fn = (ObjFunction*)obj;
            free(fn->chunk.code);
            free(fn->chunk.constants);
        } else if (obj->type == OBJ_STRUCT) {
            ObjStruct* st = (ObjStruct*)obj;
            free((void*)st->field_names);
            free(st->field_name_lens);
        } else if (obj->type == OBJ_INSTANCE) {
            ObjInstance* instance = (ObjInstance*)obj;
            free(instance->fields);
        }
        free(obj);
    }
}

void print_value(Value value) {
    switch (value.type) {
        case VAL_NUMBER:
            printf("%.2f", value.as.number);
            break;
        case VAL_BOOL:
            printf("%s", value.as.boolean ? "true" : "false");
            break;
        case VAL_NIL:
            printf("nil");
            break;
        case VAL_OBJ:
            switch (AS_OBJ(value)->type) {
            case OBJ_STRING:
                printf("%s", AS_STRING(value)->chars);
                break;
            case OBJ_FUNCTION:
                printf("<fn %.*s>", AS_FUNCTION(value)->name_len, AS_FUNCTION(value)->name);
                break;
            case OBJ_STRUCT:
                printf("<st %.*s>", ((ObjStruct*)AS_OBJ(value))->name_len, ((ObjStruct*)AS_OBJ(value))->name);
                break;
            case OBJ_INSTANCE:
                printf("<%.*s instance>", ((ObjInstance*)AS_OBJ(value))->klass->name_len, ((ObjInstance*)AS_OBJ(value))->klass->name);
                break;
            default: printf("<obj %d>", AS_OBJ(value)->type); break;
            }
            break;
    }
}

// The most basic native function: `print`
static Value native_print(int argCount, Value* args) {
    if (argCount == 0) return (Value){VAL_NIL, {.number=0}};
    
    for (int i=0; i<argCount; i++) {
        print_value(args[i]);
    }
    printf("\n");
    return (Value){VAL_NIL, {.number = 0}};
}

void init_native_core() {
    // In a full implementation we would register "print" into global scope hashing here.
    // For this demonstration, we expose the structure.
    push_native("print", native_print);
}

void push_native(const char* name, NativeFn function) {
    (void)function; // Bypass unused variable warning for this skeleton
    // Simulating registering a native C function into the ViperVM Global Map
    printf("Native Function '%s' registered via C-FFI Zero-Cost interface.\n", name);
}
