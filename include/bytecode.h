#ifndef VIPER_BYTECODE_H
#define VIPER_BYTECODE_H

#include <stdbool.h>

#include "native.h"

// Serializes raw source text into a binary source-cache file.
bool write_source_cache_file(const char* path, const char* source);

// Loads source text from a binary source-cache file.
// Returned buffer is heap-allocated and must be freed by caller.
char* read_source_cache_file(const char* path);

// Serializes a compiled function graph into a binary bytecode file.
bool write_bytecode_file(const char* path, ObjFunction* main_fn);

// Loads a compiled function graph from a binary bytecode file.
// Returns NULL on failure.
ObjFunction* read_bytecode_file(const char* path);

#endif // VIPER_BYTECODE_H
