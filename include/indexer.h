#ifndef VIPER_INDEXER_H
#define VIPER_INDEXER_H

#include <stdbool.h>

// Emits a machine-readable semantic index for the entry script and its modules.
// If out_path is NULL, JSON is written to stdout.
bool emit_semantic_index_json(const char* entry_file, const char* out_path);

// Emits a minimal resume packet from a persistent semantic state artifact.
// The artifact is verified against current tracked file hashes without re-indexing source.
// If out_path is NULL, text is written to stdout.
bool resume_project_state(const char* state_path, const char* out_path,
                          const char* focus_symbol, bool include_impact,
                          bool brief_output);

// Verifies a persistent semantic state artifact against current tracked file hashes.
// If out_path is NULL, text is written to stdout.
bool verify_project_state(const char* state_path, const char* out_path);

// Queries a persistent semantic state artifact without re-indexing source.
// Filters may be combined; matching symbols are emitted as a compact ledger.
// If out_path is NULL, text is written to stdout.
bool query_project_state(const char* state_path, const char* out_path,
                         const char* name_filter, const char* effect_filter,
                         const char* call_filter, bool include_impact,
                         bool include_dependencies, bool brief_output);

bool bench_project_state(const char* state_path, const char* out_path,
                         const char* focus_symbol, const char* name_filter,
                         const char* effect_filter, const char* call_filter,
                         bool include_impact, bool include_dependencies);

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

// Emits a source-free change and test plan between two persistent semantic state artifacts.
// The states should represent the same focus/impact slice.
// If out_path is NULL, text is written to stdout.
bool emit_state_plan(const char* before_state, const char* after_state, const char* out_path);

// Executes the focused test plan derived from two persistent semantic state artifacts.
// If out_path is NULL, text is written to stdout.
bool run_state_plan(const char* before_state, const char* after_state, const char* out_path);

// Emits a compact LLM-oriented context pack derived from the semantic index.
// If focus_symbol is not NULL, the output is narrowed to that symbol's semantic slice.
// If include_impact is true, reverse callers for the focus symbol are emitted too.
// If out_path is NULL, text is written to stdout.
bool emit_context_pack(const char* entry_file, const char* out_path,
                       const char* focus_symbol, bool include_impact);

#endif // VIPER_INDEXER_H
