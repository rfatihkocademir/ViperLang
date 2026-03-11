#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "bytecode.h"
#include "compiler.h"
#include "indexer.h"
#include "parser.h"
#include "pkg.h"
#include "vm.h"
#include "memory.h"
#include "scheduler.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef struct {
    int os;
    int fs;
    int web;
    int db;
    int ai;
    int cache;
    int util;
    int meta;
} BuildCaps;

typedef struct {
    ObjFunction** items;
    int count;
    int capacity;
} FnVisit;

static char* read_file(const char* path) {
    FILE* file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "Could not open file \"%s\".\n", path);
        exit(74);
    }
    fseek(file, 0L, SEEK_END);
    size_t fileSize = (size_t)ftell(file);
    rewind(file);
    char* buffer = (char*)malloc(fileSize + 1);
    if (buffer == NULL) {
        fprintf(stderr, "Not enough memory to read \"%s\".\n", path);
        exit(74);
    }
    size_t bytesRead = fread(buffer, sizeof(char), fileSize, file);
    if (bytesRead < fileSize) {
        fprintf(stderr, "Could not read file \"%s\".\n", path);
        exit(74);
    }
    buffer[bytesRead] = '\0';
    fclose(file);
    return buffer;
}

static ObjFunction* compile_file(const char* path, char** out_source) {
    char* source = read_file(path);

    compiler_set_entry_file(path);
    AstNode* ast = parse(source);

    init_native_core();
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

    VM* vm = malloc(sizeof(VM));
    init_vm(vm);
    
    // Setup initial frame and yield immediately
    interpret(vm, main_fn, 0); 
    
    schedule_fiber(vm);
    run_scheduler_loop();

    free(source);
}

static void run_bytecode_file(const char* path) {
    init_native_core();
    ObjFunction* main_fn = read_bytecode_file(path);
    if (!main_fn) {
        fprintf(stderr, "Could not load bytecode file \"%s\".\n", path);
        exit(1);
    }

    VM* vm = malloc(sizeof(VM));
    init_vm(vm);
    interpret(vm, main_fn, 0);
    schedule_fiber(vm);
    run_scheduler_loop();
}

static bool file_exists(const char* path) {
    return access(path, F_OK) == 0;
}

static bool ensure_dir_recursive(const char* path) {
    if (!path || path[0] == '\0') return false;
    char tmp[PATH_MAX];
    if (strlen(path) >= sizeof(tmp)) return false;
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len == 0) return false;
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';

    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return false;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return false;
    return true;
}

static bool join_path(const char* a, const char* b, char* out, size_t out_size) {
    if (!a || !b || !out || out_size == 0) return false;
    size_t la = strlen(a);
    size_t lb = strlen(b);
    bool has_sep = (la > 0 && a[la - 1] == '/');
    size_t need = la + (has_sep ? 0 : 1) + lb + 1;
    if (need > out_size) return false;
    memcpy(out, a, la);
    size_t pos = la;
    if (!has_sep) out[pos++] = '/';
    memcpy(out + pos, b, lb);
    out[pos + lb] = '\0';
    return true;
}

static void basename_no_ext(const char* path, char* out, size_t out_size) {
    if (!path || !out || out_size == 0) return;
    const char* slash = strrchr(path, '/');
    const char* base = slash ? slash + 1 : path;
    const char* dot = strrchr(base, '.');
    size_t n = dot ? (size_t)(dot - base) : strlen(base);
    if (n == 0) n = strlen(base);
    if (n >= out_size) n = out_size - 1;
    memcpy(out, base, n);
    out[n] = '\0';
}

static bool shell_quote_double(const char* in, char* out, size_t out_size) {
    if (!in || !out || out_size < 3) return false;
    size_t j = 0;
    out[j++] = '"';
    for (size_t i = 0; in[i] != '\0'; i++) {
        char c = in[i];
        if (c == '"' || c == '\\' || c == '$' || c == '`') {
            if (j + 2 >= out_size) return false;
            out[j++] = '\\';
            out[j++] = c;
        } else {
            if (j + 1 >= out_size) return false;
            out[j++] = c;
        }
    }
    if (j + 2 > out_size) return false;
    out[j++] = '"';
    out[j] = '\0';
    return true;
}

static bool fn_visit_add(FnVisit* visit, ObjFunction* fn) {
    if (!visit || !fn) return false;
    for (int i = 0; i < visit->count; i++) {
        if (visit->items[i] == fn) return false;
    }
    if (visit->count + 1 > visit->capacity) {
        int next = (visit->capacity == 0) ? 16 : visit->capacity * 2;
        ObjFunction** grown = (ObjFunction**)realloc(visit->items, sizeof(ObjFunction*) * (size_t)next);
        if (!grown) return false;
        visit->items = grown;
        visit->capacity = next;
    }
    visit->items[visit->count++] = fn;
    return true;
}

static void collect_used_natives_from_fn(ObjFunction* fn, bool* used, FnVisit* visit) {
    if (!fn || !used || !visit) return;
    if (!fn_visit_add(visit, fn)) return;

    for (int i = 0; i < fn->chunk.count; i++) {
        Instruction inst = fn->chunk.code[i];
        if (DECODE_OP(inst) == OP_CALL_NATIVE) {
            int native_idx = DECODE_A(inst);
            if (native_idx >= 0 && native_idx < 256) used[native_idx] = true;
        }
    }

    for (int i = 0; i < fn->chunk.constant_count; i++) {
        Value v = fn->chunk.constants[i];
        if (!IS_OBJ(v)) continue;
        Obj* obj = AS_OBJ(v);
        if (obj->type == OBJ_FUNCTION) {
            collect_used_natives_from_fn((ObjFunction*)obj, used, visit);
        }
    }
}

static void infer_caps_from_natives(const bool* used, BuildCaps* caps) {
    memset(caps, 0, sizeof(*caps));
    int ncount = native_count();
    for (int i = 0; i < ncount && i < 256; i++) {
        if (!used[i]) continue;
        const char* name = get_native_name(i);
        if (!name) continue;

        if (strncmp(name, "os_", 3) == 0) caps->os = 1;
        else if (strncmp(name, "fs_", 3) == 0) caps->fs = 1;
        else if (strncmp(name, "web_", 4) == 0 || strcmp(name, "serve") == 0 || strcmp(name, "fetch") == 0) caps->web = 1;
        else if (strncmp(name, "vdb_", 4) == 0 || strcmp(name, "query") == 0) caps->db = 1;
        else if (strncmp(name, "ai_", 3) == 0) caps->ai = 1;
        else if (strncmp(name, "cache_", 6) == 0) caps->cache = 1;
        else if (strncmp(name, "time_", 5) == 0 || strncmp(name, "math_", 5) == 0 ||
                 strncmp(name, "text_", 5) == 0 || strncmp(name, "arr_", 4) == 0) caps->util = 1;
        else if (strncmp(name, "meta_", 5) == 0) caps->meta = 1;
    }
}

static bool write_caps_lock(const char* path, const BuildCaps* caps, const bool* used) {
    FILE* fp = fopen(path, "w");
    if (!fp) return false;

    fprintf(fp,
            "{\n"
            "  \"caps\": {\n"
            "    \"os\": %d,\n"
            "    \"fs\": %d,\n"
            "    \"web\": %d,\n"
            "    \"db\": %d,\n"
            "    \"ai\": %d,\n"
            "    \"cache\": %d,\n"
            "    \"util\": %d,\n"
            "    \"meta\": %d\n"
            "  },\n"
            "  \"used_natives\": [",
            caps->os, caps->fs, caps->web, caps->db, caps->ai, caps->cache, caps->util, caps->meta);

    bool first = true;
    int ncount = native_count();
    for (int i = 0; i < ncount && i < 256; i++) {
        if (!used[i]) continue;
        const char* name = get_native_name(i);
        if (!name) continue;
        fprintf(fp, "%s\"%s\"", first ? "" : ", ", name);
        first = false;
    }
    fprintf(fp, "]\n}\n");

    fclose(fp);
    return true;
}

static void print_build_usage(void) {
    printf("Usage: viper build <entry.vp> [--out-dir=build] [--name=app]\n");
}

static int cmd_build_app(int argc, const char* argv[]) {
    if (argc < 1) {
        print_build_usage();
        return 64;
    }

    const char* entry = argv[0];
    const char* out_dir = "build";
    const char* app_name_opt = NULL;

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--out-dir=", 10) == 0) {
            out_dir = argv[i] + 10;
            continue;
        }
        if (strncmp(argv[i], "--name=", 7) == 0) {
            app_name_opt = argv[i] + 7;
            continue;
        }
        print_build_usage();
        return 64;
    }

    if (!file_exists(entry)) {
        fprintf(stderr, "Build Error: entry file not found: %s\n", entry);
        return 1;
    }
    if (!file_exists("Makefile")) {
        fprintf(stderr, "Build Error: Makefile not found in current directory.\n");
        fprintf(stderr, "Run this command from the Viper source root.\n");
        return 1;
    }

    compiler_set_contract_output(false);
    char* source = NULL;
    ObjFunction* main_fn = compile_file(entry, &source);
    compiler_set_contract_output(true);
    free(source);

    bool used[256];
    memset(used, 0, sizeof(used));
    FnVisit visit;
    memset(&visit, 0, sizeof(visit));
    collect_used_natives_from_fn(main_fn, used, &visit);
    free(visit.items);

    BuildCaps caps;
    infer_caps_from_natives(used, &caps);

    char app_name[128];
    if (app_name_opt && app_name_opt[0] != '\0') {
        snprintf(app_name, sizeof(app_name), "%s", app_name_opt);
    } else {
        basename_no_ext(entry, app_name, sizeof(app_name));
        if (app_name[0] == '\0') snprintf(app_name, sizeof(app_name), "app");
    }

    char app_dir[PATH_MAX];
    if (!join_path(out_dir, app_name, app_dir, sizeof(app_dir))) {
        fprintf(stderr, "Build Error: output path too long.\n");
        return 1;
    }
    if (!ensure_dir_recursive(app_dir)) {
        fprintf(stderr, "Build Error: cannot create output dir: %s\n", app_dir);
        return 1;
    }

    char vbb_path[PATH_MAX];
    char runtime_path[PATH_MAX];
    char lock_path[PATH_MAX];
    char run_path[PATH_MAX];
    if (!join_path(app_dir, "app.vbb", vbb_path, sizeof(vbb_path)) ||
        !join_path(app_dir, "viper-runtime", runtime_path, sizeof(runtime_path)) ||
        !join_path(app_dir, "capabilities.lock", lock_path, sizeof(lock_path)) ||
        !join_path(app_dir, "run.sh", run_path, sizeof(run_path))) {
        fprintf(stderr, "Build Error: output path too long.\n");
        return 1;
    }

    if (!write_bytecode_file(vbb_path, main_fn)) {
        fprintf(stderr, "Build Error: could not write bytecode: %s\n", vbb_path);
        return 1;
    }
    if (!write_caps_lock(lock_path, &caps, used)) {
        fprintf(stderr, "Build Error: could not write lock file: %s\n", lock_path);
        return 1;
    }

    char q_runtime[PATH_MAX * 2];
    if (!shell_quote_double(runtime_path, q_runtime, sizeof(q_runtime))) {
        fprintf(stderr, "Build Error: runtime output path is not shell-safe.\n");
        return 1;
    }

    char cmd[4096];
    snprintf(cmd, sizeof(cmd),
             "make -s PROFILE=full PROFILE_NAME=app-auto "
             "CAP_OS=%d CAP_FS=%d CAP_WEB=%d CAP_DB=%d CAP_AI=%d CAP_CACHE=%d CAP_UTIL=%d CAP_META=%d "
             "TARGET=%s runtime",
             caps.os, caps.fs, caps.web, caps.db, caps.ai, caps.cache, caps.util, caps.meta,
             q_runtime);
    int status = system(cmd);
    if (status != 0) {
        fprintf(stderr, "Build Error: runtime build failed.\n");
        return 1;
    }

    FILE* run_fp = fopen(run_path, "w");
    if (!run_fp) {
        fprintf(stderr, "Build Error: could not write launcher: %s\n", run_path);
        return 1;
    }
    fprintf(run_fp,
            "#!/usr/bin/env sh\n"
            "set -eu\n"
            "DIR=\"$(CDPATH= cd -- \"$(dirname -- \"$0\")\" && pwd)\"\n"
            "exec \"$DIR/viper-runtime\" --run-bytecode=\"$DIR/app.vbb\" \"$@\"\n");
    fclose(run_fp);
    chmod(run_path, 0755);

    printf("Build complete.\n");
    printf("  entry: %s\n", entry);
    printf("  output: %s\n", app_dir);
    printf("  caps: os=%d fs=%d web=%d db=%d ai=%d cache=%d util=%d meta=%d\n",
           caps.os, caps.fs, caps.web, caps.db, caps.ai, caps.cache, caps.util, caps.meta);
    printf("  run: %s\n", run_path);
    return 0;
}

static void print_usage(void) {
    printf("Usage: viper [--emit-index-json[=out.json]] [--emit-bytecode=out.vbb] [script.vp]\n");
    printf("       viper --run-bytecode=app.vbb\n");
    printf("       viper build <entry.vp> [--out-dir=build] [--name=app]\n");
    printf("       viper pkg <init|add|remove|install|lock|list|build|abi> [...args]\n");
}

int main(int argc, const char* argv[]) {
    if (argc >= 2 && strcmp(argv[1], "pkg") == 0) {
        if (argc < 3) {
            printf("Usage: viper pkg <init|add|remove|install|lock|list|build|abi> [...args]\n");
            exit(64);
        }
        return vpm_handle_cli(argc - 2, argv + 2);
    }

    init_memory();
    init_scheduler();

    if (argc >= 2 && strcmp(argv[1], "build") == 0) {
        return cmd_build_app(argc - 2, argv + 2);
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
        print_usage();
        exit(64);
    }

    if (run_bytecode_in != NULL) {
        if (script_path != NULL || emit_index || emit_bytecode_out != NULL) {
            printf("Usage: viper --run-bytecode=app.vbb\n");
            exit(64);
        }
        run_bytecode_file(run_bytecode_in);
        destroy_scheduler();
        free_memory();
        return 0;
    }

    if (script_path == NULL) {
        print_usage();
        exit(64);
    }

    if (emit_index) {
        if (!emit_semantic_index_json(script_path, index_out)) {
            exit(1);
        }
        free_memory();
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
        free_memory();
        return 0;
    }

    run_file(script_path);
    destroy_scheduler();
    free_memory();
    return 0;
}
