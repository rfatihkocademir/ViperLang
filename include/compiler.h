#ifndef VIPER_COMPILER_H
#define VIPER_COMPILER_H

#include "ast.h"
#include "native.h"

// Returns the compiled top-level code as an ObjFunction (implicit "__main__")
ObjFunction* compile(AstNode* ast);

#endif // VIPER_COMPILER_H
