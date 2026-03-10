#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parser.h"
#include "compiler.h"
#include "bytecode.h"
#include "indexer.h"
#include "pkg.h"
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

static ObjFunction* compile_file(const char* path, char** out_source) {
    char* source = read_file(path);

    compiler_set_entry_file(path);
    AstNode* ast = parse(source);

    init_native_core(); // Call init_native_core() before compiling/interpreting.
    if (parser_had_error()) {
        if (out_source) free(source);
        exit(65);
    }
    ObjFunction* main_fn = compile(ast);

    if (out_source) {
        *out_source = source;
    } else {
        free(source);
    }
    return main_fn;
}

static void run_file(const char* path) {
    char* source = NULL;
    ObjFunction* main_fn = compile_file(path, &source);

    VM vm;
    init_vm(&vm);
    interpret(&vm, main_fn);

    free(source);
}

static void run_bytecode_file(const char* path) {
    init_native_core();
    ObjFunction* main_fn = read_bytecode_file(path);
    if (!main_fn) {
        fprintf(stderr, "Could not load bytecode file \"%s\".\n", path);
        exit(1);
    }

    VM vm;
    init_vm(&vm);
    interpret(&vm, main_fn);
}

int main(int argc, const char* argv[]) {
    if (argc >= 2 && strcmp(argv[1], "pkg") == 0) {
        if (argc < 3) {
            printf("Usage: viper pkg <init|add|remove|install|lock|list|build|abi> [...args]\n");
            exit(64);
        }
        int code = vpm_handle_cli(argc - 2, argv + 2);
        return code;
    }

    bool emit_index = false;
    const char* index_out = NULL;
    const char* emit_bytecode_out = NULL;
    const char* run_bytecode_in = NULL;
    const char* script_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--emit-index-json") == 0) {
            emit_index = true;
            continue;
        }
        if (strncmp(argv[i], "--emit-index-json=", 18) == 0) {
            emit_index = true;
            index_out = argv[i] + 18;
            continue;
        }
        if (strncmp(argv[i], "--emit-bytecode=", 16) == 0) {
            emit_bytecode_out = argv[i] + 16;
            continue;
        }
        if (strncmp(argv[i], "--run-bytecode=", 15) == 0) {
            run_bytecode_in = argv[i] + 15;
            continue;
        }
        if (script_path == NULL) {
            script_path = argv[i];
            continue;
        }
        printf("Usage: viper [--emit-index-json[=out.json]] [--emit-bytecode=out.vbb] [script.vp]\n");
        printf("       viper --run-bytecode=app.vbb\n");
        printf("       viper pkg <init|add|remove|install|lock|list|build|abi> [...args]\n");
        exit(64);
    }

    if (run_bytecode_in != NULL) {
        if (script_path != NULL || emit_index || emit_bytecode_out != NULL) {
            printf("Usage: viper --run-bytecode=app.vbb\n");
            exit(64);
        }
        run_bytecode_file(run_bytecode_in);
        return 0;
    }

    if (script_path == NULL) {
        printf("Usage: viper [--emit-index-json[=out.json]] [--emit-bytecode=out.vbb] [script.vp]\n");
        printf("       viper --run-bytecode=app.vbb\n");
        printf("       viper pkg <init|add|remove|install|lock|list|build|abi> [...args]\n");
        exit(64);
    }

    if (emit_index) {
        if (!emit_semantic_index_json(script_path, index_out)) {
            exit(1);
        }
        return 0;
    }

    if (emit_bytecode_out != NULL) {
        char* source = NULL;
        ObjFunction* main_fn = compile_file(script_path, &source);
        bool ok = write_bytecode_file(emit_bytecode_out, main_fn);
        free(source);
        if (!ok) {
            fprintf(stderr, "Could not write bytecode file \"%s\".\n", emit_bytecode_out);
            exit(1);
        }
        return 0;
    }

    run_file(script_path);
    return 0;
}
