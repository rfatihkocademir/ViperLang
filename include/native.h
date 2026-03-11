#ifndef VIPER_NATIVE_H
#define VIPER_NATIVE_H

#include "vm.h"
#include <stdbool.h>
#include <pthread.h>

// The base object for GC / Reference Counting
struct sObj {
    int type; // ObjType
    int ref_count; // The core of ViperLang's deterministic memory
    struct sObj* next; // To keep track of all allocated objects
};

typedef struct sObjString ObjString;
typedef struct sObjArray ObjArray;
typedef struct sObjFunction ObjFunction;
typedef struct sObjStruct ObjStruct;
typedef struct sObjInstance ObjInstance;
typedef struct sObjDlHandle ObjDlHandle;
typedef struct sObjDynamicFunction ObjDynamicFunction;
typedef struct sObjPointer ObjPointer;
typedef struct sObjThread ObjThread;

// String Object
struct sObjString {
    Obj obj;
    int length;
    char* chars;
    uint32_t hash;
};

// Array Object (e.g. [T; N] bounded arrays)
struct sObjArray {
    Obj obj;
    int capacity;
    int count;
    Value* elements;
};

struct sObjFunction {
    Obj obj;
    int arity;           // Number of parameters
    Chunk chunk;         // The function's own bytecode chunk
    const char* name;    // For @contract output and error messages
    int name_len;
    int base_reg;        // The register offset inside a CallFrame
    
    // Tiered JIT Support
    int hot_count;       // Track how many times it was called
    void* jit_fn;        // Pointer to the dynamically compiled C-Backend function
};

struct sObjStruct {
    Obj obj;
    const char* name;
    int name_len;
    int field_count;
    const char** field_names;
    int* field_name_lens;
};

struct sObjInstance {
    Obj obj;
    ObjStruct* klass;
    Value* fields; // Array of values matching klass->field_count
};

struct sObjDlHandle {
    Obj obj;
    void* handle;
};

struct sObjPointer {
    Obj obj;
    void* ptr;
};

struct sObjDynamicFunction {
    Obj obj;
    void* fn_ptr;
    const char* signature;
    int sig_len;
    void* cif;           // ffi_cif* point (cast to void* in header to avoid ffi.h include here)
    void** arg_types;    // ffi_type** array
    void* return_type;   // ffi_type*
};

struct sObjThread {
    Obj obj;
    pthread_t thread;
    Value result;
    bool finished;     // True if the thread has completed
    bool joined;       // True if another thread has already awaited this
};

// Object type constants
#define OBJ_STRING        1
#define OBJ_ARRAY         2
#define OBJ_FUNCTION      3
#define OBJ_STRUCT        4
#define OBJ_INSTANCE      5
#define OBJ_NATIVE        6
#define OBJ_DL_HANDLE     7
#define OBJ_DYNAMIC_FUNC  8
#define OBJ_POINTER       9
#define OBJ_THREAD        10

// String methods
ObjString* copy_string(const char* chars, int length);
ObjFunction* new_function(const char* name, int name_len, int arity);
ObjStruct* new_struct(const char* name, int name_len, int field_count, const char** fields, int* field_lens);
ObjInstance* new_instance(ObjStruct* klass);
ObjDlHandle* new_dl_handle(void* handle);
ObjDynamicFunction* new_dynamic_function(void* fn_ptr, const char* signature, int sig_len, void* cif, void** arg_types, void* return_type);
ObjPointer* new_pointer(void* ptr);
ObjThread* new_thread(pthread_t thread);
ObjArray* new_array();
void array_append(ObjArray* array, Value value);
void print_value(Value value);

// Native Function Signature
typedef Value (*NativeFn)(int argCount, Value* args);

void init_native_core();
void push_native(const char* name, NativeFn function);
int find_native_index(const char* name, int length);
NativeFn get_native_by_index(int index);
const char* get_native_name(int index);
bool native_is_enabled(int index);
const char* native_capability(int index);
int native_count();

// Memory Management (Reference Counting Core)
void retain_obj(Obj* obj);
void release_obj(Obj* obj);

#define IS_OBJ(value)     ((value).type == VAL_OBJ)
#define IS_NUMBER(value)  ((value).type == VAL_NUMBER)
#define IS_BOOL(value)    ((value).type == VAL_BOOL)
#define AS_OBJ(value)     ((value).as.obj)
#define AS_STRING(value)  ((ObjString*)AS_OBJ(value))
#define AS_FUNCTION(value)((ObjFunction*)AS_OBJ(value))
#define AS_ARRAY(value)   ((ObjArray*)AS_OBJ(value))

#endif // VIPER_NATIVE_H
