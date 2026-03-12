#ifndef VIPER_COMPILER_H
#define VIPER_COMPILER_H

#include "ast.h"
#include "native.h"

// Sets the entry script path for deterministic module resolution.
void compiler_set_entry_file(const char* path);

// Returns compiler path context derived from the entry script.
const char* compiler_get_entry_file(void);
const char* compiler_get_entry_dir(void);
const char* compiler_get_project_root(void);

// Enables/disables @contract stdout emission during compile().
void compiler_set_contract_output(bool enabled);

// Controls whether top-level compile output ends with HALT (true) or RETURN (false).
void compiler_set_emit_halt(bool enabled);

// Returns the compiled top-level code as an ObjFunction (implicit "__main__")
ObjFunction* compile(AstNode* ast);

// Compiles a file by path, optionally returning the source string buffer
ObjFunction* compile_file(const char* path, char** out_source);

#endif // VIPER_COMPILER_H
