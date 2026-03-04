#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parser.h"
#include "compiler.h"
#include "vm.h"

static char* read_file(const char* path) {
    FILE* file = fopen(path, "rb");
    if (file == NULL) { fprintf(stderr, "Could not open file \"%s\".\n", path); exit(74); }
    fseek(file, 0L, SEEK_END);
    size_t fileSize = ftell(file);
    rewind(file);
    char* buffer = (char*)malloc(fileSize + 1);
    if (buffer == NULL) { fprintf(stderr, "Not enough memory to read \"%s\".\n", path); exit(74); }
    size_t bytesRead = fread(buffer, sizeof(char), fileSize, file);
    if (bytesRead < fileSize) { fprintf(stderr, "Could not read file \"%s\".\n", path); exit(74); }
    buffer[bytesRead] = '\0';
    fclose(file);
    return buffer;
}

static void run_file(const char* path) {
    char* source = read_file(path);

    AstNode* ast = parse(source);
    ObjFunction* main_fn = compile(ast);

    VM vm;
    init_vm(&vm);
    interpret(&vm, main_fn);

    free(source);
}

int main(int argc, const char* argv[]) {
    if (argc == 2) {
        run_file(argv[1]);
    } else {
        printf("Usage: viper [script.vp]\n");
        exit(64);
    }
    return 0;
}
