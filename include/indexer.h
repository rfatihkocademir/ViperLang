#ifndef VIPER_INDEXER_H
#define VIPER_INDEXER_H

#include <stdbool.h>

// Emits a machine-readable semantic index for the entry script and its modules.
// If out_path is NULL, JSON is written to stdout.
bool emit_semantic_index_json(const char* entry_file, const char* out_path);

// Emits a minimal resume packet from a persistent semantic state artifact.
// The artifact is verified against current tracked file hashes without re-indexing source.
// If out_path is NULL, text is written to stdout.
bool resume_project_state(const char* state_path, const char* out_path);

// Verifies a persistent semantic state artifact against current tracked file hashes.
// If out_path is NULL, text is written to stdout.
bool verify_project_state(const char* state_path, const char* out_path);

// Refreshes a persistent semantic state artifact in place if tracked files changed.
// If out_path is NULL, text is written to stdout.
bool refresh_project_state(const char* state_path, const char* out_path);

// Emits a persistent semantic state artifact for the entry script.
// If focus_symbol is not NULL, the artifact stores a focused slice and optional impact callers.
// If out_path is NULL, text is written to stdout.
bool emit_project_state(const char* entry_file, const char* out_path,
                        const char* focus_symbol, bool include_impact);

// Emits a semantic diff between two entry scripts for a focused symbol.
// If include_impact is true, reverse-caller deltas are included too.
// If out_path is NULL, text is written to stdout.
bool emit_semantic_diff(const char* before_entry, const char* after_entry, const char* out_path,
                        const char* focus_symbol, bool include_impact);

// Emits a compact LLM-oriented context pack derived from the semantic index.
// If focus_symbol is not NULL, the output is narrowed to that symbol's semantic slice.
// If include_impact is true, reverse callers for the focus symbol are emitted too.
// If out_path is NULL, text is written to stdout.
bool emit_context_pack(const char* entry_file, const char* out_path,
                       const char* focus_symbol, bool include_impact);

#endif // VIPER_INDEXER_H
