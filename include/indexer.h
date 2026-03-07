#ifndef VIPER_INDEXER_H
#define VIPER_INDEXER_H

#include <stdbool.h>

// Emits a machine-readable semantic index for the entry script and its modules.
// If out_path is NULL, JSON is written to stdout.
bool emit_semantic_index_json(const char* entry_file, const char* out_path);

#endif // VIPER_INDEXER_H
