#include "native.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <ffi.h>

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

ObjDlHandle* new_dl_handle(void* handle) {
    ObjDlHandle* dl = (ObjDlHandle*)allocate_obj(sizeof(ObjDlHandle), OBJ_DL_HANDLE);
    dl->handle = handle;
    return dl;
}

ObjDynamicFunction* new_dynamic_function(void* fn_ptr, const char* signature, int sig_len, void* cif, void** arg_types, void* return_type) {
    ObjDynamicFunction* dyn = (ObjDynamicFunction*)allocate_obj(sizeof(ObjDynamicFunction), OBJ_DYNAMIC_FUNC);
    dyn->fn_ptr = fn_ptr;
    dyn->signature = signature;
    dyn->sig_len = sig_len;
    dyn->cif = cif;
    dyn->arg_types = arg_types;
    dyn->return_type = return_type;
    dyn->return_type = return_type;
    return dyn;
}

ObjPointer* new_pointer(void* ptr) {
    ObjPointer* p = (ObjPointer*)allocate_obj(sizeof(ObjPointer), OBJ_POINTER);
    p->ptr = ptr;
    return p;
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
        } else if (obj->type == OBJ_DL_HANDLE) {
            ObjDlHandle* dl = (ObjDlHandle*)obj;
            if (dl->handle) dlclose(dl->handle);
        } else if (obj->type == OBJ_DYNAMIC_FUNC) {
            ObjDynamicFunction* dyn = (ObjDynamicFunction*)obj;
            free((void*)dyn->signature);
            free(dyn->cif);
            free(dyn->arg_types);
            // return_type points to static ffi_type, so we don't free it
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
            case OBJ_DL_HANDLE:
                printf("<dl_handle %p>", ((ObjDlHandle*)AS_OBJ(value))->handle);
                break;
            case OBJ_DYNAMIC_FUNC:
                printf("<dyn_fn %.*s %p>", ((ObjDynamicFunction*)AS_OBJ(value))->sig_len, ((ObjDynamicFunction*)AS_OBJ(value))->signature, ((ObjDynamicFunction*)AS_OBJ(value))->fn_ptr);
                break;
            case OBJ_POINTER:
                printf("<ptr %p>", ((ObjPointer*)AS_OBJ(value))->ptr);
                break;
            default: printf("<obj %d>", AS_OBJ(value)->type); break;
            }
            break;
    }
}

static Value native_print(int argCount, Value* args) {
    for (int i=0; i<argCount; i++) {
        print_value(args[i]);
        if (i < argCount - 1) printf(" ");
    }
    printf("\n");
    return (Value){VAL_NIL, {.number = 0}};
}

// ---- Native Registry ----
typedef struct {
    const char* name;
    NativeFn function;
} NativeEntry;

#define MAX_NATIVES 128
static NativeEntry native_registry[MAX_NATIVES];
static int _native_count = 0;

void push_native(const char* name, NativeFn function) {
    if (_native_count >= MAX_NATIVES) return;
    native_registry[_native_count].name = name;
    native_registry[_native_count].function = function;
    _native_count++;
}

int find_native_index(const char* name, int length) {
    for (int i = 0; i < _native_count; i++) {
        if (strlen(native_registry[i].name) == (size_t)length &&
            memcmp(native_registry[i].name, name, length) == 0) {
            return i;
        }
    }
    
    return -1;
}

NativeFn get_native_by_index(int index) {
    if (index < 0 || index >= _native_count) return NULL;
    return native_registry[index].function;
}

const char* get_native_name(int index) {
    if (index < 0 || index >= _native_count) return "unknown";
    return native_registry[index].name;
}

int native_count() {
    return _native_count;
}

#include <time.h>
static Value native_clock(int argCount, Value* args) {
    (void)argCount; (void)args;
    return (Value){VAL_NUMBER, {.number = (double)clock() / CLOCKS_PER_SEC}};
}

static Value native_load_dl(int argCount, Value* args) {
    if (argCount != 1 || !IS_OBJ(args[0]) || AS_OBJ(args[0])->type != OBJ_STRING) {
        printf("Runtime Error: load_dl expects a string argument.\n");
        exit(1);
    }
    const char* path = AS_STRING(args[0])->chars;
    void* handle = dlopen(path, RTLD_LAZY);
    if (!handle) {
        printf("Runtime Error: Failed to load library '%s': %s\n", path, dlerror());
        exit(1);
    }
    return (Value){VAL_OBJ, {.obj = (Obj*)new_dl_handle(handle)}};
}

static ffi_type* parse_ffi_type(const char** p) {
    char c = **p;
    if (!c) return NULL;
    (*p)++;
    switch (c) {
        case 'v': return &ffi_type_void;
        case 'i': return &ffi_type_sint32;
        case 'I': return &ffi_type_sint64;
        case 'f': return &ffi_type_float;
        case 'd': return &ffi_type_double;
        case 'p': return &ffi_type_pointer;
        case 's': return &ffi_type_pointer; // String is passed as (char*) pointer
        case '{': {
             int cap = 4;
             int count = 0;
             ffi_type** elements = malloc((cap + 1) * sizeof(ffi_type*));
             while (**p && **p != '}') {
                 if (**p == ',') { (*p)++; continue; }
                 ffi_type* elem = parse_ffi_type(p);
                 if (!elem) { free(elements); return NULL; }
                 if (count == cap) {
                     cap *= 2;
                     elements = realloc(elements, (cap + 1) * sizeof(ffi_type*));
                 }
                 elements[count++] = elem;
             }
             if (**p == '}') (*p)++;
             elements[count] = NULL;
             
             ffi_type* st = malloc(sizeof(ffi_type));
             st->size = 0;
             st->alignment = 0;
             st->type = FFI_TYPE_STRUCT;
             st->elements = elements;
             return st;
        }
        default:  return NULL;
    }
}

static Value native_get_fn(int argCount, Value* args) {
    if (argCount != 3 || !IS_OBJ(args[0]) || AS_OBJ(args[0])->type != OBJ_DL_HANDLE ||
        !IS_OBJ(args[1]) || AS_OBJ(args[1])->type != OBJ_STRING || 
        !IS_OBJ(args[2]) || AS_OBJ(args[2])->type != OBJ_STRING) {
        printf("Runtime Error: get_fn expects (dl_handle, fn_name, signature).\n");
        exit(1);
    }
    
    ObjDlHandle* dl = (ObjDlHandle*)AS_OBJ(args[0]);
    const char* fn_name = AS_STRING(args[1])->chars;
    const char* signature = AS_STRING(args[2])->chars;
    int sig_len = AS_STRING(args[2])->length;
    
    void* fn_ptr = dlsym(dl->handle, fn_name);
    if (!fn_ptr) {
        printf("Runtime Error: Symbol '%s' not found.\n", fn_name);
        exit(1);
    }

    // Parse simple signature like "i,d->d"
    // Find separator "->"
    const char* sep = strstr(signature, "->");
    if (!sep) {
        printf("Runtime Error: Invalid signature format '%s'. Expected '...->type'.\n", signature);
        exit(1);
    }
    
    const char* r_ptr = sep + 2;
    ffi_type* rtype = parse_ffi_type(&r_ptr);
    if (!rtype) {
        printf("Runtime Error: Unknown return type in signature.\n");
        exit(1);
    }
    
    int arg_cap = 4;
    int num_args = 0;
    ffi_type** arg_types = malloc(arg_cap * sizeof(ffi_type*));
    
    const char* p = signature;
    while (p < sep) {
        if (*p == ',') { p++; continue; }
        ffi_type* t = parse_ffi_type(&p);
        if (!t) {
            printf("Runtime Error: Unknown argument type in signature.\n");
            exit(1);
        }
        if (num_args == arg_cap) {
            arg_cap *= 2;
            arg_types = realloc(arg_types, arg_cap * sizeof(ffi_type*));
        }
        arg_types[num_args++] = t;
    }
    
    ffi_cif* cif = malloc(sizeof(ffi_cif));
    if (ffi_prep_cif(cif, FFI_DEFAULT_ABI, num_args, rtype, arg_types) != FFI_OK) {
        printf("Runtime Error: ffi_prep_cif failed.\n");
        exit(1);
    }
    
    // Copy signature string for GC to track correctly
    char* sig_copy = malloc(sig_len + 1);
    memcpy(sig_copy, signature, sig_len);
    sig_copy[sig_len] = '\0';
    
    ObjDynamicFunction* dyn = new_dynamic_function(fn_ptr, sig_copy, sig_len, cif, (void**)arg_types, rtype);
    return (Value){VAL_OBJ, {.obj = (Obj*)dyn}};
}

void init_native_core() {
    _native_count = 0;
    push_native("pr", native_print);
    push_native("clock", native_clock);
    push_native("load_dl", native_load_dl);
    push_native("get_fn", native_get_fn);
}
