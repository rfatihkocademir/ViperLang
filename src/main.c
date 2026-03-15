#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdarg.h>
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

static void cli_error_exit(const char* kind, const char* code, int exit_code, const char* fmt, ...) {
    va_list args;
    fprintf(stderr, "%s [%s]: ", kind, code);
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
    exit(exit_code);
}

static int cli_error_return(const char* kind, const char* code, int return_code, const char* fmt, ...) {
    va_list args;
    fprintf(stderr, "%s [%s]: ", kind, code);
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
    return return_code;
}

static char* read_file(const char* path) {
    FILE* file = fopen(path, "rb");
    if (file == NULL) {
        cli_error_exit("CLI Error", "VCL001", 74, "Could not open file \"%s\".", path);
    }
    fseek(file, 0L, SEEK_END);
    size_t fileSize = (size_t)ftell(file);
    rewind(file);
    char* buffer = (char*)malloc(fileSize + 1);
    if (buffer == NULL) {
        cli_error_exit("CLI Error", "VCL002", 74, "Not enough memory to read \"%s\".", path);
    }
    size_t bytesRead = fread(buffer, sizeof(char), fileSize, file);
    if (bytesRead < fileSize) {
        cli_error_exit("CLI Error", "VCL003", 74, "Could not read file \"%s\".", path);
    }
    buffer[bytesRead] = '\0';
    fclose(file);
    return buffer;
}

ObjFunction* compile_file(const char* path, char** out_source) {
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
        cli_error_exit("CLI Error", "VCL004", 1, "Could not load bytecode file \"%s\".", path);
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
        return cli_error_return("Build Error", "VBL001", 1, "Entry file not found: %s", entry);
    }
    if (!file_exists("Makefile")) {
        fprintf(stderr, "Build Hint [VBL003]: Run this command from the Viper source root.\n");
        return cli_error_return("Build Error", "VBL002", 1,
                                "Makefile not found in current directory.");
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
        return cli_error_return("Build Error", "VBL004", 1, "Output path too long.");
    }
    if (!ensure_dir_recursive(app_dir)) {
        return cli_error_return("Build Error", "VBL005", 1,
                                "Cannot create output dir: %s", app_dir);
    }

    char vbb_path[PATH_MAX];
    char runtime_path[PATH_MAX];
    char lock_path[PATH_MAX];
    char run_path[PATH_MAX];
    if (!join_path(app_dir, "app.vbb", vbb_path, sizeof(vbb_path)) ||
        !join_path(app_dir, "viper-runtime", runtime_path, sizeof(runtime_path)) ||
        !join_path(app_dir, "capabilities.lock", lock_path, sizeof(lock_path)) ||
        !join_path(app_dir, "run.sh", run_path, sizeof(run_path))) {
        return cli_error_return("Build Error", "VBL004", 1, "Output path too long.");
    }

    if (!write_bytecode_file(vbb_path, main_fn)) {
        return cli_error_return("Build Error", "VBL006", 1,
                                "Could not write bytecode: %s", vbb_path);
    }
    if (!write_caps_lock(lock_path, &caps, used)) {
        return cli_error_return("Build Error", "VBL007", 1,
                                "Could not write lock file: %s", lock_path);
    }

    char q_runtime[PATH_MAX * 2];
    if (!shell_quote_double(runtime_path, q_runtime, sizeof(q_runtime))) {
        return cli_error_return("Build Error", "VBL008", 1,
                                "Runtime output path is not shell-safe.");
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
        return cli_error_return("Build Error", "VBL009", 1, "Runtime build failed.");
    }

    FILE* run_fp = fopen(run_path, "w");
    if (!run_fp) {
        return cli_error_return("Build Error", "VBL010", 1,
                                "Could not write launcher: %s", run_path);
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
    printf("Usage: viper [--emit-index-json[=out.json]] [--emit-project-state[=out.vstate] [--focus=symbol] [--impact]] [--resume-project-state=state.vstate [--focus=symbol] [--impact] [--brief]] [--query-project-state=state.vstate (--query-name=symbol|--query-effect=effect|--query-call=callee) [--impact] [--query-deps] [--brief]] [--bench-project-state=state.vstate [--focus=symbol] [--impact] [--query-name=symbol|--query-effect=effect|--query-call=callee] [--query-deps]] [--verify-project-state=state.vstate] [--refresh-project-state=state.vstate] [--emit-state-plan=before.vstate after.vstate] [--run-state-plan=before.vstate after.vstate] [--emit-context-pack[=out.ctx] [--focus=symbol] [--impact]] [--emit-semantic-diff=before.vp --focus=symbol [--impact]] [--emit-bytecode=out.vbb] [script.vp]\n");
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
    bool emit_project_state_flag = false;
    bool emit_context = false;
    const char* index_out = NULL;
    const char* project_state_out = NULL;
    const char* resume_project_state_in = NULL;
    const char* query_project_state_in = NULL;
    const char* bench_project_state_in = NULL;
    const char* verify_project_state_in = NULL;
    const char* refresh_project_state_in = NULL;
    const char* context_out = NULL;
    const char* semantic_diff_before = NULL;
    const char* state_plan_before = NULL;
    const char* run_state_plan_before = NULL;
    const char* focus_symbol = NULL;
    const char* query_name = NULL;
    const char* query_effect = NULL;
    const char* query_call = NULL;
    bool query_dependencies = false;
    bool include_impact = false;
    bool brief_output = false;
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
        if (strcmp(argv[i], "--emit-project-state") == 0) {
            emit_project_state_flag = true;
            continue;
        }
        if (strncmp(argv[i], "--emit-project-state=", 21) == 0) {
            emit_project_state_flag = true;
            project_state_out = argv[i] + 21;
            continue;
        }
        if (strncmp(argv[i], "--resume-project-state=", 23) == 0) {
            resume_project_state_in = argv[i] + 23;
            continue;
        }
        if (strncmp(argv[i], "--query-project-state=", 22) == 0) {
            query_project_state_in = argv[i] + 22;
            continue;
        }
        if (strncmp(argv[i], "--bench-project-state=", 22) == 0) {
            bench_project_state_in = argv[i] + 22;
            continue;
        }
        if (strncmp(argv[i], "--verify-project-state=", 23) == 0) {
            verify_project_state_in = argv[i] + 23;
            continue;
        }
        if (strncmp(argv[i], "--refresh-project-state=", 24) == 0) {
            refresh_project_state_in = argv[i] + 24;
            continue;
        }
        if (strcmp(argv[i], "--emit-context-pack") == 0) {
            emit_context = true;
            continue;
        }
        if (strncmp(argv[i], "--emit-context-pack=", 20) == 0) {
            emit_context = true;
            context_out = argv[i] + 20;
            continue;
        }
        if (strncmp(argv[i], "--focus=", 8) == 0) {
            focus_symbol = argv[i] + 8;
            continue;
        }
        if (strncmp(argv[i], "--query-name=", 13) == 0) {
            query_name = argv[i] + 13;
            continue;
        }
        if (strncmp(argv[i], "--query-effect=", 15) == 0) {
            query_effect = argv[i] + 15;
            continue;
        }
        if (strncmp(argv[i], "--query-call=", 13) == 0) {
            query_call = argv[i] + 13;
            continue;
        }
        if (strcmp(argv[i], "--query-deps") == 0) {
            query_dependencies = true;
            continue;
        }
        if (strcmp(argv[i], "--brief") == 0) {
            brief_output = true;
            continue;
        }
        if (strncmp(argv[i], "--emit-semantic-diff=", 21) == 0) {
            semantic_diff_before = argv[i] + 21;
            continue;
        }
        if (strncmp(argv[i], "--emit-state-plan=", 18) == 0) {
            state_plan_before = argv[i] + 18;
            continue;
        }
        if (strncmp(argv[i], "--run-state-plan=", 17) == 0) {
            run_state_plan_before = argv[i] + 17;
            continue;
        }
        if (strcmp(argv[i], "--impact") == 0) {
            include_impact = true;
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
        if (script_path != NULL || emit_index || emit_project_state_flag || emit_context || emit_bytecode_out != NULL ||
            semantic_diff_before != NULL || resume_project_state_in != NULL || query_project_state_in != NULL ||
            bench_project_state_in != NULL ||
            verify_project_state_in != NULL || refresh_project_state_in != NULL || state_plan_before != NULL ||
            run_state_plan_before != NULL ||
            focus_symbol != NULL ||
            query_name != NULL || query_effect != NULL || query_call != NULL || query_dependencies ||
            include_impact || brief_output) {
            printf("Usage: viper --run-bytecode=app.vbb\n");
            exit(64);
        }
        run_bytecode_file(run_bytecode_in);
        destroy_scheduler();
        free_memory();
        return 0;
    }

    if (resume_project_state_in != NULL || query_project_state_in != NULL ||
        bench_project_state_in != NULL ||
        verify_project_state_in != NULL || refresh_project_state_in != NULL) {
        if (script_path != NULL || emit_index || emit_project_state_flag || emit_context ||
            emit_bytecode_out != NULL || semantic_diff_before != NULL || state_plan_before != NULL ||
            run_state_plan_before != NULL) {
            print_usage();
            exit(64);
        }

        if (resume_project_state_in != NULL &&
            (query_project_state_in != NULL || bench_project_state_in != NULL || verify_project_state_in != NULL ||
             refresh_project_state_in != NULL || query_name != NULL ||
             query_effect != NULL || query_call != NULL || query_dependencies)) {
            print_usage();
            exit(64);
        }

        if (query_project_state_in != NULL &&
            (resume_project_state_in != NULL || bench_project_state_in != NULL || verify_project_state_in != NULL ||
             refresh_project_state_in != NULL || focus_symbol != NULL)) {
            print_usage();
            exit(64);
        }

        if (bench_project_state_in != NULL &&
            (resume_project_state_in != NULL || query_project_state_in != NULL ||
             verify_project_state_in != NULL || refresh_project_state_in != NULL)) {
            print_usage();
            exit(64);
        }

        if (query_project_state_in != NULL &&
            query_name == NULL && query_effect == NULL && query_call == NULL) {
            print_usage();
            exit(64);
        }
        if (query_project_state_in == NULL && bench_project_state_in == NULL &&
            (query_name != NULL || query_effect != NULL || query_call != NULL || query_dependencies)) {
            print_usage();
            exit(64);
        }
        if (bench_project_state_in != NULL &&
            (query_name != NULL || query_effect != NULL || query_call != NULL) &&
            focus_symbol != NULL) {
            print_usage();
            exit(64);
        }
        if (bench_project_state_in != NULL &&
            query_name == NULL && query_effect == NULL && query_call == NULL && query_dependencies) {
            print_usage();
            exit(64);
        }

        if ((verify_project_state_in != NULL || refresh_project_state_in != NULL) &&
            (focus_symbol != NULL || include_impact || query_name != NULL ||
             query_effect != NULL || query_call != NULL || query_dependencies || brief_output)) {
            print_usage();
            exit(64);
        }
        if (brief_output && resume_project_state_in == NULL && query_project_state_in == NULL) {
            print_usage();
            exit(64);
        }
        if (bench_project_state_in != NULL && brief_output) {
            print_usage();
            exit(64);
        }

        bool ok = true;
        if (resume_project_state_in != NULL) {
            ok = resume_project_state(resume_project_state_in, NULL, focus_symbol, include_impact,
                                      brief_output);
        } else if (query_project_state_in != NULL) {
            ok = query_project_state(query_project_state_in, NULL, query_name, query_effect,
                                     query_call, include_impact, query_dependencies, brief_output);
        } else if (bench_project_state_in != NULL) {
            ok = bench_project_state(bench_project_state_in, NULL, focus_symbol, query_name,
                                     query_effect, query_call, include_impact, query_dependencies);
        } else if (verify_project_state_in != NULL) {
            ok = verify_project_state(verify_project_state_in, NULL);
        } else if (refresh_project_state_in != NULL) {
            ok = refresh_project_state(refresh_project_state_in, NULL);
        }
        if (!ok) exit(1);
        free_memory();
        return 0;
    }

    if (state_plan_before != NULL) {
        if (script_path == NULL || emit_index || emit_project_state_flag || emit_context ||
            emit_bytecode_out != NULL || semantic_diff_before != NULL || run_state_plan_before != NULL ||
            focus_symbol != NULL ||
            include_impact || query_name != NULL || query_effect != NULL || query_call != NULL ||
            brief_output ||
            query_dependencies) {
            print_usage();
            exit(64);
        }
        if (!emit_state_plan(state_plan_before, script_path, NULL)) {
            exit(1);
        }
        free_memory();
        return 0;
    }

    if (run_state_plan_before != NULL) {
        if (script_path == NULL || emit_index || emit_project_state_flag || emit_context ||
            emit_bytecode_out != NULL || semantic_diff_before != NULL || state_plan_before != NULL ||
            focus_symbol != NULL || include_impact || query_name != NULL || query_effect != NULL ||
            brief_output ||
            query_call != NULL || query_dependencies) {
            print_usage();
            exit(64);
        }
        if (!run_state_plan(run_state_plan_before, script_path, NULL)) {
            exit(1);
        }
        free_memory();
        return 0;
    }

    if (script_path == NULL) {
        print_usage();
        exit(64);
    }

    if (semantic_diff_before != NULL && (emit_index || emit_project_state_flag || emit_context || emit_bytecode_out != NULL || brief_output)) {
        print_usage();
        exit(64);
    }

    if (focus_symbol != NULL && !emit_context && !emit_project_state_flag) {
        if (semantic_diff_before == NULL) {
            print_usage();
            exit(64);
        }
    }

    if (semantic_diff_before != NULL && focus_symbol == NULL) {
        print_usage();
        exit(64);
    }

    if (include_impact && (!emit_context && !emit_project_state_flag && semantic_diff_before == NULL)) {
        print_usage();
        exit(64);
    }

    if (include_impact && semantic_diff_before != NULL && focus_symbol == NULL) {
        print_usage();
        exit(64);
    }
    if (brief_output) {
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

    if (emit_project_state_flag) {
        if (!emit_project_state(script_path, project_state_out, focus_symbol, include_impact)) {
            exit(1);
        }
        free_memory();
        return 0;
    }

    if (emit_context) {
        if (!emit_context_pack(script_path, context_out, focus_symbol, include_impact)) {
            exit(1);
        }
        free_memory();
        return 0;
    }

    if (semantic_diff_before != NULL) {
        if (!emit_semantic_diff(semantic_diff_before, script_path, NULL, focus_symbol, include_impact)) {
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
            cli_error_exit("CLI Error", "VCL005", 1, "Could not write bytecode file \"%s\".", emit_bytecode_out);
        }
        free_memory();
        return 0;
    }

    run_file(script_path);
    destroy_scheduler();
    free_memory();
    return 0;
}
