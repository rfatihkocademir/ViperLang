#ifndef VIPER_PARSER_H
#define VIPER_PARSER_H

#include "lexer.h"
#include "ast.h"

AstNode* parse(const char* source);

#endif // VIPER_PARSER_H
