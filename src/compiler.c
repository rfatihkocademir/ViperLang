#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include "parser.h"
#include "compiler.h"
#include "bytecode.h"
#include "vm.h"
#include "native.h"

// ---- Local Variable Table -----------------------------------------------

typedef struct {
    const char* name;
    int length;
    int reg;
    int depth;
} Local;

#define MAX_PATH_LEN 1024
#define MAX_ABI_NAME_LEN 128
#define MAX_NAMESPACE_ALIAS_LEN 128
#define ABI_KIND_FUNCTION 1
#define ABI_KIND_STRUCT 2

static char entry_file_path[MAX_PATH_LEN];
static char project_root_path[MAX_PATH_LEN];
static char** module_dir_stack = NULL;
static int module_dir_depth = 0;
static int module_dir_cap = 0;
static char** imported_modules = NULL;
static int imported_module_count = 0;
static int imported_module_cap = 0;
static bool contract_output_enabled = true;
static bool emit_halt_enabled = true;

static void copy_path(char* dst, size_t dst_size, const char* src) {
    if (dst_size == 0) return;
    snprintf(dst, dst_size, "%s", src ? src : "");
}

static void compiler_fatal_oom(void) {
    printf("Compiler Error: Out of memory.\n");
    exit(1);
}

static bool ensure_capacity(void** ptr, int* cap, int needed, size_t elem_size) {
    if (*cap >= needed) return true;
    int next = (*cap == 0) ? 8 : *cap;
    while (next < needed) next *= 2;
    void* grown = realloc(*ptr, (size_t)next * elem_size);
    if (!grown) return false;
    *ptr = grown;
    *cap = next;
    return true;
}

static char* dup_text(const char* src) {
    if (!src) return NULL;
    size_t n = strlen(src);
    char* out = (char*)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, src, n + 1);
    return out;
}

static bool is_absolute_path(const char* path) {
    if (!path || path[0] == '\0') return false;
    if (path[0] == '/') return true;
    if (isalpha((unsigned char)path[0]) && path[1] == ':' &&
        (path[2] == '/' || path[2] == '\\')) return true;
    return false;
}

static bool file_exists(const char* path) {
    return access(path, F_OK) == 0;
}

static bool has_suffix(const char* text, const char* suffix) {
    if (!text || !suffix) return false;
    size_t n = strlen(text);
    size_t m = strlen(suffix);
    if (m > n) return false;
    return strcmp(text + (n - m), suffix) == 0;
}

static bool build_vbc_path(const char* module_path, char* out, size_t out_size) {
    if (!module_path || !out || out_size == 0) return false;
    if (!has_suffix(module_path, ".vp")) return false;

    size_t n = strlen(module_path);
    if (n + 2 > out_size) return false; // replace last 'p' with "bc"

    memcpy(out, module_path, n - 2);
    out[n - 2] = 'v';
    out[n - 1] = 'b';
    out[n] = 'c';
    out[n + 1] = '\0';
    return true;
}

static bool build_vbb_path(const char* module_path, char* out, size_t out_size) {
    if (!module_path || !out || out_size == 0) return false;
    if (!has_suffix(module_path, ".vp")) return false;

    size_t n = strlen(module_path);
    if (n + 2 > out_size) return false; // replace last 'p' with "bb"

    memcpy(out, module_path, n - 2);
    out[n - 2] = 'v';
    out[n - 1] = 'b';
    out[n] = 'b';
    out[n + 1] = '\0';
    return true;
}

static bool build_vabi_path(const char* module_path, char* out, size_t out_size) {
    if (!module_path || !out || out_size == 0) return false;
    if (!has_suffix(module_path, ".vp")) return false;

    size_t n = strlen(module_path);
    if (n + 3 > out_size) return false;

    memcpy(out, module_path, n - 2);
    out[n - 2] = 'v';
    out[n - 1] = 'a';
    out[n] = 'b';
    out[n + 1] = 'i';
    out[n + 2] = '\0';
    return true;
}

static bool read_text_file(const char* path, char** out_source) {
    *out_source = NULL;
    FILE* file = fopen(path, "rb");
    if (!file) return false;

    if (fseek(file, 0L, SEEK_END) != 0) {
        fclose(file);
        return false;
    }
    long size = ftell(file);
    if (size < 0) {
        fclose(file);
        return false;
    }
    rewind(file);

    char* buffer = (char*)malloc((size_t)size + 1);
    if (!buffer) {
        fclose(file);
        return false;
    }

    size_t read_n = fread(buffer, 1, (size_t)size, file);
    fclose(file);
    if (read_n < (size_t)size) {
        free(buffer);
        return false;
    }

    buffer[read_n] = '\0';
    *out_source = buffer;
    return true;
}

static bool module_source_exists(const char* module_path) {
    if (file_exists(module_path)) return true;
    char vbc_path[MAX_PATH_LEN];
    if (build_vbc_path(module_path, vbc_path, sizeof(vbc_path)) && file_exists(vbc_path)) {
        return true;
    }
    char vbb_path[MAX_PATH_LEN];
    if (build_vbb_path(module_path, vbb_path, sizeof(vbb_path)) && file_exists(vbb_path)) {
        return true;
    }
    return false;
}

static bool load_module_source(const char* module_path, char** out_source) {
    *out_source = NULL;

    char vbc_path[MAX_PATH_LEN];
    if (build_vbc_path(module_path, vbc_path, sizeof(vbc_path)) && file_exists(vbc_path)) {
        char* cached = read_source_cache_file(vbc_path);
        if (cached) {
            *out_source = cached;
            return true;
        }
    }

    if (file_exists(module_path)) {
        if (read_text_file(module_path, out_source)) return true;
    }

    // Fallback order: if source was missing or unreadable, try vbc once more for resilience.
    if (build_vbc_path(module_path, vbc_path, sizeof(vbc_path)) && file_exists(vbc_path)) {
        char* cached = read_source_cache_file(vbc_path);
        if (!cached) return false;
        *out_source = cached;
        return true;
    }
    return false;
}

static bool read_first_line(const char* path, char* out, size_t out_size) {
    if (out_size == 0) return false;
    FILE* file = fopen(path, "r");
    if (!file) return false;
    if (fgets(out, (int)out_size, file) == NULL) {
        fclose(file);
        return false;
    }
    fclose(file);

    size_t n = strlen(out);
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r')) {
        out[n - 1] = '\0';
        n--;
    }
    return n > 0;
}

static void path_dirname(const char* path, char* out, size_t out_size) {
    if (!path || path[0] == '\0') {
        copy_path(out, out_size, ".");
        return;
    }
    const char* slash = strrchr(path, '/');
    if (!slash) {
        copy_path(out, out_size, ".");
        return;
    }
    if (slash == path) {
        copy_path(out, out_size, "/");
        return;
    }
    size_t len = (size_t)(slash - path);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, path, len);
    out[len] = '\0';
}

static bool join_path2(const char* a, const char* b, char* out, size_t out_size) {
    if (!a || !b) return false;
    size_t la = strlen(a);
    size_t lb = strlen(b);
    if (la + 1 + lb + 1 > out_size) return false;
    memcpy(out, a, la);
    out[la] = '/';
    memcpy(out + la + 1, b, lb);
    out[la + 1 + lb] = '\0';
    return true;
}

static bool is_package_import(const char* raw_path) {
    return raw_path && raw_path[0] == '@';
}

static void detect_project_root(void) {
    char current[MAX_PATH_LEN];
    if (entry_file_path[0] != '\0') {
        path_dirname(entry_file_path, current, sizeof(current));
    } else {
        copy_path(current, sizeof(current), ".");
    }

    while (1) {
        char manifest[MAX_PATH_LEN];
        if (!join_path2(current, "viper.vpmod", manifest, sizeof(manifest))) break;
        if (file_exists(manifest)) {
            copy_path(project_root_path, sizeof(project_root_path), current);
            return;
        }

        if (strcmp(current, "/") == 0 || strcmp(current, ".") == 0) break;

        char parent[MAX_PATH_LEN];
        path_dirname(current, parent, sizeof(parent));
        if (strcmp(parent, current) == 0) break;
        copy_path(current, sizeof(current), parent);
    }

    copy_path(project_root_path, sizeof(project_root_path), ".");
}

static void resolve_package_path(const char* raw_path, char* out, size_t out_size) {
    if (strstr(raw_path, "..") != NULL) {
        printf("Compiler Error: Package import cannot contain '..': %s\n", raw_path);
        exit(1);
    }

    // Special case for standard library.
    // e.g., "@std/io" -> "<project_root>/lib/std/io.vp"
    if (strncmp(raw_path, "@std/", 5) == 0) {
        char std_path[MAX_PATH_LEN];
        if (!join_path2(project_root_path, "lib/std", std_path, sizeof(std_path))) {
            printf("Compiler Error: Package path is too long: %s\n", raw_path);
            exit(1);
        }
        
        const char* subpath = raw_path + 5;
        char filename[MAX_PATH_LEN];
        snprintf(filename, sizeof(filename), "%s.vp", subpath);
        
        if (!join_path2(std_path, filename, out, out_size)) {
            printf("Compiler Error: Package path is too long: %s\n", raw_path);
            exit(1);
        }
        
        if (!module_source_exists(out)) {
            printf("Compiler Error: Standard library module not found: %s\n", out);
            exit(1);
        }
        return;
    }

    const char* slash = strchr(raw_path, '/');
    char package_name[MAX_PATH_LEN];
    const char* subpath = "index.vp";

    if (slash == NULL) {
        copy_path(package_name, sizeof(package_name), raw_path);
    } else {
        size_t pkg_len = (size_t)(slash - raw_path);
        if (pkg_len == 0 || pkg_len >= sizeof(package_name)) {
            printf("Compiler Error: Invalid package import: %s\n", raw_path);
            exit(1);
        }
        memcpy(package_name, raw_path, pkg_len);
        package_name[pkg_len] = '\0';
        if (slash[1] != '\0') subpath = slash + 1;
    }

    char path1[MAX_PATH_LEN];
    char package_root[MAX_PATH_LEN];
    if (!join_path2(project_root_path, ".viper/packages", path1, sizeof(path1)) ||
        !join_path2(path1, package_name, package_root, sizeof(package_root))) {
        printf("Compiler Error: Package path is too long: %s\n", raw_path);
        exit(1);
    }

    // Prefer version-pinned install if `.viper/packages/<pkg>/current` exists.
    char current_file[MAX_PATH_LEN];
    char version[MAX_PATH_LEN];
    if (join_path2(package_root, "current", current_file, sizeof(current_file)) &&
        read_first_line(current_file, version, sizeof(version))) {
        char versions_dir[MAX_PATH_LEN];
        char version_dir[MAX_PATH_LEN];
        char candidate[MAX_PATH_LEN];
        if (join_path2(package_root, "versions", versions_dir, sizeof(versions_dir)) &&
            join_path2(versions_dir, version, version_dir, sizeof(version_dir)) &&
            join_path2(version_dir, subpath, candidate, sizeof(candidate)) &&
            module_source_exists(candidate)) {
            copy_path(out, out_size, candidate);
            return;
        }
    }

    // Backward-compatible fallback: `.viper/packages/<pkg>/<subpath>`
    if (!join_path2(package_root, subpath, out, out_size)) {
        printf("Compiler Error: Package path is too long: %s\n", raw_path);
        exit(1);
    }

    if (!module_source_exists(out)) {
        printf("Compiler Error: Package module not found for '%s' (resolved: %s). Run 'viper pkg install'.\n",
               raw_path, out);
        exit(1);
    }
}

static void push_module_dir(const char* dir) {
    if (!ensure_capacity((void**)&module_dir_stack, &module_dir_cap,
                         module_dir_depth + 1, sizeof(char*))) {
        compiler_fatal_oom();
    }

    char* dup = dup_text(dir ? dir : ".");
    if (!dup) compiler_fatal_oom();
    module_dir_stack[module_dir_depth++] = dup;
}

static void pop_module_dir(void) {
    if (module_dir_depth <= 0) return;
    module_dir_depth--;
    free(module_dir_stack[module_dir_depth]);
    module_dir_stack[module_dir_depth] = NULL;
}

static const char* current_module_dir(void) {
    if (module_dir_depth <= 0) return ".";
    return module_dir_stack[module_dir_depth - 1];
}

static bool module_already_imported(const char* resolved_path) {
    for (int i = 0; i < imported_module_count; i++) {
        if (strcmp(imported_modules[i], resolved_path) == 0) return true;
    }
    return false;
}

static void remember_imported_module(const char* resolved_path) {
    if (!ensure_capacity((void**)&imported_modules, &imported_module_cap,
                         imported_module_count + 1, sizeof(char*))) {
        compiler_fatal_oom();
    }

    char* dup = dup_text(resolved_path);
    if (!dup) compiler_fatal_oom();
    imported_modules[imported_module_count++] = dup;
}

static void clear_module_dirs(void) {
    while (module_dir_depth > 0) pop_module_dir();
}

static void clear_imported_modules(void) {
    for (int i = 0; i < imported_module_count; i++) {
        free(imported_modules[i]);
    }
    imported_module_count = 0;
}

static void decode_use_path(Token token, char* out, size_t out_size) {
    const char* start = token.start;
    int length = token.length;
    if (length >= 2 && start[0] == '"' && start[length - 1] == '"') {
        start += 1;
        length -= 2;
    }
    if (length < 0) length = 0;
    if ((size_t)length >= out_size) {
        printf("Compiler Error: Module path is too long.\n");
        exit(1);
    }
    memcpy(out, start, (size_t)length);
    out[length] = '\0';
}

static void resolve_module_path(const char* raw_path, char* out, size_t out_size) {
    if (is_package_import(raw_path)) {
        resolve_package_path(raw_path, out, out_size);
        return;
    }
    if (is_absolute_path(raw_path)) {
        copy_path(out, out_size, raw_path);
    } else {
        const char* base = current_module_dir();
        size_t base_len = strlen(base);
        size_t raw_len = strlen(raw_path);
        if (base_len + 1 + raw_len + 1 > out_size) {
            printf("Compiler Error: Resolved module path is too long.\n");
            exit(1);
        }
        memcpy(out, base, base_len);
        out[base_len] = '/';
        memcpy(out + base_len + 1, raw_path, raw_len);
        out[base_len + 1 + raw_len] = '\0';
    }
}

void compiler_set_entry_file(const char* path) {
    copy_path(entry_file_path, sizeof(entry_file_path), path ? path : "");
}

void compiler_set_contract_output(bool enabled) {
    contract_output_enabled = enabled;
}

void compiler_set_emit_halt(bool enabled) {
    emit_halt_enabled = enabled;
}

// Global registries
static ObjFunction** fn_registry = NULL;
static int fn_count = 0;
static int fn_cap = 0;

static ObjStruct** st_registry = NULL;
static int st_count = 0;
static int st_cap = 0;

typedef struct {
    char alias[MAX_NAMESPACE_ALIAS_LEN];
    int kind;
    const char* name;
    int name_len;
    Obj* obj;
} NamespaceEntry;

static NamespaceEntry* ns_registry = NULL;
static int ns_count = 0;
static int ns_cap = 0;

typedef struct {
    const char* name;
    int length;
} GlobalVar;

static GlobalVar* global_vars = NULL;
static int global_var_count = 0;
static int global_var_cap = 0;

typedef struct {
    Local* locals;
    int local_count;
    int local_cap;
    int next_reg;
    int scope_depth;
    ObjFunction* current_fn; // The function we are currently compiling into
} CompilerState;

static CompilerState cst;

static bool ensure_local_capacity(CompilerState* state, int needed) {
    return ensure_capacity((void**)&state->locals, &state->local_cap, needed, sizeof(Local));
}

static void add_local(const char* name, int length, int reg, int depth) {
    if (!ensure_local_capacity(&cst, cst.local_count + 1)) {
        compiler_fatal_oom();
    }
    Local* local = &cst.locals[cst.local_count++];
    local->name = name;
    local->length = length;
    local->reg = reg;
    local->depth = depth;
}

static void register_function_symbol(ObjFunction* fn) {
    if (!ensure_capacity((void**)&fn_registry, &fn_cap, fn_count + 1, sizeof(ObjFunction*))) {
        compiler_fatal_oom();
    }
    fn_registry[fn_count++] = fn;
}

static void register_struct_symbol(ObjStruct* st) {
    if (!ensure_capacity((void**)&st_registry, &st_cap, st_count + 1, sizeof(ObjStruct*))) {
        compiler_fatal_oom();
    }
    st_registry[st_count++] = st;
}

static void clear_namespace_registry(void) {
    ns_count = 0;
}

static int find_namespace_entry(const char* alias, int alias_len,
                                int kind, const char* name, int name_len) {
    if (!alias || alias_len <= 0 || !name || name_len <= 0) return -1;
    for (int i = 0; i < ns_count; i++) {
        NamespaceEntry* entry = &ns_registry[i];
        if (entry->kind != kind) continue;
        if ((int)strlen(entry->alias) != alias_len) continue;
        if (entry->name_len != name_len) continue;
        if (memcmp(entry->alias, alias, (size_t)alias_len) != 0) continue;
        if (memcmp(entry->name, name, (size_t)name_len) != 0) continue;
        return i;
    }
    return -1;
}

static bool register_namespace_symbol(const char* alias, int alias_len,
                                      int kind, const char* name, int name_len,
                                      Obj* obj, const char* module_path) {
    if (!alias || alias_len <= 0 || !name || name_len <= 0 || !obj) return false;
    if (alias_len >= MAX_NAMESPACE_ALIAS_LEN) {
        printf("Compiler Error: Namespace alias too long for import '%s'.\n", module_path);
        return false;
    }

    int existing = find_namespace_entry(alias, alias_len, kind, name, name_len);
    if (existing >= 0) {
        if (ns_registry[existing].obj != obj) {
            printf("Compiler Error: Namespaced symbol conflict for '%.*s.%.*s'.\n",
                   alias_len, alias, name_len, name);
            return false;
        }
        return true;
    }

    if (!ensure_capacity((void**)&ns_registry, &ns_cap, ns_count + 1, sizeof(NamespaceEntry))) {
        compiler_fatal_oom();
    }

    NamespaceEntry* entry = &ns_registry[ns_count++];
    memcpy(entry->alias, alias, (size_t)alias_len);
    entry->alias[alias_len] = '\0';
    entry->kind = kind;
    entry->name = name;
    entry->name_len = name_len;
    entry->obj = obj;
    return true;
}

static ObjFunction* resolve_namespace_function(const char* alias, int alias_len,
                                               const char* name, int name_len) {
    int idx = find_namespace_entry(alias, alias_len, ABI_KIND_FUNCTION, name, name_len);
    if (idx < 0) return NULL;
    return (ObjFunction*)ns_registry[idx].obj;
}

static ObjStruct* resolve_namespace_struct(const char* alias, int alias_len,
                                           const char* name, int name_len) {
    int idx = find_namespace_entry(alias, alias_len, ABI_KIND_STRUCT, name, name_len);
    if (idx < 0) return NULL;
    return (ObjStruct*)ns_registry[idx].obj;
}

static void register_global_symbol(const char* name, int length) {
    if (!ensure_capacity((void**)&global_vars, &global_var_cap,
                         global_var_count + 1, sizeof(GlobalVar))) {
        compiler_fatal_oom();
    }
    global_vars[global_var_count].name = name;
    global_vars[global_var_count].length = length;
    global_var_count++;
}

static void init_compiler(ObjFunction* fn) {
    cst.locals = NULL;
    cst.local_count = 0;
    cst.local_cap = 0;
    cst.next_reg = 1;       // R0 is reserved for call bookkeeping
    cst.scope_depth = 0;
    cst.current_fn = fn;
}

static Chunk* current_chunk() {
    return &cst.current_fn->chunk;
}

static int resolve_local(const char* name, int length) {
    for (int i = cst.local_count - 1; i >= 0; i--) {
        if (cst.locals[i].length == length &&
            memcmp(cst.locals[i].name, name, length) == 0) {
            return cst.locals[i].reg;
        }
    }
    return -1;
}

static ObjFunction* resolve_function(const char* name, int length) {
    for (int i = 0; i < fn_count; i++) {
        if (fn_registry[i]->name_len == length &&
            memcmp(fn_registry[i]->name, name, length) == 0) {
            return fn_registry[i];
        }
    }
    return NULL;
}

static ObjStruct* resolve_struct(const char* name, int length) {
    for (int i = 0; i < st_count; i++) {
        if (st_registry[i]->name_len == length &&
            memcmp(st_registry[i]->name, name, length) == 0) {
            return st_registry[i];
        }
    }
    return NULL;
}

static bool is_main_function_name(const ObjFunction* fn) {
    return fn &&
           fn->name_len == 8 &&
           memcmp(fn->name, "__main__", 8) == 0;
}

typedef struct {
    int kind;
    char name[MAX_ABI_NAME_LEN];
    int metric;
} AbiSymbol;

typedef struct {
    int kind;
    const char* name;
    int name_len;
    int metric;
    Obj* obj;
} ModuleSymbol;

typedef struct {
    int kind;
    const char* name;
    int name_len;
} ExportDecl;

static const char* abi_symbol_label(int kind) {
    return (kind == ABI_KIND_FUNCTION) ? "function" : "struct";
}

static int find_abi_symbol(const AbiSymbol* symbols, int count, int kind, const char* name) {
    if (!symbols || !name) return -1;
    for (int i = 0; i < count; i++) {
        if (symbols[i].kind == kind && strcmp(symbols[i].name, name) == 0) return i;
    }
    return -1;
}

static int find_module_symbol(const ModuleSymbol* symbols, int count,
                              int kind, const char* name, int name_len) {
    if (!symbols || !name || name_len <= 0) return -1;
    for (int i = 0; i < count; i++) {
        if (symbols[i].kind != kind || symbols[i].name_len != name_len) continue;
        if (memcmp(symbols[i].name, name, (size_t)name_len) == 0) return i;
    }
    return -1;
}

static bool append_abi_symbol(AbiSymbol** symbols, int* count, int* cap,
                              int kind, const char* name, int metric,
                              const char* abi_path, int line_no) {
    if (!symbols || !count || !cap || !name) return false;
    if (metric < 0 || metric > 255) {
        printf("Compiler Error: Invalid ABI metric on %s:%d (%s %s %d).\n",
               abi_path, line_no, (kind == ABI_KIND_FUNCTION ? "fn" : "st"), name, metric);
        return false;
    }

    int existing = find_abi_symbol(*symbols, *count, kind, name);
    if (existing >= 0) {
        if ((*symbols)[existing].metric != metric) {
            printf("Compiler Error: Conflicting ABI entries on %s:%d for %s '%s'.\n",
                   abi_path, line_no, abi_symbol_label(kind), name);
            return false;
        }
        return true;
    }

    if (!ensure_capacity((void**)symbols, cap, *count + 1, sizeof(AbiSymbol))) {
        compiler_fatal_oom();
    }

    AbiSymbol* out = &(*symbols)[*count];
    out->kind = kind;
    snprintf(out->name, sizeof(out->name), "%s", name);
    out->metric = metric;
    (*count)++;
    return true;
}

static bool parse_metric_token(const char* token, int* out_metric) {
    if (!token || !out_metric) return false;
    errno = 0;
    char* end = NULL;
    long value = strtol(token, &end, 10);
    if (errno != 0 || end == token || *end != '\0' || value < 0 || value > INT_MAX) {
        return false;
    }
    *out_metric = (int)value;
    return true;
}

static bool load_module_abi_symbols(const char* abi_path, AbiSymbol** out_symbols, int* out_count) {
    *out_symbols = NULL;
    *out_count = 0;
    int cap = 0;

    FILE* file = fopen(abi_path, "r");
    if (!file) return false;

    bool ok = true;
    int line_no = 0;
    char line[512];
    while (fgets(line, sizeof(line), file) != NULL) {
        line_no++;

        char* cursor = line;
        while (*cursor && isspace((unsigned char)*cursor)) cursor++;
        if (*cursor == '\0' || *cursor == '#') continue;

        char* kind_tok = strtok(cursor, " \t\r\n");
        char* name_tok = strtok(NULL, " \t\r\n");
        char* metric_tok = strtok(NULL, " \t\r\n");
        char* extra_tok = strtok(NULL, " \t\r\n");

        if (!kind_tok || !name_tok || !metric_tok || extra_tok) {
            printf("Compiler Error: Invalid ABI entry on %s:%d.\n", abi_path, line_no);
            ok = false;
            break;
        }

        int kind = 0;
        if (strcmp(kind_tok, "fn") == 0) {
            kind = ABI_KIND_FUNCTION;
        } else if (strcmp(kind_tok, "st") == 0) {
            kind = ABI_KIND_STRUCT;
        } else {
            printf("Compiler Error: Invalid ABI symbol kind on %s:%d: %s\n",
                   abi_path, line_no, kind_tok);
            ok = false;
            break;
        }

        size_t name_len = strlen(name_tok);
        if (name_len == 0 || name_len >= MAX_ABI_NAME_LEN) {
            printf("Compiler Error: Invalid ABI symbol name on %s:%d.\n", abi_path, line_no);
            ok = false;
            break;
        }

        int metric = 0;
        if (!parse_metric_token(metric_tok, &metric)) {
            printf("Compiler Error: Invalid ABI metric on %s:%d.\n", abi_path, line_no);
            ok = false;
            break;
        }

        if (!append_abi_symbol(out_symbols, out_count, &cap, kind, name_tok, metric, abi_path, line_no)) {
            ok = false;
            break;
        }
    }

    if (ok && ferror(file)) ok = false;
    fclose(file);

    if (!ok) {
        free(*out_symbols);
        *out_symbols = NULL;
        *out_count = 0;
        return false;
    }

    return true;
}

static bool append_module_symbol(ModuleSymbol** symbols, int* count, int* cap,
                                 int kind, const char* name, int name_len,
                                 int metric, Obj* obj, const char* module_path) {
    if (!symbols || !count || !cap || !name || name_len <= 0 || !obj) return false;

    int existing = find_module_symbol(*symbols, *count, kind, name, name_len);
    if (existing >= 0) {
        if ((*symbols)[existing].metric != metric || (*symbols)[existing].obj != obj) {
            printf("Compiler Error: Export collision in bytecode module '%s' for %s '%.*s'.\n",
                   module_path, abi_symbol_label(kind), name_len, name);
            return false;
        }
        return true;
    }

    if (!ensure_capacity((void**)symbols, cap, *count + 1, sizeof(ModuleSymbol))) {
        compiler_fatal_oom();
    }

    ModuleSymbol* out = &(*symbols)[*count];
    out->kind = kind;
    out->name = name;
    out->name_len = name_len;
    out->metric = metric;
    out->obj = obj;
    (*count)++;
    return true;
}

static bool check_registry_collision(const ModuleSymbol* symbol, const char* module_path) {
    if (!symbol || !module_path) return false;
    ObjFunction* fn = resolve_function(symbol->name, symbol->name_len);
    ObjStruct* st = resolve_struct(symbol->name, symbol->name_len);

    if (symbol->kind == ABI_KIND_FUNCTION) {
        if (fn) {
            printf("Compiler Error: Import conflict for package '%s': function '%.*s' already exists.\n",
                   module_path, symbol->name_len, symbol->name);
            return false;
        }
        if (st) {
            printf("Compiler Error: Import conflict for package '%s': function '%.*s' collides with struct.\n",
                   module_path, symbol->name_len, symbol->name);
            return false;
        }
    } else {
        if (st) {
            printf("Compiler Error: Import conflict for package '%s': struct '%.*s' already exists.\n",
                   module_path, symbol->name_len, symbol->name);
            return false;
        }
        if (fn) {
            printf("Compiler Error: Import conflict for package '%s': struct '%.*s' collides with function.\n",
                   module_path, symbol->name_len, symbol->name);
            return false;
        }
    }

    return true;
}

static bool append_export_decl(ExportDecl** out, int* count, int* cap,
                               int kind, const char* name, int name_len) {
    if (!out || !count || !cap || !name || name_len <= 0) return false;

    for (int i = 0; i < *count; i++) {
        if ((*out)[i].kind == kind &&
            (*out)[i].name_len == name_len &&
            memcmp((*out)[i].name, name, (size_t)name_len) == 0) {
            return true;
        }
    }

    if (!ensure_capacity((void**)out, cap, *count + 1, sizeof(ExportDecl))) {
        return false;
    }

    ExportDecl* entry = &(*out)[*count];
    entry->kind = kind;
    entry->name = name;
    entry->name_len = name_len;
    (*count)++;
    return true;
}

static bool export_decl_contains(const ExportDecl* exports, int export_count,
                                 int kind, const char* name, int name_len) {
    if (!exports || !name || name_len <= 0) return false;
    for (int i = 0; i < export_count; i++) {
        if (exports[i].kind != kind || exports[i].name_len != name_len) continue;
        if (memcmp(exports[i].name, name, (size_t)name_len) == 0) return true;
    }
    return false;
}

static bool collect_module_export_decls(AstNode* mod_ast,
                                        ExportDecl** out_exports, int* out_count,
                                        bool* out_has_public) {
    *out_exports = NULL;
    *out_count = 0;
    *out_has_public = false;
    int cap = 0;

    if (!mod_ast || mod_ast->type != AST_PROGRAM) return true;

    for (int i = 0; i < mod_ast->data.block.count; i++) {
        AstNode* stmt = mod_ast->data.block.statements[i];
        if (!stmt) continue;

        if (stmt->type == AST_FUNC_DECL && stmt->data.func_decl.is_public) {
            *out_has_public = true;
            if (!append_export_decl(out_exports, out_count, &cap, ABI_KIND_FUNCTION,
                                    stmt->data.func_decl.name.start,
                                    stmt->data.func_decl.name.length)) {
                free(*out_exports);
                *out_exports = NULL;
                *out_count = 0;
                return false;
            }
        } else if (stmt->type == AST_STRUCT_DECL && stmt->data.struct_decl.is_public) {
            *out_has_public = true;
            if (!append_export_decl(out_exports, out_count, &cap, ABI_KIND_STRUCT,
                                    stmt->data.struct_decl.name.start,
                                    stmt->data.struct_decl.name.length)) {
                free(*out_exports);
                *out_exports = NULL;
                *out_count = 0;
                return false;
            }
        }
    }

    return true;
}

static bool filter_or_namespace_imported_symbols(int base_fn_count, int base_st_count,
                                                 const ExportDecl* exports, int export_count, bool has_public,
                                                 const char* namespace_alias, int alias_len,
                                                 bool apply_public_filter, const char* module_path) {
    bool use_namespace = namespace_alias && alias_len > 0;

    int write_fn = base_fn_count;
    for (int i = base_fn_count; i < fn_count; i++) {
        ObjFunction* fn = fn_registry[i];
        bool exported = !has_public || export_decl_contains(exports, export_count, ABI_KIND_FUNCTION,
                                                            fn->name, fn->name_len);
        if (use_namespace) {
            if (exported) {
                if (!register_namespace_symbol(namespace_alias, alias_len, ABI_KIND_FUNCTION,
                                               fn->name, fn->name_len, (Obj*)fn, module_path)) {
                    return false;
                }
            }
        } else {
            if (!apply_public_filter || exported) {
                fn_registry[write_fn++] = fn;
            }
        }
    }

    int write_st = base_st_count;
    for (int i = base_st_count; i < st_count; i++) {
        ObjStruct* st = st_registry[i];
        bool exported = !has_public || export_decl_contains(exports, export_count, ABI_KIND_STRUCT,
                                                            st->name, st->name_len);
        if (use_namespace) {
            if (exported) {
                if (!register_namespace_symbol(namespace_alias, alias_len, ABI_KIND_STRUCT,
                                               st->name, st->name_len, (Obj*)st, module_path)) {
                    return false;
                }
            }
        } else {
            if (!apply_public_filter || exported) {
                st_registry[write_st++] = st;
            }
        }
    }

    if (use_namespace) {
        fn_count = base_fn_count;
        st_count = base_st_count;
    } else {
        fn_count = write_fn;
        st_count = write_st;
    }

    return true;
}

static bool import_precompiled_package_module(const char* resolved_module_path,
                                              const char* namespace_alias, int alias_len,
                                              ObjFunction** out_module_main) {
    char vbb_path[MAX_PATH_LEN];
    if (!build_vbb_path(resolved_module_path, vbb_path, sizeof(vbb_path))) return false;
    if (!file_exists(vbb_path)) return false;

    char vabi_path[MAX_PATH_LEN];
    if (!build_vabi_path(resolved_module_path, vabi_path, sizeof(vabi_path))) return false;
    if (!file_exists(vabi_path)) return false;

    AbiSymbol* abi_symbols = NULL;
    int abi_count = 0;
    if (!load_module_abi_symbols(vabi_path, &abi_symbols, &abi_count)) {
        printf("Compiler Error: Could not read ABI file '%s'.\n", vabi_path);
        exit(1);
    }

    ObjFunction* root = read_bytecode_file(vbb_path);
    if (!root) {
        free(abi_symbols);
        return false;
    }
    if (out_module_main) *out_module_main = root;

    Obj** queue = NULL;
    int q_count = 0;
    int q_cap = 0;
    ModuleSymbol* module_symbols = NULL;
    int symbol_count = 0;
    int symbol_cap = 0;

    if (!ensure_capacity((void**)&queue, &q_cap, 1, sizeof(Obj*))) {
        free(abi_symbols);
        free(queue);
        compiler_fatal_oom();
    }
    queue[q_count++] = (Obj*)root;

    bool ok = true;
    for (int i = 0; i < q_count; i++) {
        Obj* obj = queue[i];
        if (!obj) continue;

        if (obj->type == OBJ_FUNCTION) {
            ObjFunction* fn = (ObjFunction*)obj;
            if (!is_main_function_name(fn)) {
                if (!append_module_symbol(&module_symbols, &symbol_count, &symbol_cap,
                                          ABI_KIND_FUNCTION, fn->name, fn->name_len,
                                          fn->arity, (Obj*)fn, resolved_module_path)) {
                    ok = false;
                    break;
                }
            }

            for (int ci = 0; ci < fn->chunk.constant_count; ci++) {
                Value v = fn->chunk.constants[ci];
                if (v.type != VAL_OBJ || v.as.obj == NULL) continue;
                Obj* dep = v.as.obj;
                if (dep->type != OBJ_FUNCTION && dep->type != OBJ_STRUCT) continue;

                bool seen = false;
                for (int q = 0; q < q_count; q++) {
                    if (queue[q] == dep) {
                        seen = true;
                        break;
                    }
                }
                if (seen) continue;

                if (!ensure_capacity((void**)&queue, &q_cap, q_count + 1, sizeof(Obj*))) {
                    free(module_symbols);
                    free(abi_symbols);
                    free(queue);
                    compiler_fatal_oom();
                }
                queue[q_count++] = dep;
            }
        } else if (obj->type == OBJ_STRUCT) {
            ObjStruct* st = (ObjStruct*)obj;
            if (!append_module_symbol(&module_symbols, &symbol_count, &symbol_cap,
                                      ABI_KIND_STRUCT, st->name, st->name_len,
                                      st->field_count, (Obj*)st, resolved_module_path)) {
                ok = false;
                break;
            }
        }
    }

    if (!ok) {
        free(module_symbols);
        free(abi_symbols);
        free(queue);
        exit(1);
    }

    for (int i = 0; i < abi_count; i++) {
        AbiSymbol* abi = &abi_symbols[i];
        int name_len = (int)strlen(abi->name);
        int found = find_module_symbol(module_symbols, symbol_count,
                                       abi->kind, abi->name, name_len);
        if (found < 0) {
            printf("Compiler Error: ABI mismatch for package module '%s': missing %s '%s' in bytecode.\n",
                   resolved_module_path, abi_symbol_label(abi->kind), abi->name);
            free(module_symbols);
            free(abi_symbols);
            free(queue);
            exit(1);
        }

        ModuleSymbol* symbol = &module_symbols[found];
        if (symbol->metric != abi->metric) {
            printf("Compiler Error: ABI mismatch for package module '%s': %s '%s' metric %d != %d.\n",
                   resolved_module_path, abi_symbol_label(abi->kind), abi->name,
                   symbol->metric, abi->metric);
            free(module_symbols);
            free(abi_symbols);
            free(queue);
            exit(1);
        }

        if (namespace_alias && alias_len > 0) {
            if (!register_namespace_symbol(namespace_alias, alias_len, symbol->kind,
                                           symbol->name, symbol->name_len, symbol->obj,
                                           resolved_module_path)) {
                free(module_symbols);
                free(abi_symbols);
                free(queue);
                exit(1);
            }
        } else {
            if (!check_registry_collision(symbol, resolved_module_path)) {
                free(module_symbols);
                free(abi_symbols);
                free(queue);
                exit(1);
            }
            if (symbol->kind == ABI_KIND_FUNCTION) {
                register_function_symbol((ObjFunction*)symbol->obj);
            } else {
                register_struct_symbol((ObjStruct*)symbol->obj);
            }
        }
    }

    free(module_symbols);
    free(abi_symbols);
    free(queue);
    return true;
}

// ---- Jump helpers --------------------------------------------------------

static int emit_jump(uint8_t jmpType, int regCond) {
    write_chunk(current_chunk(), ENCODE_INST(jmpType, regCond, 0, 0));
    return current_chunk()->count - 1;
}

static void patch_jump(int offset) {
    int jump = current_chunk()->count - offset - 1;
    uint32_t inst  = current_chunk()->code[offset];
    uint8_t  op    = DECODE_OP(inst);
    uint8_t  rA    = DECODE_A(inst);
    current_chunk()->code[offset] = ENCODE_INST(op, rA, (jump >> 8) & 0xFF, jump & 0xFF);
}

static void begin_scope() { cst.scope_depth++; }
static void end_scope() {
    cst.scope_depth--;
    while (cst.local_count > 0 &&
           cst.locals[cst.local_count - 1].depth > cst.scope_depth) {
        cst.local_count--;
    }
}

// ---- Expression Compiler -------------------------------------------------

static int compile_expr(AstNode* expr);
static void compile_stmt(AstNode* stmt);

static int compile_expr(AstNode* expr) {
    if (!expr) return -1;

    if (expr->type == AST_NUMBER) {
        int reg = cst.next_reg++;
        Value val = {VAL_NUMBER, {.number = expr->data.number_val}};
        int ci = add_constant(current_chunk(), val);
        write_chunk(current_chunk(), ENCODE_INST(OP_LOAD_CONST, reg, ci, 0));
        return reg;
    }

    if (expr->type == AST_STRING) {
        int reg = cst.next_reg++;
        ObjString* s = copy_string(expr->data.str_val.name, expr->data.str_val.length);
        Value val = {VAL_OBJ, {.obj = (Obj*)s}};
        int ci = add_constant(current_chunk(), val);
        write_chunk(current_chunk(), ENCODE_INST(OP_LOAD_CONST, reg, ci, 0));
        return reg;
    }

    if (expr->type == AST_IDENTIFIER) {
        int reg = resolve_local(expr->data.identifier.name.start, expr->data.identifier.name.length);
        if (reg == -1) {
            printf("Compiler Error: Undefined variable '%.*s'\n",
                expr->data.identifier.name.length, expr->data.identifier.name.start);
            exit(1);
        }
        return reg;
    }

    if (expr->type == AST_ASSIGN_EXPR) {
        int src = compile_expr(expr->data.assign.value);
        int dst = resolve_local(expr->data.assign.name.start, expr->data.assign.name.length);
        if (dst == -1) { printf("Compiler Error: Undefined variable for assignment\n"); exit(1); }
        write_chunk(current_chunk(), ENCODE_INST(OP_MOVE, dst, src, 0));
        return dst;
    }

    if (expr->type == AST_BINARY_EXPR) {
        int rL = compile_expr(expr->data.binary.left);
        int rR = compile_expr(expr->data.binary.right);
        int rD = cst.next_reg++;
        uint8_t op = OP_ADD;
        switch (expr->data.binary.op.type) {
            case TOKEN_PLUS:        op = OP_ADD;     break;
            case TOKEN_MINUS:       op = OP_SUB;     break;
            case TOKEN_STAR:        op = OP_MUL;     break;
            case TOKEN_SLASH:       op = OP_DIV;     break;
            case TOKEN_LESS:        op = OP_LESS;    break;
            case TOKEN_GREATER:     op = OP_GREATER; break;
            case TOKEN_EQUAL_EQUAL: op = OP_EQUAL;   break;
            case TOKEN_PLUS_TILDE:  op = OP_ADD_WRAP;break;
            case TOKEN_CARET_PLUS:  op = OP_ADD_SAT; break;
            default: break;
        }
        write_chunk(current_chunk(), ENCODE_INST(op, rD, rL, rR));
        return rD;
    }

    if (expr->type == AST_CALL_EXPR) {
        const char* name = expr->data.call_expr.name.start;
        int namelen      = expr->data.call_expr.name.length;
        int arg_count    = expr->data.call_expr.arg_count;
        int* arg_regs = NULL;
        ObjFunction* callee_fn = NULL;
        ObjStruct* callee_st = NULL;
        int native_idx = -1;

        if (arg_count > 255) {
            printf("Compiler Error: Too many call arguments (%d).\n", arg_count);
            exit(1);
        }

        if (arg_count > 0) {
            arg_regs = (int*)malloc(sizeof(int) * (size_t)arg_count);
            if (!arg_regs) compiler_fatal_oom();
        }

        AstNode* callee_expr = expr->data.call_expr.callee;
        if (callee_expr && callee_expr->type == AST_GET_EXPR &&
            callee_expr->data.get_expr.obj &&
            callee_expr->data.get_expr.obj->type == AST_IDENTIFIER) {
            Token alias = callee_expr->data.get_expr.obj->data.identifier.name;
            Token member = callee_expr->data.get_expr.name;
            callee_fn = resolve_namespace_function(alias.start, alias.length,
                                                   member.start, member.length);
            callee_st = callee_fn ? NULL : resolve_namespace_struct(alias.start, alias.length,
                                                                    member.start, member.length);
            if (!callee_fn && !callee_st) {
                // If namespace resolution fails, maybe it's a native method?
                native_idx = find_native_index(member.start, member.length);
                if (native_idx == -1) {
                    printf("Compiler Error: Unknown namespaced callable '%.*s.%.*s'.\n",
                           alias.length, alias.start, member.length, member.start);
                    free(arg_regs);
                    exit(1);
                }
                
                // For native methods (like obj.get_fn()), the object itself is the first argument!
                int obj_reg = resolve_local(alias.start, alias.length);
                if (obj_reg == -1) {
                     printf("Compiler Error: Undefined object '%.*s' for method call.\n", alias.length, alias.start);
                     free(arg_regs);
                     exit(1);
                }
                
                // Shift all arguments by 1 to make room for 'self'
                int* new_args = malloc(sizeof(int) * (arg_count + 1));
                new_args[0] = obj_reg;
                for(int i=0; i<arg_count; i++) {
                    new_args[i+1] = compile_expr(expr->data.call_expr.args[i]);
                }
                free(arg_regs);
                arg_regs = new_args;
                arg_count++; // method acts like a native with N+1 args
                
                // Now emit the NATIVE_CALL immediately and RETURN
                for (int i = 0; i < arg_count; i++) {
                    int argReg = cst.next_reg++;
                    write_chunk(current_chunk(), ENCODE_INST(OP_MOVE, argReg, arg_regs[i], 0));
                }
                int destReg = cst.next_reg++;
                write_chunk(current_chunk(), ENCODE_INST(OP_CALL_NATIVE, (uint8_t)native_idx, (uint8_t)arg_count, (uint8_t)destReg));
                free(arg_regs);
                return destReg;
            }
        } else if (callee_expr && callee_expr->type == AST_IDENTIFIER) {
            callee_fn = resolve_function(name, namelen);
            callee_st = callee_fn ? NULL : resolve_struct(name, namelen);
            native_idx = (callee_fn || callee_st) ? -1 : find_native_index(name, namelen);
            
            // If it's not a static function, struct, or built-in native, 
            // it might be a dynamic function variable (e.g., `v puts = lib.get_fn(...)`).
            // We can just compile it as a variable access and let the VM handle the call.
            if (!callee_fn && !callee_st && native_idx == -1) {
                int reg = resolve_local(name, namelen);
                if (reg != -1) {
                    for (int i = 0; i < arg_count; i++) {
                        arg_regs[i] = compile_expr(expr->data.call_expr.args[i]);
                    }
                    
                    int fnReg = cst.next_reg++;
                    write_chunk(current_chunk(), ENCODE_INST(OP_MOVE, fnReg, reg, 0));
                    
                    for (int i = 0; i < arg_count; i++) {
                        int argReg = cst.next_reg++;
                        write_chunk(current_chunk(), ENCODE_INST(OP_MOVE, argReg, arg_regs[i], 0));
                    }
                    int destReg = cst.next_reg++;
                    write_chunk(current_chunk(), ENCODE_INST(OP_CALL, fnReg, (uint8_t)arg_count, (uint8_t)destReg));
                    free(arg_regs);
                    return destReg;
                } else {
                    printf("Compiler Error: Unknown callable '%.*s' (len=%d)\n", namelen, name, namelen);
                    free(arg_regs);
                    exit(1);
                }
            }
        } else {
            printf("Compiler Error: Unsupported call target.\n");
            free(arg_regs);
            exit(1);
        }

        // We only reach here if it's a STATIC func, STATIC struct, or STATIC built-in native.
        for (int i = 0; i < arg_count; i++) {
            arg_regs[i] = compile_expr(expr->data.call_expr.args[i]);
        }

        if (native_idx != -1) {
            for (int i = 0; i < arg_count; i++) {
                int argReg = cst.next_reg++;
                write_chunk(current_chunk(), ENCODE_INST(OP_MOVE, argReg, arg_regs[i], 0));
            }
            int destReg = cst.next_reg++;
            write_chunk(current_chunk(), ENCODE_INST(OP_CALL_NATIVE, (uint8_t)native_idx, (uint8_t)arg_count, (uint8_t)destReg));
            free(arg_regs);
            return destReg;
        }

        int fnReg = cst.next_reg++;
        Value val = {VAL_OBJ, {.obj = callee_fn ? (Obj*)callee_fn : (Obj*)callee_st}};
        int ci = add_constant(current_chunk(), val);
        write_chunk(current_chunk(), ENCODE_INST(OP_LOAD_CONST, fnReg, ci, 0));

        for (int i = 0; i < arg_count; i++) {
            int argReg = cst.next_reg++;
            write_chunk(current_chunk(), ENCODE_INST(OP_MOVE, argReg, arg_regs[i], 0));
        }

        int destReg = cst.next_reg++;
        write_chunk(current_chunk(), ENCODE_INST(OP_CALL, fnReg, (uint8_t)arg_count, (uint8_t)destReg));
        free(arg_regs);
        return destReg;
    }

    if (expr->type == AST_GET_EXPR) {
        int rObj = compile_expr(expr->data.get_expr.obj);
        int rDest = cst.next_reg++;
        // Add field name as a string constant
        ObjString* fieldName = copy_string(expr->data.get_expr.name.start, expr->data.get_expr.name.length);
        int ci = add_constant(current_chunk(), (Value){VAL_OBJ, {.obj = (Obj*)fieldName}});
        write_chunk(current_chunk(), ENCODE_INST(OP_GET_FIELD, rDest, rObj, ci));
        return rDest;
    }

    if (expr->type == AST_SET_EXPR) {
        int rObj = compile_expr(expr->data.set_expr.obj);
        int rVal = compile_expr(expr->data.set_expr.value);
        ObjString* fieldName = copy_string(expr->data.set_expr.name.start, expr->data.set_expr.name.length);
        int ci = add_constant(current_chunk(), (Value){VAL_OBJ, {.obj = (Obj*)fieldName}});
        write_chunk(current_chunk(), ENCODE_INST(OP_SET_FIELD, rObj, ci, rVal));
        return rVal;
    }

    return -1;
}

// ---- Statement Compiler --------------------------------------------------

static void compile_stmt(AstNode* stmt) {
    if (!stmt) return;

    if (stmt->type == AST_VAR_DECL) {
        int reg = compile_expr(stmt->data.var_decl.initializer);
        add_local(stmt->data.var_decl.name.start,
                  stmt->data.var_decl.name.length,
                  reg,
                  cst.scope_depth);

        if (cst.scope_depth == 0) {
            register_global_symbol(stmt->data.var_decl.name.start,
                                   stmt->data.var_decl.name.length);
        }
    }
    else if (stmt->type == AST_BLOCK_STMT) {
        begin_scope();
        for (int i = 0; i < stmt->data.block.count; i++) compile_stmt(stmt->data.block.statements[i]);
        end_scope();
    }
    else if (stmt->type == AST_IF_STMT) {
        int rCond      = compile_expr(stmt->data.if_stmt.condition);
        int jiFalse    = emit_jump(OP_JUMP_IF_FALSE, rCond);
        compile_stmt(stmt->data.if_stmt.then_branch);
        int jEnd       = emit_jump(OP_JUMP, 0);
        patch_jump(jiFalse);
        if (stmt->data.if_stmt.else_branch) compile_stmt(stmt->data.if_stmt.else_branch);
        patch_jump(jEnd);
    }
    else if (stmt->type == AST_WHILE_STMT) {
        int loopStart = current_chunk()->count;
        int rCond     = compile_expr(stmt->data.while_stmt.condition);
        int exitJump  = emit_jump(OP_JUMP_IF_FALSE, rCond);
        compile_stmt(stmt->data.while_stmt.body);
        int offset    = current_chunk()->count - loopStart + 1;
        write_chunk(current_chunk(), ENCODE_INST(OP_LOOP, 0, (offset >> 8) & 0xFF, offset & 0xFF));
        patch_jump(exitJump);
    }
    else if (stmt->type == AST_RETURN_STMT) {
        if (stmt->data.return_stmt.value) {
            int reg = compile_expr(stmt->data.return_stmt.value);
            write_chunk(current_chunk(), ENCODE_INST(OP_RETURN, reg, 0, 0));
        } else {
            write_chunk(current_chunk(), ENCODE_INST(OP_RETURN, 0, 0, 0));
        }
    }
    else if (stmt->type == AST_FUNC_DECL) {
        // Create a new ObjFunction and compile the body into it
        ObjFunction* fn = new_function(
            stmt->data.func_decl.name.start,
            stmt->data.func_decl.name.length,
            stmt->data.func_decl.param_count
        );

        // Register globally so call expressions can find it
        register_function_symbol(fn);

        // Save outer compiler state and start compiling the function body
        CompilerState outer = cst;
        init_compiler(fn);

        // Bind parameters as locals at register 1..N (R0 is bookkeeping)
        for (int i = 0; i < stmt->data.func_decl.param_count; i++) {
            add_local(stmt->data.func_decl.params[i].start,
                      stmt->data.func_decl.params[i].length,
                      cst.next_reg++,
                      0);
        }

        // Compile function body
        compile_stmt(stmt->data.func_decl.body);

        // Implicit return nil if no explicit ret
        write_chunk(current_chunk(), ENCODE_INST(OP_RETURN, 0, 0, 0));

        free(cst.locals);
        // Restore outer state
        cst = outer;
    }
    else if (stmt->type == AST_STRUCT_DECL) {
        // Prepare arrays of field names and lengths
        const char** field_names = malloc(sizeof(char*) * stmt->data.struct_decl.field_count);
        int* field_lens = malloc(sizeof(int) * stmt->data.struct_decl.field_count);
        for (int i = 0; i < stmt->data.struct_decl.field_count; i++) {
            field_names[i] = stmt->data.struct_decl.fields[i].start;
            field_lens[i] = stmt->data.struct_decl.fields[i].length;
        }

        ObjStruct* st = new_struct(
            stmt->data.struct_decl.name.start,
            stmt->data.struct_decl.name.length,
            stmt->data.struct_decl.field_count,
            field_names,
            field_lens
        );

        register_struct_symbol(st);

        int reg = cst.next_reg++;
        int ci = add_constant(current_chunk(), (Value){VAL_OBJ, {.obj = (Obj*)st}});
        write_chunk(current_chunk(), ENCODE_INST(OP_STRUCT, reg, ci, 0));
        
        // Register in local table if at top level so it acts like a global type name
        if (cst.scope_depth == 0) {
            add_local(stmt->data.struct_decl.name.start,
                      stmt->data.struct_decl.name.length,
                      reg,
                      0);
        }
    }
    else if (stmt->type == AST_USE_STMT) {
        // Module system: Resolve relative to importer and compile once.
        char raw_path[MAX_PATH_LEN];
        char resolved_path[MAX_PATH_LEN];
        char module_dir[MAX_PATH_LEN];
        char alias_buf[MAX_NAMESPACE_ALIAS_LEN];
        int alias_len = 0;
        ObjFunction* package_main = NULL;
        int base_fn_count = fn_count;
        int base_st_count = st_count;

        decode_use_path(stmt->data.use_stmt.path, raw_path, sizeof(raw_path));
        if (stmt->data.use_stmt.alias.length > 0) {
            if ((size_t)stmt->data.use_stmt.alias.length >= sizeof(alias_buf)) {
                printf("Compiler Error: Namespace alias is too long.\n");
                exit(1);
            }
            memcpy(alias_buf, stmt->data.use_stmt.alias.start, (size_t)stmt->data.use_stmt.alias.length);
            alias_buf[stmt->data.use_stmt.alias.length] = '\0';
            alias_len = stmt->data.use_stmt.alias.length;
        }
        resolve_module_path(raw_path, resolved_path, sizeof(resolved_path));

        if (module_already_imported(resolved_path)) return;

        if (is_package_import(raw_path) &&
            import_precompiled_package_module(resolved_path, alias_len > 0 ? alias_buf : NULL,
                                              alias_len, &package_main)) {
            remember_imported_module(resolved_path);
            // Execute package module init once to preserve top-level side effects.
            if (package_main) {
                int fn_reg = cst.next_reg++;
                int fn_ci = add_constant(current_chunk(), (Value){VAL_OBJ, {.obj = (Obj*)package_main}});
                write_chunk(current_chunk(), ENCODE_INST(OP_LOAD_CONST, fn_reg, fn_ci, 0));
                int dest_reg = cst.next_reg++;
                write_chunk(current_chunk(), ENCODE_INST(OP_CALL, fn_reg, 0, dest_reg));
            }
            return;
        }

        char* source = NULL;
        if (!load_module_source(resolved_path, &source)) {
            printf("Compiler Error: Could not open module '%s' (resolved as '%s').\n",
                   raw_path, resolved_path);
            exit(1);
        }
        remember_imported_module(resolved_path);

        path_dirname(resolved_path, module_dir, sizeof(module_dir));
        push_module_dir(module_dir);
        AstNode* mod_ast = parse(source);

        ExportDecl* exports = NULL;
        int export_count = 0;
        bool has_public = false;
        if (!collect_module_export_decls(mod_ast, &exports, &export_count, &has_public)) {
            printf("Compiler Error: Out of memory while collecting exports for '%s'.\n", resolved_path);
            exit(1);
        }

        if (mod_ast->type == AST_PROGRAM) {
            for (int i = 0; i < mod_ast->data.block.count; i++) {
                compile_stmt(mod_ast->data.block.statements[i]);
            }
        }
        pop_module_dir();

        bool apply_public_filter = is_package_import(raw_path) && has_public;
        if ((alias_len > 0) || apply_public_filter) {
            const char* alias = (alias_len > 0) ? alias_buf : NULL;
            if (!filter_or_namespace_imported_symbols(base_fn_count, base_st_count,
                                                      exports, export_count, has_public,
                                                      alias, alias_len,
                                                      apply_public_filter, resolved_path)) {
                free(exports);
                printf("Compiler Error: Failed to apply import visibility for '%s'.\n", resolved_path);
                exit(1);
            }
        }
        free(exports);

        // Note: We do NOT free(source) here because names of functions/structs 
        // compiled from this module point directly into this source string.
    }
    else if (stmt->type == AST_EXPR_STMT) {
        (void)compile_expr(stmt->data.expr_stmt.expr);
    }
}

// ---- Top-Level Entry Point -----------------------------------------------

void generate_contract() {
    printf("\n--- @contract ---\n");
    printf("// @contract\n");
    
    if (global_var_count > 0) {
        printf("// export_v: ");
        for (int i = 0; i < global_var_count; i++) {
            printf("%.*s%s", global_vars[i].length, global_vars[i].name, (i == global_var_count - 1) ? "" : ", ");
        }
        printf("\n");
    }

    if (fn_count > 0) {
        printf("// export_fn: ");
        for (int i = 0; i < fn_count; i++) {
            printf("%.*s(%d)%s", fn_registry[i]->name_len, fn_registry[i]->name, fn_registry[i]->arity, (i == fn_count - 1) ? "" : ", ");
        }
        printf("\n");
    }
    printf("-----------------\n\n");
}

ObjFunction* compile(AstNode* ast) {
    clear_imported_modules();
    clear_module_dirs();
    clear_namespace_registry();
    if (cst.locals) {
        free(cst.locals);
        cst.locals = NULL;
    }
    fn_count = 0;
    st_count = 0;
    global_var_count = 0;
    cst.local_cap = 0;
    cst.local_count = 0;
    detect_project_root();

    char entry_dir[MAX_PATH_LEN];
    if (entry_file_path[0] != '\0') {
        path_dirname(entry_file_path, entry_dir, sizeof(entry_dir));
    } else {
        copy_path(entry_dir, sizeof(entry_dir), ".");
    }
    push_module_dir(entry_dir);

    // Wrap all top-level code in an implicit "main" function
    ObjFunction* main_fn = new_function("__main__", 8, 0);
    init_compiler(main_fn);

    if (ast->type == AST_PROGRAM) {
        for (int i = 0; i < ast->data.block.count; i++) {
            compile_stmt(ast->data.block.statements[i]);
        }
    } else {
        compile_stmt(ast);
    }

    // Keep exported symbols reachable from root for bytecode serialization.
    for (int i = 0; i < fn_count; i++) {
        add_constant(current_chunk(), (Value){VAL_OBJ, {.obj = (Obj*)fn_registry[i]}});
    }
    for (int i = 0; i < st_count; i++) {
        add_constant(current_chunk(), (Value){VAL_OBJ, {.obj = (Obj*)st_registry[i]}});
    }

    if (emit_halt_enabled) {
        write_chunk(current_chunk(), ENCODE_INST(OP_HALT, 0, 0, 0));
    } else {
        write_chunk(current_chunk(), ENCODE_INST(OP_RETURN, 0, 0, 0));
    }
    pop_module_dir();
    free(cst.locals);
    cst.locals = NULL;
    cst.local_cap = 0;
    cst.local_count = 0;
    
    if (contract_output_enabled) {
        generate_contract();
    }
    
    return main_fn;
}
