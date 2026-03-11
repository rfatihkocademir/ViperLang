#ifndef VIPER_JIT_H
#define VIPER_JIT_H

#include "native.h"

void jit_compile_function(ObjFunction* fn);
Value jit_execute(void* jit_fn, int argCount, Value* args);

#endif // VIPER_JIT_H
