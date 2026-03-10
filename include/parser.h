#ifndef VIPER_PARSER_H
#define VIPER_PARSER_H

#include "lexer.h"
#include "ast.h"

AstNode* parse(const char* source);
bool parser_had_error();

#endif // VIPER_PARSER_H
