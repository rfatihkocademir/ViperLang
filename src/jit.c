#include "jit.h"
#include "ir.h"
#include "profiler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>

void jit_compile_function(ObjFunction* fn) {
    if (!fn || fn->jit_fn) return;
    if (fn->hot_count == -1) return; // Prevent infinite retries
    if (fn->name_len == 8 && memcmp(fn->name, "__main__", 8) == 0) return;

    // 1. Generate IR
    IrBlock* ir = ir_from_chunk(&fn->chunk);
    if (!ir) {
        fn->hot_count = -1;
        return;
    }

    // 2. Transpile to C
    char* c_src = ir_to_c_source(ir, fn);
    if (!c_src) {
        free_ir_block(ir);
        fn->hot_count = -1;
        return;
    }

    // 3. Save to temp file
    char src_path[256];
    char so_path[256];
    snprintf(src_path, sizeof(src_path), "/tmp/viper_jit_%.*s.c", fn->name_len, fn->name);
    snprintf(so_path, sizeof(so_path), "/tmp/viper_jit_%.*s.so", fn->name_len, fn->name);

    FILE* f = fopen(src_path, "w");
    if (!f) {
        free(c_src);
        free_ir_block(ir);
        fn->hot_count = -1;
        return;
    }
    fputs(c_src, f);
    fclose(f);

    // 4. Compile via GCC
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "gcc -O2 -fPIC -shared -Iinclude %s -o %s 2> /dev/null", src_path, so_path);
    int res = system(cmd);
    if (res != 0) {
        unlink(src_path);
        free(c_src);
        free_ir_block(ir);
        fn->hot_count = -1;
        return;
    }

    // 5. Dynamic Load
    void* handle = dlopen(so_path, RTLD_NOW);
    if (!handle) {
        unlink(src_path);
        unlink(so_path);
        free(c_src);
        free_ir_block(ir);
        fn->hot_count = -1;
        return;
    }

    char sym_name[256];
    snprintf(sym_name, sizeof(sym_name), "%.*s_jit", fn->name_len, fn->name);
    fn->jit_fn = dlsym(handle, sym_name);

    if (!fn->jit_fn) {
        dlclose(handle);
        unlink(so_path);
        fn->hot_count = -1;
    }

    // Cleanup
    unlink(src_path);
    free(c_src);
    free_ir_block(ir);
}

Value jit_execute(void* jit_fn, int argCount, Value* args) {
    if (!jit_fn) return (Value){VAL_NIL, {.number = 0}};
    typedef Value (*CompiledFn)(int argCount, Value* args, Value* closure);
    CompiledFn exec = (CompiledFn)jit_fn;
    return exec(argCount, args, NULL);
}
