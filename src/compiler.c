#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <unistd.h>
#include "parser.h"
#include "compiler.h"
#include "bytecode.h"
#include "vm.h"
#include "native.h"
#include "capabilities.h"

ObjFunction* new_function(const char* name, int name_len, int arity);
static void register_struct_prototypes(AstNode* node, const char* alias, int alias_len);
static void register_function_prototypes(AstNode* node, const char* alias, int alias_len);

// ---- Local Variable Table -----------------------------------------------

typedef enum {
    STATIC_TYPE_UNKNOWN,
    STATIC_TYPE_ANY,
    STATIC_TYPE_NUMBER,
    STATIC_TYPE_BOOL,
    STATIC_TYPE_STRING,
    STATIC_TYPE_ARRAY,
    STATIC_TYPE_FUNCTION,
    STATIC_TYPE_NIL,
    STATIC_TYPE_STRUCT_DEF,
    STATIC_TYPE_STRUCT_INSTANCE
} StaticTypeKind;

typedef struct sStaticType {
    StaticTypeKind kind;
    ObjStruct* struct_obj;
} StaticType;

typedef struct {
    const char* name;
    int length;
    int reg;
    int depth;
    bool has_declared_type;
    StaticType type;
} Local;

#define MAX_PATH_LEN 1024
#define MAX_ABI_NAME_LEN 128
#define MAX_ABI_EFFECTS_LEN 256
#define MAX_ABI_PARAMS_LEN 512
#define MAX_NAMESPACE_ALIAS_LEN 128
#define ABI_KIND_FUNCTION 1
#define ABI_KIND_STRUCT 2

static char entry_file_path[MAX_PATH_LEN];
static char entry_dir_path[MAX_PATH_LEN];
static char project_root_path[MAX_PATH_LEN];

static char current_module_alias[MAX_NAMESPACE_ALIAS_LEN];
static int current_alias_len = 0;

static char** module_dir_stack = NULL;
static int module_dir_depth = 0;
static int module_dir_cap = 0;
static char** imported_modules = NULL;
static int imported_module_count = 0;
static int imported_module_cap = 0;
static bool contract_output_enabled = true;
static bool emit_halt_enabled = true;

static bool join_path2(const char* a, const char* b, char* out, size_t out_size);
static bool file_exists(const char* path);

static void compiler_error(const char* code, const char* fmt, ...) {
    va_list args;
    fprintf(stderr, "Compiler Error [%s]: ", code);
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

static void compiler_error_exit(const char* code, const char* fmt, ...) {
    va_list args;
    fprintf(stderr, "Compiler Error [%s]: ", code);
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
    exit(1);
}

static const char* capability_hint_for_symbol(const char* name, int len) {
    (void)name;
    (void)len;
#if !VIPER_CAP_WEB
    if ((len == 5 && memcmp(name, "serve", 5) == 0) ||
        (len == 5 && memcmp(name, "fetch", 5) == 0) ||
        (len >= 4 && memcmp(name, "web_", 4) == 0)) {
        return "web capability is disabled in this build profile";
    }
#endif
#if !VIPER_CAP_DB
    if ((len == 5 && memcmp(name, "query", 5) == 0) ||
        (len >= 4 && memcmp(name, "vdb_", 4) == 0)) {
        return "db capability is disabled in this build profile";
    }
#endif
#if !VIPER_CAP_AI
    if (len >= 3 && memcmp(name, "ai_", 3) == 0) {
        return "ai capability is disabled in this build profile";
    }
#endif
#if !VIPER_CAP_FS
    if (len >= 3 && memcmp(name, "fs_", 3) == 0) {
        return "fs capability is disabled in this build profile";
    }
#endif
#if !VIPER_CAP_OS
    if (len >= 3 && memcmp(name, "os_", 3) == 0) {
        return "os capability is disabled in this build profile";
    }
#endif
#if !VIPER_CAP_CACHE
    if (len >= 6 && memcmp(name, "cache_", 6) == 0) {
        return "cache capability is disabled in this build profile";
    }
#endif
#if !VIPER_CAP_UTIL
    if ((len >= 5 && memcmp(name, "time_", 5) == 0) ||
        (len >= 5 && memcmp(name, "math_", 5) == 0) ||
        (len >= 5 && memcmp(name, "text_", 5) == 0) ||
        (len >= 4 && memcmp(name, "arr_", 4) == 0)) {
        return "util capability is disabled in this build profile";
    }
#endif
#if !VIPER_CAP_META
    if (len >= 5 && memcmp(name, "meta_", 5) == 0) {
        return "meta capability is disabled in this build profile";
    }
#endif
    return NULL;
}

static void compiler_error_disabled_native(const char* name, int len, int native_idx) {
    const char* cap = native_capability(native_idx);
    if (!cap) cap = "unknown";
    compiler_error_exit("VCP001", "Native '%.*s' is disabled (capability=%s, profile=%s)",
                        len, name, cap, VIPER_PROFILE_NAME);
}

static void copy_path(char* dst, size_t dst_size, const char* src) {
    if (dst_size == 0) return;
    snprintf(dst, dst_size, "%s", src ? src : "");
}

static void compiler_fatal_oom(void) {
    compiler_error_exit("VCP002", "Out of memory.");
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

static bool dir_looks_like_project_root(const char* dir) {
    char stdlib_dir[MAX_PATH_LEN];
    char src_dir[MAX_PATH_LEN];
    char makefile[MAX_PATH_LEN];
    return join_path2(dir, "lib/std", stdlib_dir, sizeof(stdlib_dir)) && file_exists(stdlib_dir) &&
           join_path2(dir, "src", src_dir, sizeof(src_dir)) && file_exists(src_dir) &&
           join_path2(dir, "Makefile", makefile, sizeof(makefile)) && file_exists(makefile);
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

static char* unescape_string(const char* src, int len, int* out_len) {
    char* res = malloc(len + 1);
    int j = 0;
    for (int i = 0; i < len; i++) {
        if (src[i] == '\\' && i + 1 < len) {
            switch (src[i+1]) {
                case 'n': res[j++] = '\n'; i++; break;
                case 'r': res[j++] = '\r'; i++; break;
                case 't': res[j++] = '\t'; i++; break;
                case '"': res[j++] = '"';  i++; break;
                case '\\': res[j++] = '\\'; i++; break;
                default: res[j++] = src[i]; break;
            }
        } else {
            res[j++] = src[i];
        }
    }
    res[j] = '\0';
    if (out_len) *out_len = j;
    return res;
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
    char fallback_root[MAX_PATH_LEN];
    fallback_root[0] = '\0';

    if (entry_dir_path[0] != '\0') {
        copy_path(current, sizeof(current), entry_dir_path);
    } else {
        if (getcwd(current, sizeof(current)) == NULL) {
            copy_path(current, sizeof(current), ".");
        }
    }

    while (1) {
        char manifest[MAX_PATH_LEN];
        if (!join_path2(current, "viper.vpmod", manifest, sizeof(manifest))) break;
        if (file_exists(manifest)) {
            copy_path(project_root_path, sizeof(project_root_path), current);
            return;
        }

        if (fallback_root[0] == '\0' && dir_looks_like_project_root(current)) {
            copy_path(fallback_root, sizeof(fallback_root), current);
        }

        if (strcmp(current, "/") == 0 || strcmp(current, ".") == 0) break;

        char parent[MAX_PATH_LEN];
        path_dirname(current, parent, sizeof(parent));
        if (strcmp(parent, current) == 0) break;
        copy_path(current, sizeof(current), parent);
    }

    if (fallback_root[0] != '\0') {
        copy_path(project_root_path, sizeof(project_root_path), fallback_root);
    } else if (entry_dir_path[0] != '\0') {
        copy_path(project_root_path, sizeof(project_root_path), entry_dir_path);
    } else if (getcwd(project_root_path, sizeof(project_root_path)) == NULL) {
        copy_path(project_root_path, sizeof(project_root_path), ".");
    }
}

static void resolve_package_path(const char* raw_path, char* out, size_t out_size) {
    if (strstr(raw_path, "..") != NULL) {
        compiler_error_exit("VCP003", "Package import cannot contain '..': %s", raw_path);
    }

    // Special case for standard library.
    if (strncmp(raw_path, "@std/", 5) == 0) {
        const char* subpath = raw_path + 5;
        char filename[MAX_PATH_LEN];
        snprintf(filename, sizeof(filename), "%s.vp", subpath);

        // Try 1: VIPER_STD_PATH environment variable
        const char* env_std = getenv("VIPER_STD_PATH");
        if (env_std && env_std[0] != '\0') {
            if (join_path2(env_std, filename, out, out_size) && module_source_exists(out)) {
                return;
            }
        }

        // Try 2: project_root/lib/std/<module>.vp (development)
        char dev_path[MAX_PATH_LEN];
        if (join_path2(project_root_path, "lib/std", dev_path, sizeof(dev_path)) &&
            join_path2(dev_path, filename, out, out_size) &&
            module_source_exists(out)) {
            return;
        }

        // Try 3: /usr/local/lib/viper/std/<module>.vp (system)
        char sys_path[MAX_PATH_LEN];
        if (join_path2("/usr/local/lib/viper/std", filename, sys_path, sizeof(sys_path)) &&
            module_source_exists(sys_path)) {
            copy_path(out, out_size, sys_path);
            return;
        }

        compiler_error("VCP004", "Standard library module not found: %s", raw_path);
        if (env_std) fprintf(stderr, "  Checked VIPER_STD_PATH: %s\n", env_std);
        fprintf(stderr, "  Checked Dev Path: %s/lib/std/%s\n", project_root_path, filename);
        fprintf(stderr, "  Checked Sys Path: /usr/local/lib/viper/std/%s\n", filename);
        exit(1);
    }

    const char* slash = strchr(raw_path, '/');
    char package_name[MAX_PATH_LEN];
    const char* subpath = "index.vp";

    if (slash == NULL) {
        copy_path(package_name, sizeof(package_name), raw_path);
    } else {
        size_t pkg_len = (size_t)(slash - raw_path);
        if (pkg_len == 0 || pkg_len >= sizeof(package_name)) {
            compiler_error_exit("VCP005", "Invalid package import: %s", raw_path);
        }
        memcpy(package_name, raw_path, pkg_len);
        package_name[pkg_len] = '\0';
        if (slash[1] != '\0') subpath = slash + 1;
    }

    char path1[MAX_PATH_LEN];
    char package_root[MAX_PATH_LEN];
    if (!join_path2(project_root_path, ".viper/packages", path1, sizeof(path1)) ||
        !join_path2(path1, package_name, package_root, sizeof(package_root))) {
        compiler_error_exit("VCP006", "Package path is too long: %s", raw_path);
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
        compiler_error_exit("VCP006", "Package path is too long: %s", raw_path);
    }

    if (!module_source_exists(out)) {
        compiler_error_exit("VCP007",
                            "Package module not found for '%s' (resolved: %s). Run 'viper pkg install'.",
                            raw_path, out);
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
        compiler_error_exit("VCP008", "Module path is too long.");
    }
    memcpy(out, start, (size_t)length);
    out[length] = '\0';
}

static void resolve_module_path(const char* raw_path, char* out, size_t out_size) {
    if (strncmp(raw_path, "@std/", 5) == 0) {
        resolve_package_path(raw_path, out, out_size);
        return;
    }
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
            compiler_error_exit("VCP009", "Resolved module path is too long.");
        }
        memcpy(out, base, base_len);
        out[base_len] = '/';
        memcpy(out + base_len + 1, raw_path, raw_len);
        out[base_len + 1 + raw_len] = '\0';
    }
}

void compiler_set_entry_file(const char* path) {
    if (!path || path[0] == '\0') {
        entry_file_path[0] = '\0';
        copy_path(entry_dir_path, sizeof(entry_dir_path), ".");
        return;
    }

    char resolved[MAX_PATH_LEN];
    if (realpath(path, resolved) != NULL) {
        copy_path(entry_file_path, sizeof(entry_file_path), resolved);
    } else if (is_absolute_path(path)) {
        copy_path(entry_file_path, sizeof(entry_file_path), path);
    } else {
        char cwd[MAX_PATH_LEN];
        if (getcwd(cwd, sizeof(cwd)) != NULL &&
            join_path2(cwd, path, resolved, sizeof(resolved))) {
            copy_path(entry_file_path, sizeof(entry_file_path), resolved);
        } else {
            copy_path(entry_file_path, sizeof(entry_file_path), path);
        }
    }

    path_dirname(entry_file_path, entry_dir_path, sizeof(entry_dir_path));
}

const char* compiler_get_entry_file(void) {
    return entry_file_path;
}

const char* compiler_get_entry_dir(void) {
    return entry_dir_path;
}

const char* compiler_get_project_root(void) {
    return project_root_path;
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
    bool has_declared_type;
    StaticType type;
} GlobalVar;

static GlobalVar* global_vars = NULL;
static int global_var_count = 0;
static int global_var_cap = 0;

typedef unsigned int EffectMask;

typedef struct {
    ObjFunction* fn;
    int param_count;
    const char** param_names;
    int* param_name_lens;
    StaticType* param_types;
    char* param_name_storage;
    bool has_return_type;
    StaticType return_type;
    EffectMask declared_effects;
} FunctionTypeInfo;

static FunctionTypeInfo* fn_type_registry = NULL;
static int fn_type_count = 0;
static int fn_type_cap = 0;

typedef struct {
    Local* locals;
    int local_count;
    int local_cap;
    int next_reg;
    int scope_depth;
    ObjFunction* current_fn; // The function we are currently compiling into
    bool has_return_type;
    StaticType return_type;
    int return_type_ci;      // Constant index of the canonical return type string (-1 = untyped)
} CompilerState;

static CompilerState cst;

enum {
    EFFECT_OS      = 1u << 0,
    EFFECT_FS      = 1u << 1,
    EFFECT_WEB     = 1u << 2,
    EFFECT_DB      = 1u << 3,
    EFFECT_CACHE   = 1u << 4,
    EFFECT_AI      = 1u << 5,
    EFFECT_META    = 1u << 6,
    EFFECT_FFI     = 1u << 7,
    EFFECT_PANIC   = 1u << 8,
    EFFECT_ASYNC   = 1u << 9,
    EFFECT_DYNAMIC = 1u << 10
};

typedef struct {
    EffectMask bit;
    const char* name;
} EffectName;

static const EffectName kEffectNames[] = {
    {EFFECT_OS, "os"},
    {EFFECT_FS, "fs"},
    {EFFECT_WEB, "web"},
    {EFFECT_DB, "db"},
    {EFFECT_CACHE, "cache"},
    {EFFECT_AI, "ai"},
    {EFFECT_META, "meta"},
    {EFFECT_FFI, "ffi"},
    {EFFECT_PANIC, "panic"},
    {EFFECT_ASYNC, "async"},
    {EFFECT_DYNAMIC, "dynamic"},
};

static bool ensure_local_capacity(CompilerState* state, int needed) {
    return ensure_capacity((void**)&state->locals, &state->local_cap, needed, sizeof(Local));
}

static StaticType static_type_make(StaticTypeKind kind, ObjStruct* struct_obj) {
    StaticType type;
    type.kind = kind;
    type.struct_obj = struct_obj;
    return type;
}

static StaticType static_type_unknown(void) {
    return static_type_make(STATIC_TYPE_UNKNOWN, NULL);
}

static bool static_type_is_known(StaticType type) {
    return type.kind != STATIC_TYPE_UNKNOWN;
}

static void add_local_typed(const char* name, int length, int reg, int depth,
                            bool has_declared_type, StaticType type) {
    if (!ensure_local_capacity(&cst, cst.local_count + 1)) {
        compiler_fatal_oom();
    }
    Local* local = &cst.locals[cst.local_count++];
    local->name = name;
    local->length = length;
    local->reg = reg;
    local->depth = depth;
    local->has_declared_type = has_declared_type;
    local->type = type;
}

static void add_local(const char* name, int length, int reg, int depth) {
    add_local_typed(name, length, reg, depth, false, static_type_unknown());
}

static void register_function_symbol(ObjFunction* fn) {
    if (!ensure_capacity((void**)&fn_registry, &fn_cap, fn_count + 1, sizeof(ObjFunction*))) {
        compiler_fatal_oom();
    }
    fn_registry[fn_count++] = fn;
}

static void free_function_type_registry(void) {
    if (!fn_type_registry) return;
    for (int i = 0; i < fn_type_count; i++) {
        free(fn_type_registry[i].param_name_storage);
        free(fn_type_registry[i].param_names);
        free(fn_type_registry[i].param_name_lens);
        free(fn_type_registry[i].param_types);
        fn_type_registry[i].param_name_storage = NULL;
        fn_type_registry[i].param_names = NULL;
        fn_type_registry[i].param_name_lens = NULL;
        fn_type_registry[i].param_types = NULL;
        fn_type_registry[i].param_count = 0;
    }
    free(fn_type_registry);
    fn_type_registry = NULL;
    fn_type_count = 0;
    fn_type_cap = 0;
}

static void register_struct_symbol(ObjStruct* st) {
    if (!ensure_capacity((void**)&st_registry, &st_cap, st_count + 1, sizeof(ObjStruct*))) {
        compiler_fatal_oom();
    }
    st_registry[st_count++] = st;
}

static FunctionTypeInfo* find_function_type_info(ObjFunction* fn) {
    if (!fn) return NULL;
    for (int i = 0; i < fn_type_count; i++) {
        if (fn_type_registry[i].fn == fn) return &fn_type_registry[i];
    }
    return NULL;
}

static FunctionTypeInfo* ensure_function_type_info(ObjFunction* fn) {
    FunctionTypeInfo* info = find_function_type_info(fn);
    if (info) return info;

    if (!ensure_capacity((void**)&fn_type_registry, &fn_type_cap,
                         fn_type_count + 1, sizeof(FunctionTypeInfo))) {
        compiler_fatal_oom();
    }

    info = &fn_type_registry[fn_type_count++];
    info->fn = fn;
    info->param_count = 0;
    info->param_names = NULL;
    info->param_name_lens = NULL;
    info->param_types = NULL;
    info->param_name_storage = NULL;
    info->has_return_type = false;
    info->return_type = static_type_unknown();
    info->declared_effects = 0;
    return info;
}

static void clear_function_param_contract(FunctionTypeInfo* info) {
    if (!info) return;
    free(info->param_name_storage);
    free(info->param_names);
    free(info->param_name_lens);
    free(info->param_types);
    info->param_name_storage = NULL;
    info->param_names = NULL;
    info->param_name_lens = NULL;
    info->param_types = NULL;
    info->param_count = 0;
}

static void clear_namespace_registry(void) {
    ns_count = 0;
}

static int find_namespace_entry(const char* alias, int alias_len,
                                int kind, const char* name, int name_len) {
    if (!name || name_len <= 0) return -1;
    // Normalize NULL alias to empty string for consistent comparison
    if (!alias) { alias = ""; alias_len = 0; }
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
    if (!name || name_len <= 0 || !obj) return false;
    if (!alias) { alias = ""; alias_len = 0; }
    
    if (alias_len >= MAX_NAMESPACE_ALIAS_LEN) {
        compiler_error("VCP010", "Namespace alias too long for import '%s'.", module_path);
        return false;
    }

    int existing = find_namespace_entry(alias, alias_len, kind, name, name_len);
    if (existing >= 0) {
        if (ns_registry[existing].obj != obj) {
            compiler_error("VCP011", "Namespaced symbol conflict for '%.*s.%.*s'.",
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

static void register_global_symbol(const char* name, int length,
                                   bool has_declared_type, StaticType type) {
    if (!ensure_capacity((void**)&global_vars, &global_var_cap,
                         global_var_count + 1, sizeof(GlobalVar))) {
        compiler_fatal_oom();
    }
    global_vars[global_var_count].name = name;
    global_vars[global_var_count].length = length;
    global_vars[global_var_count].has_declared_type = has_declared_type;
    global_vars[global_var_count].type = has_declared_type ? type : static_type_unknown();
    global_var_count++;
}

static void init_compiler(ObjFunction* fn) {
    cst.locals = NULL;
    cst.local_count = 0;
    cst.local_cap = 0;
    cst.next_reg = 1;       // R0 is reserved for call bookkeeping
    cst.scope_depth = 0;
    cst.current_fn = fn;
    cst.has_return_type = false;
    cst.return_type = static_type_unknown();
    cst.return_type_ci = -1; // No return type by default
}

static Chunk* current_chunk() {
    return &cst.current_fn->chunk;
}

static int resolve_local_index(const char* name, int length) {
    for (int i = cst.local_count - 1; i >= 0; i--) {
        if (cst.locals[i].length == length &&
            memcmp(cst.locals[i].name, name, length) == 0) {
            return i;
        }
    }
    return -1;
}

static int resolve_local(const char* name, int length) {
    int idx = resolve_local_index(name, length);
    return idx >= 0 ? cst.locals[idx].reg : -1;
}

static int resolve_global_index(const char* name, int length) {
    for (int i = global_var_count - 1; i >= 0; i--) {
        if (global_vars[i].length == length &&
            memcmp(global_vars[i].name, name, (size_t)length) == 0) {
            return i;
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

static bool type_name_matches(const char* name, int length, const char* expected) {
    return name && expected &&
           (int)strlen(expected) == length &&
           memcmp(name, expected, (size_t)length) == 0;
}

static StaticType resolve_type_annotation_in_namespace(Token token,
                                                       const char* alias, int alias_len) {
    if (token.length <= 0) return static_type_unknown();

    switch (token.type) {
        case TOKEN_TYPE_ANY:
            return static_type_make(STATIC_TYPE_ANY, NULL);
        case TOKEN_TYPE_I:
        case TOKEN_TYPE_F:
        case TOKEN_TYPE_C:
        case TOKEN_TYPE_U8:
            return static_type_make(STATIC_TYPE_NUMBER, NULL);
        case TOKEN_TYPE_B:
            return static_type_make(STATIC_TYPE_BOOL, NULL);
        case TOKEN_TYPE_S:
            return static_type_make(STATIC_TYPE_STRING, NULL);
        default:
            break;
    }

    if (type_name_matches(token.start, token.length, "any")) {
        return static_type_make(STATIC_TYPE_ANY, NULL);
    }
    if (type_name_matches(token.start, token.length, "int") ||
        type_name_matches(token.start, token.length, "float") ||
        type_name_matches(token.start, token.length, "number") ||
        type_name_matches(token.start, token.length, "char") ||
        type_name_matches(token.start, token.length, "u8") ||
        type_name_matches(token.start, token.length, "i") ||
        type_name_matches(token.start, token.length, "f") ||
        type_name_matches(token.start, token.length, "c")) {
        return static_type_make(STATIC_TYPE_NUMBER, NULL);
    }
    if (type_name_matches(token.start, token.length, "bool") ||
        type_name_matches(token.start, token.length, "b")) {
        return static_type_make(STATIC_TYPE_BOOL, NULL);
    }
    if (type_name_matches(token.start, token.length, "str") ||
        type_name_matches(token.start, token.length, "string") ||
        type_name_matches(token.start, token.length, "s")) {
        return static_type_make(STATIC_TYPE_STRING, NULL);
    }
    if (type_name_matches(token.start, token.length, "array")) {
        return static_type_make(STATIC_TYPE_ARRAY, NULL);
    }
    if (type_name_matches(token.start, token.length, "fn") ||
        type_name_matches(token.start, token.length, "function")) {
        return static_type_make(STATIC_TYPE_FUNCTION, NULL);
    }
    if (type_name_matches(token.start, token.length, "struct")) {
        return static_type_make(STATIC_TYPE_STRUCT_INSTANCE, NULL);
    }

    ObjStruct* st = resolve_namespace_struct(alias, alias_len, token.start, token.length);
    if (!st) st = resolve_struct(token.start, token.length);
    if (st) return static_type_make(STATIC_TYPE_STRUCT_INSTANCE, st);

    compiler_error_exit("VCP012", "Unknown type annotation '%.*s'.", token.length, token.start);
    return static_type_unknown();
}

static StaticType resolve_type_annotation(Token token) {
    return resolve_type_annotation_in_namespace(token, current_module_alias, current_alias_len);
}

static StaticType resolve_type_annotation_text_in_namespace(const char* text,
                                                            const char* alias, int alias_len) {
    if (!text || text[0] == '\0' || strcmp(text, "-") == 0) {
        return static_type_unknown();
    }
    Token token = {0};
    token.type = TOKEN_IDENTIFIER;
    token.start = text;
    token.length = (int)strlen(text);
    return resolve_type_annotation_in_namespace(token, alias, alias_len);
}

static bool static_types_equal(StaticType a, StaticType b) {
    return a.kind == b.kind && a.struct_obj == b.struct_obj;
}

static bool static_type_is_compatible(StaticType actual, StaticType expected) {
    if (expected.kind == STATIC_TYPE_ANY) return true;
    if (actual.kind == STATIC_TYPE_UNKNOWN || actual.kind == STATIC_TYPE_ANY) return true;
    if (actual.kind != expected.kind) return false;
    if (expected.kind == STATIC_TYPE_STRUCT_INSTANCE ||
        expected.kind == STATIC_TYPE_STRUCT_DEF) {
        return expected.struct_obj == NULL ||
               actual.struct_obj == NULL ||
               actual.struct_obj == expected.struct_obj;
    }
    return true;
}

static void format_static_type(StaticType type, char* out, size_t out_size) {
    const char* builtin = "unknown";

    switch (type.kind) {
        case STATIC_TYPE_ANY: builtin = "any"; break;
        case STATIC_TYPE_NUMBER: builtin = "int"; break;
        case STATIC_TYPE_BOOL: builtin = "bool"; break;
        case STATIC_TYPE_STRING: builtin = "str"; break;
        case STATIC_TYPE_ARRAY: builtin = "array"; break;
        case STATIC_TYPE_FUNCTION: builtin = "fn"; break;
        case STATIC_TYPE_NIL: builtin = "nil"; break;
        case STATIC_TYPE_STRUCT_DEF:
        case STATIC_TYPE_STRUCT_INSTANCE:
            if (type.struct_obj && type.struct_obj->name) {
                snprintf(out, out_size, "%.*s",
                         type.struct_obj->name_len, type.struct_obj->name);
                return;
            }
            builtin = "struct";
            break;
        case STATIC_TYPE_UNKNOWN:
        default:
            builtin = "unknown";
            break;
    }

    snprintf(out, out_size, "%s", builtin);
}

static int add_type_constant(StaticType type) {
    char type_name[MAX_ABI_NAME_LEN];
    format_static_type(type, type_name, sizeof(type_name));
    ObjString* type_str = copy_string(type_name, (int)strlen(type_name));
    return add_constant(current_chunk(), (Value){VAL_OBJ, {.obj = (Obj*)type_str}});
}

static void emit_type_assert(int reg, StaticType type) {
    if (!static_type_is_known(type)) return;
    int ci = add_type_constant(type);
    write_chunk(current_chunk(), ENCODE_INST(OP_ASSERT_TYPE, reg, ci, 0));
}

static void compiler_type_mismatch(const char* context,
                                   const char* name, int name_len,
                                   StaticType expected, StaticType actual) {
    char expected_name[MAX_ABI_NAME_LEN];
    char actual_name[MAX_ABI_NAME_LEN];
    format_static_type(expected, expected_name, sizeof(expected_name));
    format_static_type(actual, actual_name, sizeof(actual_name));

    if (name && name_len > 0) {
        compiler_error("VCP013", "Type mismatch for %s '%.*s': expected '%s', got '%s'.",
                       context, name_len, name, expected_name, actual_name);
    } else {
        compiler_error("VCP013", "Type mismatch for %s: expected '%s', got '%s'.",
                       context, expected_name, actual_name);
    }
    exit(1);
}

static bool token_matches_cstr(Token token, const char* text) {
    size_t len = strlen(text);
    return token.length == (int)len && memcmp(token.start, text, len) == 0;
}

static EffectMask effect_mask_from_name(const char* name, int len) {
    if (!name || len <= 0) return 0;
    for (size_t i = 0; i < sizeof(kEffectNames) / sizeof(kEffectNames[0]); i++) {
        if ((int)strlen(kEffectNames[i].name) == len &&
            memcmp(name, kEffectNames[i].name, (size_t)len) == 0) {
            return kEffectNames[i].bit;
        }
    }
    return 0;
}

static EffectMask declared_effect_mask(Token* effects, int effect_count) {
    EffectMask mask = 0;
    for (int i = 0; i < effect_count; i++) {
        mask |= effect_mask_from_name(effects[i].start, effects[i].length);
    }
    return mask;
}

static void format_effect_mask(EffectMask mask, char* out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';

    size_t offset = 0;
    for (size_t i = 0; i < sizeof(kEffectNames) / sizeof(kEffectNames[0]); i++) {
        if ((mask & kEffectNames[i].bit) == 0) continue;
        int written = snprintf(out + offset, out_size - offset, "%s%s",
                               offset > 0 ? "," : "", kEffectNames[i].name);
        if (written < 0 || (size_t)written >= out_size - offset) {
            out[out_size - 1] = '\0';
            return;
        }
        offset += (size_t)written;
    }
    if (offset == 0) {
        snprintf(out, out_size, "-");
    }
}

static void compiler_effect_mismatch(Token fn_name, EffectMask missing_mask) {
    char missing[256];
    format_effect_mask(missing_mask, missing, sizeof(missing));
    compiler_error_exit("VCP014",
                        "Effect mismatch for function '%.*s' on line %d: missing declared effects for inferred '%s'.",
                        fn_name.length, fn_name.start, fn_name.line, missing);
}

static EffectMask infer_expr_effect_mask(AstNode* expr);
static EffectMask infer_stmt_effect_mask(AstNode* stmt, bool include_nested_functions);

static EffectMask infer_call_effect_mask(AstNode* expr) {
    if (!expr || expr->type != AST_CALL_EXPR) return 0;

    Token name = expr->data.call_expr.name;
    const char* qualifier = NULL;
    int qualifier_len = 0;
    AstNode* callee = expr->data.call_expr.callee;
    ObjFunction* callee_fn = NULL;
    if (callee && callee->type == AST_GET_EXPR &&
        callee->data.get_expr.obj &&
        callee->data.get_expr.obj->type == AST_IDENTIFIER) {
        qualifier = callee->data.get_expr.obj->data.identifier.name.start;
        qualifier_len = callee->data.get_expr.obj->data.identifier.name.length;
        callee_fn = resolve_namespace_function(qualifier, qualifier_len, name.start, name.length);
    } else if (callee && callee->type == AST_IDENTIFIER) {
        callee_fn = resolve_namespace_function(current_module_alias, current_alias_len,
                                              name.start, name.length);
        if (!callee_fn) callee_fn = resolve_namespace_function(NULL, 0, name.start, name.length);
        if (!callee_fn) callee_fn = resolve_function(name.start, name.length);
    }

    if (callee_fn) {
        FunctionTypeInfo* info = find_function_type_info(callee_fn);
        if (info && info->declared_effects != 0) return info->declared_effects;
    }

    if (qualifier && qualifier_len > 0) {
        if (qualifier_len == 2 && memcmp(qualifier, "os", 2) == 0) return EFFECT_OS;
        if (qualifier_len == 2 && memcmp(qualifier, "io", 2) == 0) return EFFECT_FS;
        if (qualifier_len == 3 && memcmp(qualifier, "web", 3) == 0) return EFFECT_WEB;
        if (qualifier_len == 2 && memcmp(qualifier, "db", 2) == 0) return EFFECT_DB;
        if (qualifier_len == 5 && memcmp(qualifier, "cache", 5) == 0) return EFFECT_CACHE;
        if (qualifier_len == 2 && memcmp(qualifier, "ai", 2) == 0) return EFFECT_AI;
        if (qualifier_len == 4 && memcmp(qualifier, "meta", 4) == 0) return EFFECT_META;
    }

    if (name.length >= 3 && memcmp(name.start, "os_", 3) == 0) return EFFECT_OS;
    if (name.length >= 3 && memcmp(name.start, "fs_", 3) == 0) return EFFECT_FS;
    if ((name.length >= 4 && memcmp(name.start, "web_", 4) == 0) ||
        token_matches_cstr(name, "serve") ||
        token_matches_cstr(name, "fetch")) {
        return EFFECT_WEB;
    }
    if ((name.length >= 4 && memcmp(name.start, "vdb_", 4) == 0) ||
        token_matches_cstr(name, "query")) {
        return EFFECT_DB;
    }
    if (name.length >= 3 && memcmp(name.start, "ai_", 3) == 0) return EFFECT_AI;
    if (name.length >= 6 && memcmp(name.start, "cache_", 6) == 0) return EFFECT_CACHE;
    if (name.length >= 5 && memcmp(name.start, "meta_", 5) == 0) return EFFECT_META;
    if (token_matches_cstr(name, "load_dl") || token_matches_cstr(name, "get_fn")) return EFFECT_FFI;
    if (token_matches_cstr(name, "panic") || token_matches_cstr(name, "recover")) return EFFECT_PANIC;
    return 0;
}

static EffectMask infer_expr_effect_mask(AstNode* expr) {
    if (!expr) return 0;

    switch (expr->type) {
        case AST_CALL_EXPR: {
            EffectMask mask = infer_call_effect_mask(expr);
            mask |= infer_expr_effect_mask(expr->data.call_expr.callee);
            for (int i = 0; i < expr->data.call_expr.arg_count; i++) {
                mask |= infer_expr_effect_mask(expr->data.call_expr.args[i]);
            }
            return mask;
        }
        case AST_BINARY_EXPR:
            return infer_expr_effect_mask(expr->data.binary.left) |
                   infer_expr_effect_mask(expr->data.binary.right);
        case AST_ASSIGN_EXPR:
            return infer_expr_effect_mask(expr->data.assign.value);
        case AST_GET_EXPR:
        case AST_SAFE_GET_EXPR:
            return infer_expr_effect_mask(expr->data.get_expr.obj);
        case AST_SET_EXPR:
            return infer_expr_effect_mask(expr->data.set_expr.obj) |
                   infer_expr_effect_mask(expr->data.set_expr.value);
        case AST_INDEX_EXPR:
            return infer_expr_effect_mask(expr->data.index_expr.target) |
                   infer_expr_effect_mask(expr->data.index_expr.index) |
                   infer_expr_effect_mask(expr->data.index_expr.value);
        case AST_ARRAY_EXPR: {
            EffectMask mask = 0;
            for (int i = 0; i < expr->data.array_expr.count; i++) {
                mask |= infer_expr_effect_mask(expr->data.array_expr.elements[i]);
            }
            return mask;
        }
        case AST_MATCH_EXPR:
            return infer_expr_effect_mask(expr->data.match_expr.left) |
                   infer_expr_effect_mask(expr->data.match_expr.right);
        case AST_SPAWN_EXPR:
            return EFFECT_ASYNC | infer_expr_effect_mask(expr->data.spawn_expr.expr);
        case AST_AWAIT_EXPR:
            return EFFECT_ASYNC | infer_expr_effect_mask(expr->data.await_expr.expr);
        case AST_TRY_EXPR:
            return infer_expr_effect_mask(expr->data.try_expr.try_block) |
                   infer_expr_effect_mask(expr->data.try_expr.catch_block);
        case AST_TYPEOF_EXPR:
            return infer_expr_effect_mask(expr->data.typeof_expr.expr);
        case AST_CLONE_EXPR:
        case AST_KEYS_EXPR:
            return infer_expr_effect_mask(expr->data.clone_expr.expr);
        case AST_EVAL_EXPR:
            return EFFECT_DYNAMIC | infer_expr_effect_mask(expr->data.clone_expr.expr);
        case AST_HAS_EXPR:
            return infer_expr_effect_mask(expr->data.has_expr.obj) |
                   infer_expr_effect_mask(expr->data.has_expr.prop);
        case AST_ERROR_PROPAGATE_EXPR:
            return infer_expr_effect_mask(expr->data.error_propagate.expr);
        default:
            return 0;
    }
}

static EffectMask infer_stmt_effect_mask(AstNode* stmt, bool include_nested_functions) {
    if (!stmt) return 0;

    switch (stmt->type) {
        case AST_PROGRAM:
        case AST_BLOCK_STMT: {
            EffectMask mask = 0;
            for (int i = 0; i < stmt->data.block.count; i++) {
                mask |= infer_stmt_effect_mask(stmt->data.block.statements[i], include_nested_functions);
            }
            return mask;
        }
        case AST_VAR_DECL:
            return infer_expr_effect_mask(stmt->data.var_decl.initializer);
        case AST_EXPR_STMT:
            return infer_expr_effect_mask(stmt->data.expr_stmt.expr);
        case AST_IF_STMT:
            return infer_expr_effect_mask(stmt->data.if_stmt.condition) |
                   infer_stmt_effect_mask(stmt->data.if_stmt.then_branch, include_nested_functions) |
                   infer_stmt_effect_mask(stmt->data.if_stmt.else_branch, include_nested_functions);
        case AST_WHILE_STMT:
            return infer_expr_effect_mask(stmt->data.while_stmt.condition) |
                   infer_stmt_effect_mask(stmt->data.while_stmt.body, include_nested_functions);
        case AST_RETURN_STMT:
            return infer_expr_effect_mask(stmt->data.return_stmt.value);
        case AST_SYNC_STMT:
            return EFFECT_ASYNC |
                   infer_stmt_effect_mask(stmt->data.sync_stmt.body, include_nested_functions);
        case AST_FUNC_DECL:
            if (!include_nested_functions) return 0;
            return infer_stmt_effect_mask(stmt->data.func_decl.body, true);
        default:
            return 0;
    }
}

static void verify_function_declared_effects(AstNode* fn_decl) {
    if (!fn_decl || fn_decl->type != AST_FUNC_DECL || fn_decl->data.func_decl.effect_count == 0) return;

    EffectMask declared = declared_effect_mask(fn_decl->data.func_decl.effects,
                                               fn_decl->data.func_decl.effect_count);
    EffectMask inferred = infer_stmt_effect_mask(fn_decl->data.func_decl.body, true);
    EffectMask missing = inferred & ~declared;
    if (missing != 0) {
        compiler_effect_mismatch(fn_decl->data.func_decl.name, missing);
    }
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
    char params[MAX_ABI_PARAMS_LEN];
    char return_type[MAX_ABI_NAME_LEN];
    EffectMask effects;
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

static int count_abi_params(const char* text);
static bool validate_abi_params_token(const char* text, int metric);

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
                              const char* params, const char* return_type, EffectMask effects,
                              const char* abi_path, int line_no) {
    if (!symbols || !count || !cap || !name) return false;
    if (metric < 0 || metric > 255) {
        compiler_error("VCP015", "Invalid ABI metric on %s:%d (%s %s %d).",
                       abi_path, line_no, (kind == ABI_KIND_FUNCTION ? "fn" : "st"), name, metric);
        return false;
    }
    if (kind == ABI_KIND_FUNCTION &&
        !validate_abi_params_token(params ? params : "", metric)) {
        compiler_error("VCP045", "Invalid ABI params field on %s:%d.", abi_path, line_no);
        return false;
    }

    int existing = find_abi_symbol(*symbols, *count, kind, name);
    if (existing >= 0) {
        if ((*symbols)[existing].metric != metric ||
            strcmp((*symbols)[existing].params, params ? params : "") != 0 ||
            strcmp((*symbols)[existing].return_type, return_type ? return_type : "") != 0 ||
            (*symbols)[existing].effects != effects) {
            compiler_error("VCP016", "Conflicting ABI entries on %s:%d for %s '%s'.",
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
    copy_path(out->params, sizeof(out->params), params ? params : "");
    copy_path(out->return_type, sizeof(out->return_type), return_type ? return_type : "");
    out->effects = effects;
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

static bool parse_abi_effects_token(const char* text, EffectMask* out_mask) {
    if (!out_mask) return false;
    *out_mask = 0;
    if (!text || text[0] == '\0' || strcmp(text, "-") == 0) return true;

    const char* start = text;
    while (*start) {
        const char* comma = strchr(start, ',');
        size_t len = comma ? (size_t)(comma - start) : strlen(start);
        if (len == 0 || len >= MAX_ABI_NAME_LEN) return false;
        EffectMask bit = effect_mask_from_name(start, (int)len);
        if (bit == 0) return false;
        *out_mask |= bit;
        if (!comma) break;
        start = comma + 1;
    }
    return true;
}

static int count_abi_params(const char* text) {
    if (!text || text[0] == '\0') return -1;
    if (strcmp(text, "-") == 0) return 0;

    int count = 0;
    const char* p = text;
    while (*p) {
        const char* name_start = p;
        while (*p && *p != ':' && *p != ',') p++;
        size_t name_len = (size_t)(p - name_start);
        if (name_len == 0 || name_len >= MAX_ABI_NAME_LEN || *p != ':') return -1;
        p++;

        const char* type_start = p;
        while (*p && *p != ',') p++;
        size_t type_len = (size_t)(p - type_start);
        if (type_len == 0 || type_len >= MAX_ABI_NAME_LEN) return -1;
        count++;

        if (*p == ',') {
            p++;
            if (*p == '\0') return -1;
        }
    }

    return count;
}

static bool validate_abi_params_token(const char* text, int metric) {
    if (!text || text[0] == '\0') return true;
    int count = count_abi_params(text);
    return count >= 0 && count == metric;
}

static bool load_module_abi_symbols(const char* abi_path, AbiSymbol** out_symbols, int* out_count) {
    *out_symbols = NULL;
    *out_count = 0;
    int cap = 0;

    FILE* file = fopen(abi_path, "r");
    if (!file) return false;

    bool ok = true;
    int line_no = 0;
    char line[1024];
    while (fgets(line, sizeof(line), file) != NULL) {
        line_no++;

        char* cursor = line;
        while (*cursor && isspace((unsigned char)*cursor)) cursor++;
        if (*cursor == '\0' || *cursor == '#') continue;

        char* kind_tok = strtok(cursor, " \t\r\n");
        char* name_tok = strtok(NULL, " \t\r\n");
        char* metric_tok = strtok(NULL, " \t\r\n");

        if (!kind_tok || !name_tok || !metric_tok) {
            compiler_error("VCP017", "Invalid ABI entry on %s:%d.", abi_path, line_no);
            ok = false;
            break;
        }

        int kind = 0;
        if (strcmp(kind_tok, "fn") == 0) {
            kind = ABI_KIND_FUNCTION;
        } else if (strcmp(kind_tok, "st") == 0) {
            kind = ABI_KIND_STRUCT;
        } else {
            compiler_error("VCP018", "Invalid ABI symbol kind on %s:%d: %s",
                           abi_path, line_no, kind_tok);
            ok = false;
            break;
        }

        size_t name_len = strlen(name_tok);
        if (name_len == 0 || name_len >= MAX_ABI_NAME_LEN) {
            compiler_error("VCP019", "Invalid ABI symbol name on %s:%d.", abi_path, line_no);
            ok = false;
            break;
        }

        int metric = 0;
        if (!parse_metric_token(metric_tok, &metric)) {
            compiler_error("VCP015", "Invalid ABI metric on %s:%d.", abi_path, line_no);
            ok = false;
            break;
        }

        char params[MAX_ABI_PARAMS_LEN];
        char return_type[MAX_ABI_NAME_LEN];
        params[0] = '\0';
        return_type[0] = '\0';
        EffectMask effects = 0;
        bool seen_params = false;
        bool seen_ret = false;
        bool seen_eff = false;
        char* extra_tok = NULL;
        while ((extra_tok = strtok(NULL, " \t\r\n")) != NULL) {
            if (strncmp(extra_tok, "params=", 7) == 0) {
                if (seen_params) {
                    compiler_error("VCP044", "Duplicate ABI params field on %s:%d.", abi_path, line_no);
                    ok = false;
                    break;
                }
                copy_path(params, sizeof(params), extra_tok + 7);
                if (!validate_abi_params_token(params, metric)) {
                    compiler_error("VCP045", "Invalid ABI params field on %s:%d.", abi_path, line_no);
                    ok = false;
                    break;
                }
                seen_params = true;
            } else if (strncmp(extra_tok, "ret=", 4) == 0) {
                if (seen_ret) {
                    compiler_error("VCP020", "Duplicate ABI return type on %s:%d.", abi_path, line_no);
                    ok = false;
                    break;
                }
                copy_path(return_type, sizeof(return_type), extra_tok + 4);
                seen_ret = true;
            } else if (strncmp(extra_tok, "eff=", 4) == 0) {
                if (seen_eff || !parse_abi_effects_token(extra_tok + 4, &effects)) {
                    compiler_error("VCP021", "Invalid ABI effects on %s:%d.", abi_path, line_no);
                    ok = false;
                    break;
                }
                seen_eff = true;
            } else {
                compiler_error("VCP017", "Invalid ABI entry on %s:%d.", abi_path, line_no);
                ok = false;
                break;
            }
        }
        if (!ok) break;

        if (!append_abi_symbol(out_symbols, out_count, &cap, kind, name_tok, metric,
                               params,
                               return_type, effects, abi_path, line_no)) {
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

static void populate_imported_function_param_contract(FunctionTypeInfo* info,
                                                      const AbiSymbol* abi,
                                                      const char* namespace_alias, int alias_len,
                                                      const char* module_path) {
    if (!info || !abi) return;

    clear_function_param_contract(info);
    if (abi->params[0] == '\0') return;
    if (strcmp(abi->params, "-") == 0) return;

    int count = count_abi_params(abi->params);
    if (count < 0 || count != abi->metric) {
        compiler_error("VCP045", "Invalid ABI params field for package module '%s': function '%s'.",
                       module_path, abi->name);
        exit(1);
    }
    if (count == 0) return;

    info->param_names = (const char**)calloc((size_t)count, sizeof(const char*));
    info->param_name_lens = (int*)calloc((size_t)count, sizeof(int));
    info->param_types = (StaticType*)calloc((size_t)count, sizeof(StaticType));
    if (!info->param_names || !info->param_name_lens || !info->param_types) {
        compiler_fatal_oom();
    }

    size_t total_name_bytes = 0;
    const char* scan = abi->params;
    while (*scan) {
        const char* colon = strchr(scan, ':');
        if (!colon) {
            compiler_error("VCP045", "Invalid ABI params field for package module '%s': function '%s'.",
                           module_path, abi->name);
            exit(1);
        }
        total_name_bytes += (size_t)(colon - scan) + 1;
        const char* comma = strchr(colon + 1, ',');
        if (!comma) break;
        scan = comma + 1;
    }

    info->param_name_storage = (char*)malloc(total_name_bytes > 0 ? total_name_bytes : 1);
    if (!info->param_name_storage) compiler_fatal_oom();

    info->param_count = count;
    char* storage_cursor = info->param_name_storage;
    const char* p = abi->params;
    for (int i = 0; i < count; i++) {
        const char* colon = strchr(p, ':');
        if (!colon) {
            compiler_error("VCP045", "Invalid ABI params field for package module '%s': function '%s'.",
                           module_path, abi->name);
            exit(1);
        }
        const char* comma = strchr(colon + 1, ',');
        const char* entry_end = comma ? comma : (abi->params + strlen(abi->params));

        int name_len = (int)(colon - p);
        int type_len = (int)(entry_end - (colon + 1));
        memcpy(storage_cursor, p, (size_t)name_len);
        storage_cursor[name_len] = '\0';
        info->param_names[i] = storage_cursor;
        info->param_name_lens[i] = name_len;
        storage_cursor += name_len + 1;

        if (type_len == 1 && colon[1] == '-') {
            info->param_types[i] = static_type_unknown();
        } else {
            char type_name[MAX_ABI_NAME_LEN];
            if (type_len <= 0 || type_len >= MAX_ABI_NAME_LEN) {
                compiler_error("VCP045", "Invalid ABI params field for package module '%s': function '%s'.",
                               module_path, abi->name);
                exit(1);
            }
            memcpy(type_name, colon + 1, (size_t)type_len);
            type_name[type_len] = '\0';
            info->param_types[i] = resolve_type_annotation_text_in_namespace(
                type_name,
                (namespace_alias && alias_len > 0) ? namespace_alias : NULL,
                (namespace_alias && alias_len > 0) ? alias_len : 0);
        }

        p = comma ? comma + 1 : entry_end;
    }
}

static bool append_module_symbol(ModuleSymbol** symbols, int* count, int* cap,
                                 int kind, const char* name, int name_len,
                                 int metric, Obj* obj, const char* module_path) {
    if (!symbols || !count || !cap || !name || name_len <= 0 || !obj) return false;

    int existing = find_module_symbol(*symbols, *count, kind, name, name_len);
    if (existing >= 0) {
        if ((*symbols)[existing].metric != metric || (*symbols)[existing].obj != obj) {
            compiler_error("VCP022", "Export collision in bytecode module '%s' for %s '%.*s'.",
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
            compiler_error("VCP023", "Import conflict for package '%s': function '%.*s' already exists.",
                           module_path, symbol->name_len, symbol->name);
            return false;
        }
        if (st) {
            compiler_error("VCP023", "Import conflict for package '%s': function '%.*s' collides with struct.",
                           module_path, symbol->name_len, symbol->name);
            return false;
        }
    } else {
        if (st) {
            compiler_error("VCP023", "Import conflict for package '%s': struct '%.*s' already exists.",
                           module_path, symbol->name_len, symbol->name);
            return false;
        }
        if (fn) {
            compiler_error("VCP023", "Import conflict for package '%s': struct '%.*s' collides with function.",
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
        compiler_error_exit("VCP024", "Could not read ABI file '%s'.", vabi_path);
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
            compiler_error("VCP025",
                           "ABI mismatch for package module '%s': missing %s '%s' in bytecode.",
                           resolved_module_path, abi_symbol_label(abi->kind), abi->name);
            free(module_symbols);
            free(abi_symbols);
            free(queue);
            exit(1);
        }

        ModuleSymbol* symbol = &module_symbols[found];
        if (symbol->metric != abi->metric) {
            compiler_error("VCP026",
                           "ABI mismatch for package module '%s': %s '%s' metric %d != %d.",
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

    for (int i = 0; i < abi_count; i++) {
        AbiSymbol* abi = &abi_symbols[i];
        if (abi->kind != ABI_KIND_FUNCTION) continue;

        int name_len = (int)strlen(abi->name);
        int found = find_module_symbol(module_symbols, symbol_count,
                                       abi->kind, abi->name, name_len);
        if (found < 0) continue;

        ObjFunction* fn = (ObjFunction*)module_symbols[found].obj;
        FunctionTypeInfo* info = ensure_function_type_info(fn);
        populate_imported_function_param_contract(info, abi, namespace_alias, alias_len,
                                                  resolved_module_path);
        info->declared_effects = abi->effects;
        if (abi->return_type[0] != '\0' && strcmp(abi->return_type, "-") != 0) {
            info->has_return_type = true;
            info->return_type = resolve_type_annotation_text_in_namespace(
                abi->return_type,
                (namespace_alias && alias_len > 0) ? namespace_alias : NULL,
                (namespace_alias && alias_len > 0) ? alias_len : 0);
        } else {
            info->has_return_type = false;
            info->return_type = static_type_unknown();
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

static StaticType infer_expr_type(AstNode* expr);

static StaticType infer_function_return_type(ObjFunction* fn) {
    FunctionTypeInfo* info = find_function_type_info(fn);
    if (!info || !info->has_return_type) return static_type_unknown();
    return info->return_type;
}

static void compiler_call_arity_mismatch(const char* fn_name, int fn_name_len,
                                         int expected, int actual) {
    compiler_error("VCP042", "Argument count mismatch for function '%.*s': expected %d, got %d.",
                   fn_name_len, fn_name, expected, actual);
    exit(1);
}

static void compiler_call_param_mismatch(Token fn_name, Token param_name,
                                         StaticType expected, StaticType actual) {
    char expected_name[MAX_ABI_NAME_LEN];
    char actual_name[MAX_ABI_NAME_LEN];
    format_static_type(expected, expected_name, sizeof(expected_name));
    format_static_type(actual, actual_name, sizeof(actual_name));
    compiler_error("VCP043",
                   "Type mismatch for parameter '%.*s' in function '%.*s': expected '%s', got '%s'.",
                   param_name.length, param_name.start,
                   fn_name.length, fn_name.start,
                   expected_name, actual_name);
    exit(1);
}

static void validate_static_call_contract(AstNode* call_expr, ObjFunction* callee_fn) {
    if (!call_expr || call_expr->type != AST_CALL_EXPR || !callee_fn) return;
    if (call_expr->data.call_expr.arg_count != callee_fn->arity) {
        compiler_call_arity_mismatch(call_expr->data.call_expr.name.start,
                                     call_expr->data.call_expr.name.length,
                                     callee_fn->arity,
                                     call_expr->data.call_expr.arg_count);
    }

    FunctionTypeInfo* info = find_function_type_info(callee_fn);
    if (!info || info->param_count <= 0 || !info->param_types) return;

    for (int i = 0; i < call_expr->data.call_expr.arg_count && i < info->param_count; i++) {
        StaticType expected = info->param_types[i];
        if (!static_type_is_known(expected)) continue;

        StaticType actual = infer_expr_type(call_expr->data.call_expr.args[i]);
        if (!static_type_is_known(actual)) continue;
        if (!static_type_is_compatible(actual, expected)) {
            Token param_name = {
                TOKEN_IDENTIFIER,
                (info->param_names && info->param_names[i]) ? info->param_names[i] : "",
                (info->param_name_lens && info->param_name_lens[i] > 0) ? info->param_name_lens[i] : 0,
                call_expr->data.call_expr.name.line
            };
            compiler_call_param_mismatch(call_expr->data.call_expr.name, param_name, expected, actual);
        }
    }
}

static StaticType infer_identifier_type(const char* name, int length) {
    int local_idx = resolve_local_index(name, length);
    if (local_idx >= 0 && static_type_is_known(cst.locals[local_idx].type)) {
        return cst.locals[local_idx].type;
    }

    int global_idx = resolve_global_index(name, length);
    if (global_idx >= 0 && static_type_is_known(global_vars[global_idx].type)) {
        return global_vars[global_idx].type;
    }

    ObjFunction* fn = resolve_namespace_function(current_module_alias, current_alias_len, name, length);
    if (!fn) fn = resolve_namespace_function(NULL, 0, name, length);
    if (!fn) fn = resolve_function(name, length);
    if (fn) return static_type_make(STATIC_TYPE_FUNCTION, NULL);

    ObjStruct* st = resolve_namespace_struct(current_module_alias, current_alias_len, name, length);
    if (!st) st = resolve_namespace_struct(NULL, 0, name, length);
    if (!st) st = resolve_struct(name, length);
    if (st) return static_type_make(STATIC_TYPE_STRUCT_DEF, st);

    if (find_native_index(name, length) != -1) {
        return static_type_make(STATIC_TYPE_FUNCTION, NULL);
    }

    return static_type_unknown();
}

static StaticType infer_call_expr_type(AstNode* expr) {
    AstNode* callee_expr = expr->data.call_expr.callee;
    const char* name = expr->data.call_expr.name.start;
    int namelen = expr->data.call_expr.name.length;
    ObjFunction* callee_fn = NULL;
    ObjStruct* callee_st = NULL;

    if (callee_expr && callee_expr->type == AST_GET_EXPR &&
        callee_expr->data.get_expr.obj &&
        callee_expr->data.get_expr.obj->type == AST_IDENTIFIER) {
        Token alias = callee_expr->data.get_expr.obj->data.identifier.name;
        Token member = callee_expr->data.get_expr.name;
        callee_fn = resolve_namespace_function(alias.start, alias.length,
                                               member.start, member.length);
        callee_st = callee_fn ? NULL : resolve_namespace_struct(alias.start, alias.length,
                                                                member.start, member.length);
        if (!callee_fn && !callee_st) return static_type_unknown();
    } else if (callee_expr && callee_expr->type == AST_IDENTIFIER) {
        callee_fn = resolve_namespace_function(current_module_alias, current_alias_len, name, namelen);
        if (!callee_fn) callee_fn = resolve_namespace_function(NULL, 0, name, namelen);
            if (!callee_fn) callee_fn = resolve_function(name, namelen);

            callee_st = callee_fn ? NULL : resolve_namespace_struct(current_module_alias, current_alias_len,
                                                                    name, namelen);
        if (!callee_st) callee_st = resolve_namespace_struct(NULL, 0, name, namelen);
        if (!callee_st) callee_st = resolve_struct(name, namelen);
    } else {
        return static_type_unknown();
    }

    if (callee_st) return static_type_make(STATIC_TYPE_STRUCT_INSTANCE, callee_st);
    if (callee_fn) return infer_function_return_type(callee_fn);
    return static_type_unknown();
}

static int compile_expr(AstNode* expr);
static void compile_stmt(AstNode* stmt);

static int ast_line(AstNode* expr) {
    if (!expr) return 0;

    switch (expr->type) {
        case AST_IDENTIFIER:
            return expr->data.identifier.name.line;
        case AST_BINARY_EXPR:
            return expr->data.binary.op.line;
        case AST_VAR_DECL:
            return expr->data.var_decl.name.line;
        case AST_ASSIGN_EXPR:
            return expr->data.assign.name.line;
        case AST_EXPR_STMT:
            return ast_line(expr->data.expr_stmt.expr);
        case AST_USE_STMT:
            return expr->data.use_stmt.path.line;
        case AST_BLOCK_STMT:
        case AST_PROGRAM:
            if (expr->data.block.count > 0) return ast_line(expr->data.block.statements[0]);
            return 0;
        case AST_IF_STMT:
            return ast_line(expr->data.if_stmt.condition);
        case AST_WHILE_STMT:
            return ast_line(expr->data.while_stmt.condition);
        case AST_FUNC_DECL:
            return expr->data.func_decl.name.line;
        case AST_STRUCT_DECL:
            return expr->data.struct_decl.name.line;
        case AST_SYNC_STMT:
            return ast_line(expr->data.sync_stmt.body);
        case AST_CALL_EXPR:
            return expr->data.call_expr.name.line;
        case AST_RETURN_STMT:
            return ast_line(expr->data.return_stmt.value);
        case AST_GET_EXPR:
        case AST_SAFE_GET_EXPR:
            return expr->data.get_expr.name.line;
        case AST_SET_EXPR:
            return expr->data.set_expr.name.line;
        case AST_SPAWN_EXPR:
            return ast_line(expr->data.spawn_expr.expr);
        case AST_AWAIT_EXPR:
            return ast_line(expr->data.await_expr.expr);
        case AST_TRY_EXPR:
            return ast_line(expr->data.try_expr.try_block);
        case AST_TYPEOF_EXPR:
            return ast_line(expr->data.typeof_expr.expr);
        case AST_CLONE_EXPR:
        case AST_EVAL_EXPR:
        case AST_KEYS_EXPR:
            return ast_line(expr->data.clone_expr.expr);
        case AST_HAS_EXPR:
            return ast_line(expr->data.has_expr.obj);
        case AST_INDEX_EXPR:
            return ast_line(expr->data.index_expr.target);
        case AST_MATCH_EXPR:
            return ast_line(expr->data.match_expr.left);
        case AST_ERROR_PROPAGATE_EXPR:
            return ast_line(expr->data.error_propagate.expr);
        default:
            return 0;
    }
}

static StaticType infer_expr_type(AstNode* expr) {
    if (!expr) return static_type_unknown();

    switch (expr->type) {
        case AST_NUMBER:
            return static_type_make(STATIC_TYPE_NUMBER, NULL);
        case AST_NIL:
            return static_type_make(STATIC_TYPE_NIL, NULL);
        case AST_STRING:
        case AST_REGEX:
            return static_type_make(STATIC_TYPE_STRING, NULL);
        case AST_IDENTIFIER:
            return infer_identifier_type(expr->data.identifier.name.start,
                                         expr->data.identifier.name.length);
        case AST_ASSIGN_EXPR:
            return infer_expr_type(expr->data.assign.value);
        case AST_BINARY_EXPR: {
            TokenType op = expr->data.binary.op.type;
            if (op == TOKEN_EQUAL_EQUAL || op == TOKEN_BANG_EQUAL ||
                op == TOKEN_LESS || op == TOKEN_LESS_EQUAL ||
                op == TOKEN_GREATER || op == TOKEN_GREATER_EQUAL ||
                op == TOKEN_MATCH) {
                return static_type_make(STATIC_TYPE_BOOL, NULL);
            }

            if (op == TOKEN_QUESTION_QUESTION) {
                StaticType left = infer_expr_type(expr->data.binary.left);
                StaticType right = infer_expr_type(expr->data.binary.right);
                if (left.kind == STATIC_TYPE_NIL) return right;
                if (right.kind == STATIC_TYPE_NIL) return left;
                if (static_types_equal(left, right)) return left;
                return static_type_unknown();
            }

            StaticType left = infer_expr_type(expr->data.binary.left);
            StaticType right = infer_expr_type(expr->data.binary.right);
            if (op == TOKEN_PLUS &&
                (left.kind == STATIC_TYPE_STRING || right.kind == STATIC_TYPE_STRING)) {
                return static_type_make(STATIC_TYPE_STRING, NULL);
            }
            if (left.kind == STATIC_TYPE_NUMBER && right.kind == STATIC_TYPE_NUMBER) {
                return static_type_make(STATIC_TYPE_NUMBER, NULL);
            }
            return static_type_unknown();
        }
        case AST_MATCH_EXPR:
        case AST_HAS_EXPR:
            return static_type_make(STATIC_TYPE_BOOL, NULL);
        case AST_INDEX_EXPR:
            return static_type_unknown();
        case AST_CALL_EXPR:
            return infer_call_expr_type(expr);
        case AST_SPAWN_EXPR:
        case AST_AWAIT_EXPR:
        case AST_EVAL_EXPR:
            return static_type_unknown();
        case AST_TRY_EXPR: {
            StaticType try_type = infer_expr_type(expr->data.try_expr.try_block);
            StaticType catch_type = infer_expr_type(expr->data.try_expr.catch_block);
            if (try_type.kind == STATIC_TYPE_ANY || catch_type.kind == STATIC_TYPE_ANY) {
                return static_type_make(STATIC_TYPE_ANY, NULL);
            }
            if (static_types_equal(try_type, catch_type)) return try_type;
            return static_type_unknown();
        }
        case AST_TYPEOF_EXPR:
            return static_type_make(STATIC_TYPE_STRING, NULL);
        case AST_CLONE_EXPR:
            return infer_expr_type(expr->data.clone_expr.expr);
        case AST_KEYS_EXPR:
        case AST_ARRAY_EXPR:
            return static_type_make(STATIC_TYPE_ARRAY, NULL);
        case AST_GET_EXPR:
        case AST_SAFE_GET_EXPR:
            return static_type_unknown();
        case AST_SET_EXPR:
            return infer_expr_type(expr->data.set_expr.value);
        case AST_ERROR_PROPAGATE_EXPR:
            return infer_expr_type(expr->data.error_propagate.expr);
        default:
            return static_type_unknown();
    }
}

static int compile_expr(AstNode* expr) {
    if (!expr) return -1;
    set_chunk_write_line(ast_line(expr));

    if (expr->type == AST_NUMBER) {
        int ci = add_constant(current_chunk(), (Value){VAL_NUMBER, {.number = expr->data.number_val}});
        int rD = cst.next_reg++;
        write_chunk(current_chunk(), ENCODE_INST(OP_LOAD_CONST, rD, ci, 0));
        return rD;
    }

    if (expr->type == AST_NIL) {
        int ci = add_constant(current_chunk(), (Value){VAL_NIL, {.number = 0}});
        int rD = cst.next_reg++;
        write_chunk(current_chunk(), ENCODE_INST(OP_LOAD_CONST, rD, ci, 0));
        return rD;
    }

    if (expr->type == AST_STRING) {
        int reg = cst.next_reg++;
        int unescaped_len = 0;
        char* unescaped = unescape_string(expr->data.str_val.name, expr->data.str_val.length, &unescaped_len);
        ObjString* s = copy_string(unescaped, unescaped_len);
        free(unescaped);
        Value val = {VAL_OBJ, {.obj = (Obj*)s}};
        int ci = add_constant(current_chunk(), val);
        write_chunk(current_chunk(), ENCODE_INST(OP_LOAD_CONST, reg, ci, 0));
        return reg;
    }

    if (expr->type == AST_REGEX) {
        int reg = cst.next_reg++;
        ObjString* s = copy_string(expr->data.regex.pattern, expr->data.regex.length);
        Value val = {VAL_OBJ, {.obj = (Obj*)s}};
        int ci = add_constant(current_chunk(), val);
        write_chunk(current_chunk(), ENCODE_INST(OP_LOAD_CONST, reg, ci, 0));
        return reg;
    }

    if (expr->type == AST_IDENTIFIER) {
        int local_idx = resolve_local_index(expr->data.identifier.name.start,
                                            expr->data.identifier.name.length);
        if (local_idx == -1) {
            // Check globals
            int global_idx = resolve_global_index(expr->data.identifier.name.start,
                                                  expr->data.identifier.name.length);
            if (global_idx >= 0) {
                int rD = cst.next_reg++;
                ObjString* nameObj = copy_string(global_vars[global_idx].name,
                                                 global_vars[global_idx].length);
                int ci = add_constant(current_chunk(), (Value){VAL_OBJ, {.obj = (Obj*)nameObj}});
                write_chunk(current_chunk(), ENCODE_INST(OP_GET_GLOBAL, rD, ci, 0));
                return rD;
            }

            // [NEW] Check for functions in global registry (Forward Reference support)
            ObjFunction* fn = resolve_namespace_function(current_module_alias, current_alias_len,
                                                         expr->data.identifier.name.start,
                                                         expr->data.identifier.name.length);
            if (!fn) {
                fn = resolve_namespace_function(NULL, 0,
                                                expr->data.identifier.name.start,
                                                expr->data.identifier.name.length);
            }
            if (!fn) {
                fn = resolve_function(expr->data.identifier.name.start,
                                      expr->data.identifier.name.length);
            }
            if (fn) {
                int rD = cst.next_reg++;
                int ci = add_constant(current_chunk(), (Value){VAL_OBJ, {.obj = (Obj*)fn}});
                write_chunk(current_chunk(), ENCODE_INST(OP_LOAD_CONST, rD, ci, 0));
                return rD;
            }

            // [NEW] Check native functions (built-in/registered)
            int native_idx = find_native_index(expr->data.identifier.name.start, expr->data.identifier.name.length);
            if (native_idx != -1) {
                int rD = cst.next_reg++;
                // We emit a dummy LOAD_CONST for now to satisfy the compiler
                // but ideally we should have a way to represent a native as a Value.
                // For 'typeof', we just need to satisfy the compiler.
                // In ViperLang v4, native functions are handled by index in OP_CALL_NATIVE.
                // To treat them as objects, we'd need to wrap them.
                // For simplicity of the test, let's just emit a Nil or something if it's found.
                // Actually, let's emit a specific value or the index.
                Value val = {VAL_OBJ, {.obj = (Obj*)new_function(get_native_name(native_idx), (int)strlen(get_native_name(native_idx)), 0)}};
                int ci = add_constant(current_chunk(), val);
                write_chunk(current_chunk(), ENCODE_INST(OP_LOAD_CONST, rD, ci, 0));
                return rD;
            }

            compiler_error_exit("VCP027", "Undefined variable '%.*s'",
                                expr->data.identifier.name.length,
                                expr->data.identifier.name.start);
        }
        return cst.locals[local_idx].reg;
    }

    if (expr->type == AST_ASSIGN_EXPR) {
        int src = compile_expr(expr->data.assign.value);
        int local_idx = resolve_local_index(expr->data.assign.name.start,
                                            expr->data.assign.name.length);
        if (local_idx != -1) {
            Local* local = &cst.locals[local_idx];
            if (local->has_declared_type) {
                StaticType actual = infer_expr_type(expr->data.assign.value);
                if (static_type_is_known(actual) &&
                    !static_type_is_compatible(actual, local->type)) {
                    compiler_type_mismatch("assignment", local->name, local->length,
                                           local->type, actual);
                }
                emit_type_assert(src, local->type);
            }
            write_chunk(current_chunk(), ENCODE_INST(OP_MOVE, local->reg, src, 0));
            return local->reg;
        }

        // Check globals
        int global_idx = resolve_global_index(expr->data.assign.name.start,
                                              expr->data.assign.name.length);
        if (global_idx >= 0) {
            GlobalVar* global = &global_vars[global_idx];
            if (global->has_declared_type) {
                StaticType actual = infer_expr_type(expr->data.assign.value);
                if (static_type_is_known(actual) &&
                    !static_type_is_compatible(actual, global->type)) {
                    compiler_type_mismatch("assignment", global->name, global->length,
                                           global->type, actual);
                }
                emit_type_assert(src, global->type);
            }

            ObjString* nameObj = copy_string(global->name, global->length);
            int ci = add_constant(current_chunk(), (Value){VAL_OBJ, {.obj = (Obj*)nameObj}});
            write_chunk(current_chunk(), ENCODE_INST(OP_SET_GLOBAL, src, ci, 0));
            return src;
        }

        compiler_error_exit("VCP028", "Undefined variable '%.*s' for assignment",
                            expr->data.assign.name.length, expr->data.assign.name.start);
    }

    if (expr->type == AST_BINARY_EXPR) {
        if (expr->data.binary.op.type == TOKEN_QUESTION_QUESTION) {
            int rL = compile_expr(expr->data.binary.left);
            int destReg = cst.next_reg++;
            
            // Move left result to dest
            write_chunk(current_chunk(), ENCODE_INST(OP_MOVE, destReg, rL, 0));
            
            // If NOT NIL, jump over right side evaluation
            int jumpOverRight = current_chunk()->count;
            write_chunk(current_chunk(), ENCODE_INST(OP_JUMP_IF_NOT_NIL, destReg, 0, 0));
            
            // Right side evaluation
            int rR = compile_expr(expr->data.binary.right);
            write_chunk(current_chunk(), ENCODE_INST(OP_MOVE, destReg, rR, 0));
            
            // Patch jump
            int offset = current_chunk()->count - jumpOverRight - 1;
            Instruction* p_inst = &current_chunk()->code[jumpOverRight];
            *p_inst = ENCODE_INST(OP_JUMP_IF_NOT_NIL, destReg, (offset >> 8) & 0xFF, offset & 0xFF);
            
            return destReg;
        }

        int rL = compile_expr(expr->data.binary.left);
        int rR = compile_expr(expr->data.binary.right);

        if (expr->data.binary.op.type == TOKEN_BANG_EQUAL ||
            expr->data.binary.op.type == TOKEN_LESS_EQUAL ||
            expr->data.binary.op.type == TOKEN_GREATER_EQUAL) {
            int rTmp = cst.next_reg++;
            int rD = cst.next_reg++;
            uint8_t compare = OP_EQUAL;
            switch (expr->data.binary.op.type) {
                case TOKEN_BANG_EQUAL:    compare = OP_EQUAL;   break;
                case TOKEN_LESS_EQUAL:    compare = OP_GREATER; break;
                case TOKEN_GREATER_EQUAL: compare = OP_LESS;    break;
                default: break;
            }
            set_chunk_write_line(ast_line(expr));
            write_chunk(current_chunk(), ENCODE_INST(compare, rTmp, rL, rR));
            set_chunk_write_line(ast_line(expr));
            write_chunk(current_chunk(), ENCODE_INST(OP_NOT, rD, rTmp, 0));
            return rD;
        }

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
        set_chunk_write_line(ast_line(expr));
        write_chunk(current_chunk(), ENCODE_INST(op, rD, rL, rR));
        return rD;
    }

    if (expr->type == AST_MATCH_EXPR) {
        int rL = compile_expr(expr->data.match_expr.left);
        int rR = compile_expr(expr->data.match_expr.right);
        int rD = cst.next_reg++;
        set_chunk_write_line(ast_line(expr));
        write_chunk(current_chunk(), ENCODE_INST(OP_MATCH, rD, rL, rR));
        return rD;
    }

    if (expr->type == AST_INDEX_EXPR) {
        int rObj = compile_expr(expr->data.index_expr.target);
        int rIdx = compile_expr(expr->data.index_expr.index);
        
        if (expr->data.index_expr.value) {
            // SET_INDEX
            int rVal = compile_expr(expr->data.index_expr.value);
            set_chunk_write_line(ast_line(expr));
            write_chunk(current_chunk(), ENCODE_INST(OP_SET_INDEX, rObj, rIdx, rVal));
            return rVal;
        } else {
            // GET_INDEX
            int rDest = cst.next_reg++;
            set_chunk_write_line(ast_line(expr));
            write_chunk(current_chunk(), ENCODE_INST(OP_GET_INDEX, rDest, rObj, rIdx));
            return rDest;
        }
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
            compiler_error_exit("VCP029", "Too many call arguments (%d).", arg_count);
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
                    compiler_error("VCP030", "Unknown namespaced callable '%.*s.%.*s'.",
                                   alias.length, alias.start, member.length, member.start);
                    free(arg_regs);
                    exit(1);
                }
                if (!native_is_enabled(native_idx)) {
                    free(arg_regs);
                    compiler_error_disabled_native(member.start, member.length, native_idx);
                }
                
                // For native methods (like obj.get_fn()), the object itself is the first argument!
                int obj_reg = resolve_local(alias.start, alias.length);
                if (obj_reg == -1) {
                     compiler_error("VCP031", "Undefined object '%.*s' for method call.",
                                    alias.length, alias.start);
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
                set_chunk_write_line(ast_line(expr));
                write_chunk(current_chunk(), ENCODE_INST(OP_CALL_NATIVE, (uint8_t)native_idx, (uint8_t)arg_count, (uint8_t)destReg));
                free(arg_regs);
                return destReg;
            }
        } else if (callee_expr && callee_expr->type == AST_IDENTIFIER) {
            callee_fn = resolve_namespace_function(current_module_alias, current_alias_len,
                                                   name, namelen);
            if (!callee_fn) callee_fn = resolve_function(name, namelen);
            callee_st = callee_fn ? NULL : resolve_namespace_struct(current_module_alias, current_alias_len,
                                                                    name, namelen);
            if (!callee_st) callee_st = resolve_struct(name, namelen);
            native_idx = (callee_fn || callee_st) ? -1 : find_native_index(name, namelen);
            if (native_idx != -1 && !native_is_enabled(native_idx)) {
                free(arg_regs);
                compiler_error_disabled_native(name, namelen, native_idx);
            }
            
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
                    set_chunk_write_line(ast_line(expr));
                    write_chunk(current_chunk(), ENCODE_INST(OP_CALL, fnReg, (uint8_t)arg_count, (uint8_t)destReg));
                    free(arg_regs);
                    return destReg;
                } else {
                    // [NEW] Late binding for global functions
                    ObjFunction* late_fn = resolve_namespace_function(NULL, 0, name, namelen);
                    if (late_fn) {
                        callee_fn = late_fn;
                    } else {
                        const char* hint = capability_hint_for_symbol(name, namelen);
                    if (hint) {
                            compiler_error("VCP032", "Unknown callable '%.*s' (%s, profile=%s)",
                                           namelen, name, hint, VIPER_PROFILE_NAME);
                        } else {
                            compiler_error("VCP032", "Unknown callable '%.*s'", namelen, name);
                        }
                        free(arg_regs);
                        exit(1);
                    }
                }
            }
        } else {
            compiler_error("VCP033", "Unsupported call target.");
            free(arg_regs);
            exit(1);
        }

        if (callee_fn) {
            validate_static_call_contract(expr, callee_fn);
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
            set_chunk_write_line(ast_line(expr));
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
        set_chunk_write_line(ast_line(expr));
        write_chunk(current_chunk(), ENCODE_INST(OP_CALL, fnReg, (uint8_t)arg_count, (uint8_t)destReg));
        free(arg_regs);
        return destReg;
    }

    if (expr->type == AST_SPAWN_EXPR) {
        AstNode* call_expr = expr->data.spawn_expr.expr;
        if (call_expr->type != AST_CALL_EXPR) {
            compiler_error_exit("VCP034", "'spawn' keyword requires a function call.");
        }
        
        const char* name = call_expr->data.call_expr.name.start;
        int namelen      = call_expr->data.call_expr.name.length;
        int arg_count    = call_expr->data.call_expr.arg_count;
        int* arg_regs = NULL;
        ObjFunction* callee_fn = resolve_namespace_function(current_module_alias, current_alias_len,
                                                            name, namelen);
        if (!callee_fn) callee_fn = resolve_function(name, namelen);
        if (!callee_fn) callee_fn = resolve_namespace_function(NULL, 0, name, namelen);
        
        if (!callee_fn) {
            const char* hint = capability_hint_for_symbol(name, namelen);
            if (hint) {
                compiler_error("VCP035", "Unknown spawn callable '%.*s' (%s, profile=%s)",
                               namelen, name, hint, VIPER_PROFILE_NAME);
            } else {
                compiler_error("VCP035", "Unknown spawn callable '%.*s'", namelen, name);
            }
            exit(1);
        }
        
        if (arg_count > 0) {
            arg_regs = malloc(sizeof(int) * arg_count);
            for (int i=0; i<arg_count; i++) {
                arg_regs[i] = compile_expr(call_expr->data.call_expr.args[i]);
            }
        }
        
        int fnReg = cst.next_reg++;
        Value val = {VAL_OBJ, {.obj = (Obj*)callee_fn}};
        int ci = add_constant(current_chunk(), val);
        write_chunk(current_chunk(), ENCODE_INST(OP_LOAD_CONST, fnReg, ci, 0));
        
        for (int i = 0; i < arg_count; i++) {
            int argReg = cst.next_reg++;
            write_chunk(current_chunk(), ENCODE_INST(OP_MOVE, argReg, arg_regs[i], 0));
        }
        
        int destReg = cst.next_reg++;
        write_chunk(current_chunk(), ENCODE_INST(OP_SPAWN, destReg, fnReg, (uint8_t)arg_count));
        if(arg_regs) free(arg_regs);
        return destReg;
    }

    if (expr->type == AST_AWAIT_EXPR) {
        int rHandle = compile_expr(expr->data.await_expr.expr);
        int destReg = cst.next_reg++;
        write_chunk(current_chunk(), ENCODE_INST(OP_AWAIT, destReg, rHandle, 0));
        return destReg;
    }

    if (expr->type == AST_TRY_EXPR) {
        int destReg = cst.next_reg++;
        
        // Emits OP_SETUP_TRY <destReg> 0 0
        int setup_inst_index = current_chunk()->count;
        write_chunk(current_chunk(), ENCODE_INST(OP_SETUP_TRY, destReg, 0, 0));
        
        int tryReg = compile_expr(expr->data.try_expr.try_block);
        if (tryReg != destReg) {
            write_chunk(current_chunk(), ENCODE_INST(OP_MOVE, destReg, tryReg, 0));
        }
        
        // Success: Cancel the panic interceptor
        write_chunk(current_chunk(), ENCODE_INST(OP_TEARDOWN_TRY, 0, 0, 0));
        
        // Success: Skip the else branch
        int jump_over_catch_index = current_chunk()->count;
        write_chunk(current_chunk(), ENCODE_INST(OP_JUMP, 0, 0, 0));
        
        // Patch SETUP_TRY targeting here!
        int catch_offset = current_chunk()->count - setup_inst_index;
        Instruction* p_setup = &current_chunk()->code[setup_inst_index];
        *p_setup = ENCODE_INST(OP_SETUP_TRY, destReg, (catch_offset >> 8) & 0xFF, catch_offset & 0xFF);
        
        // Else Block Compilation
        int catchReg = compile_expr(expr->data.try_expr.catch_block);
        if (catchReg != destReg) {
            write_chunk(current_chunk(), ENCODE_INST(OP_MOVE, destReg, catchReg, 0));
        }
        
        // Patch JUMP targeting here!
        int end_offset = current_chunk()->count - jump_over_catch_index;
        Instruction* p_end = &current_chunk()->code[jump_over_catch_index];
        *p_end = ENCODE_INST(OP_JUMP, 0, (end_offset >> 8) & 0xFF, end_offset & 0xFF);
        
        return destReg;
    }

    if (expr->type == AST_TYPEOF_EXPR) {
        int rVal = compile_expr(expr->data.typeof_expr.expr);
        int destReg = cst.next_reg++;
        write_chunk(current_chunk(), ENCODE_INST(OP_TYPEOF, destReg, rVal, 0));
        return destReg;
    }

    if (expr->type == AST_CLONE_EXPR) {
        int rVal = compile_expr(expr->data.clone_expr.expr);
        int destReg = cst.next_reg++;
        write_chunk(current_chunk(), ENCODE_INST(OP_CLONE, destReg, rVal, 0));
        return destReg;
    }

    if (expr->type == AST_EVAL_EXPR) {
        int rCode = compile_expr(expr->data.clone_expr.expr); 
        int rDest = cst.next_reg++;
        write_chunk(current_chunk(), ENCODE_INST(OP_EVAL, rDest, rCode, 0));
        return rDest;
    }

    if (expr->type == AST_KEYS_EXPR) {
        int rObj = compile_expr(expr->data.clone_expr.expr);
        int rDest = cst.next_reg++;
        write_chunk(current_chunk(), ENCODE_INST(OP_KEYS, rDest, rObj, 0));
        return rDest;
    }

    if (expr->type == AST_HAS_EXPR) {
        int rObj = compile_expr(expr->data.has_expr.obj);
        int rProp = compile_expr(expr->data.has_expr.prop);
        int rDest = cst.next_reg++;
        write_chunk(current_chunk(), ENCODE_INST(OP_HAS, rDest, rObj, rProp));
        return rDest;
    }

    if (expr->type == AST_ARRAY_EXPR) {
        int count = expr->data.array_expr.count;
        int startReg = cst.next_reg;
        for (int i = 0; i < count; i++) {
            int elemReg = compile_expr(expr->data.array_expr.elements[i]);
            if (elemReg != startReg + i) {
                write_chunk(current_chunk(), ENCODE_INST(OP_MOVE, startReg + i, elemReg, 0));
            }
            if (cst.next_reg <= startReg + i) cst.next_reg = startReg + i + 1;
        }
        int destReg = cst.next_reg++;
        write_chunk(current_chunk(), ENCODE_INST(OP_ARRAY, destReg, count, startReg));
        return destReg;
    }

    if (expr->type == AST_GET_EXPR) {
        int rObj = compile_expr(expr->data.get_expr.obj);
        int destReg = cst.next_reg++;
        ObjString* fieldName = copy_string(expr->data.get_expr.name.start, expr->data.get_expr.name.length);
        int ci = add_constant(current_chunk(), (Value){VAL_OBJ, {.obj = (Obj*)fieldName}});
        write_chunk(current_chunk(), ENCODE_INST(OP_GET_FIELD, destReg, rObj, ci));
        return destReg;
    }

    if (expr->type == AST_SAFE_GET_EXPR) {
        int rObj = compile_expr(expr->data.get_expr.obj);
        int destReg = cst.next_reg++;
        
        // Handle nil check
        int jumpIfNil = current_chunk()->count;
        write_chunk(current_chunk(), ENCODE_INST(OP_JUMP_IF_NIL, rObj, 0, 0));
        
        // Regular get
        ObjString* fieldName = copy_string(expr->data.get_expr.name.start, expr->data.get_expr.name.length);
        int ci = add_constant(current_chunk(), (Value){VAL_OBJ, {.obj = (Obj*)fieldName}});
        write_chunk(current_chunk(), ENCODE_INST(OP_GET_FIELD, destReg, rObj, ci));
        
        // Jump over nil-result assignment
        int jumpEnd = current_chunk()->count;
        write_chunk(current_chunk(), ENCODE_INST(OP_JUMP, 0, 0, 0));
        
        // Target for NIL check
        int nilOffset = current_chunk()->count - jumpIfNil - 1;
        Instruction* p_nil = &current_chunk()->code[jumpIfNil];
        *p_nil = ENCODE_INST(OP_JUMP_IF_NIL, rObj, (nilOffset >> 8) & 0xFF, nilOffset & 0xFF);
        
        // Load NIL into destReg
        Value nilVal = {VAL_NIL, {.number = 0}};
        int nilCi = add_constant(current_chunk(), nilVal);
        write_chunk(current_chunk(), ENCODE_INST(OP_LOAD_CONST, destReg, nilCi, 0));
        
        // Patch end jump
        int endOffset = current_chunk()->count - jumpEnd - 1;
        Instruction* p_end = &current_chunk()->code[jumpEnd];
        *p_end = ENCODE_INST(OP_JUMP, 0, (endOffset >> 8) & 0xFF, endOffset & 0xFF);
        
        return destReg;
    }

    if (expr->type == AST_SET_EXPR) {
        int rObj = compile_expr(expr->data.set_expr.obj);
        int rVal = compile_expr(expr->data.set_expr.value);
        ObjString* fieldName = copy_string(expr->data.set_expr.name.start, expr->data.set_expr.name.length);
        int ci = add_constant(current_chunk(), (Value){VAL_OBJ, {.obj = (Obj*)fieldName}});
        write_chunk(current_chunk(), ENCODE_INST(OP_SET_FIELD, rObj, ci, rVal));
        return rVal;
    }

    if (expr->type == AST_ERROR_PROPAGATE_EXPR) {
        int rVal = compile_expr(expr->data.error_propagate.expr);
        write_chunk(current_chunk(), ENCODE_INST(OP_PROPAGATE_ERR, rVal, 0, 0));
        return rVal;
    }

    return -1;
}

// ---- Statement Compiler --------------------------------------------------

static void compile_stmt(AstNode* stmt) {
    if (!stmt) return;
    set_chunk_write_line(ast_line(stmt));

    if (stmt->type == AST_VAR_DECL) {
        bool has_declared_type = stmt->data.var_decl.type_annot.length > 0;
        StaticType declared_type = static_type_unknown();
        if (has_declared_type) {
            declared_type = resolve_type_annotation(stmt->data.var_decl.type_annot);
            StaticType actual_type = infer_expr_type(stmt->data.var_decl.initializer);
            if (static_type_is_known(actual_type) &&
                !static_type_is_compatible(actual_type, declared_type)) {
                compiler_type_mismatch("variable", stmt->data.var_decl.name.start,
                                       stmt->data.var_decl.name.length,
                                       declared_type, actual_type);
            }
        }

        int reg = compile_expr(stmt->data.var_decl.initializer);
        add_local_typed(stmt->data.var_decl.name.start,
                        stmt->data.var_decl.name.length,
                        reg,
                        cst.scope_depth,
                        has_declared_type,
                        declared_type);

        if (has_declared_type) {
            emit_type_assert(reg, declared_type);
        }

        if (cst.scope_depth == 0) {
            register_global_symbol(stmt->data.var_decl.name.start,
                                   stmt->data.var_decl.name.length,
                                   has_declared_type,
                                   declared_type);
            
            ObjString* nameObj = copy_string(stmt->data.var_decl.name.start, stmt->data.var_decl.name.length);
            int ci = add_constant(current_chunk(), (Value){VAL_OBJ, {.obj = (Obj*)nameObj}});
            write_chunk(current_chunk(), ENCODE_INST(OP_SET_GLOBAL, reg, ci, 0));
        }
    }
    else if (stmt->type == AST_BLOCK_STMT) {
        begin_scope();
        for (int i = 0; i < stmt->data.block.count; i++) compile_stmt(stmt->data.block.statements[i]);
        end_scope();
    }
    else if (stmt->type == AST_SYNC_STMT) {
        write_chunk(current_chunk(), ENCODE_INST(OP_SYNC_START, 0, 0, 0));
        compile_stmt(stmt->data.sync_stmt.body);
        write_chunk(current_chunk(), ENCODE_INST(OP_SYNC_END, 0, 0, 0));
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
            StaticType actual_type = infer_expr_type(stmt->data.return_stmt.value);
            int reg = compile_expr(stmt->data.return_stmt.value);

            if (cst.has_return_type) {
                if (static_type_is_known(actual_type) &&
                    !static_type_is_compatible(actual_type, cst.return_type)) {
                    compiler_type_mismatch("return in function",
                                           cst.current_fn ? cst.current_fn->name : NULL,
                                           cst.current_fn ? cst.current_fn->name_len : 0,
                                           cst.return_type, actual_type);
                }
                if (cst.return_type_ci >= 0) {
                    write_chunk(current_chunk(), ENCODE_INST(OP_ASSERT_TYPE, reg, cst.return_type_ci, 0));
                }
            }
            write_chunk(current_chunk(), ENCODE_INST(OP_RETURN, reg, 0, 0));
        } else {
            if (cst.has_return_type && cst.return_type.kind != STATIC_TYPE_ANY) {
                compiler_type_mismatch("return in function",
                                       cst.current_fn ? cst.current_fn->name : NULL,
                                       cst.current_fn ? cst.current_fn->name_len : 0,
                                       cst.return_type,
                                       static_type_make(STATIC_TYPE_NIL, NULL));
            }
            write_chunk(current_chunk(), ENCODE_INST(OP_RETURN, 0, 0, 0));
        }
    }
    if (stmt->type == AST_FUNC_DECL) {
        verify_function_declared_effects(stmt);
        // Find already registered prototype
        ObjFunction* fn = resolve_namespace_function(current_module_alias, current_alias_len,
                                                       stmt->data.func_decl.name.start, stmt->data.func_decl.name.length);
        if (!fn) {
             compiler_error_exit("VCP036", "Proto for %.*s not found (alias: %.*s)",
                                 stmt->data.func_decl.name.length, stmt->data.func_decl.name.start,
                                 current_alias_len, current_module_alias);
        }
        
        CompilerState prevCompiler = cst;
        init_compiler(fn);

        FunctionTypeInfo* fn_info = find_function_type_info(fn);
        if (fn_info && fn_info->has_return_type) {
            cst.has_return_type = true;
            cst.return_type = fn_info->return_type;
            cst.return_type_ci = add_type_constant(cst.return_type);
        }
        
        begin_scope();
        for (int i = 0; i < stmt->data.func_decl.param_count; i++) {
            StaticType param_type = static_type_unknown();
            bool has_param_type = false;
            if (fn_info && i < fn_info->param_count) {
                param_type = fn_info->param_types ? fn_info->param_types[i] : static_type_unknown();
                has_param_type = static_type_is_known(param_type);
            }
            int reg = cst.next_reg++;
            add_local_typed(stmt->data.func_decl.params[i].start,
                            stmt->data.func_decl.params[i].length,
                            reg, 0, has_param_type, param_type);
            if (has_param_type) {
                emit_type_assert(reg, param_type);
            }
        }
        compile_stmt(stmt->data.func_decl.body);
        write_chunk(current_chunk(), ENCODE_INST(OP_RETURN, 0, 0, 0));
        end_scope();
        free(cst.locals);
        cst = prevCompiler;
    }
    else if (stmt->type == AST_STRUCT_DECL) {
        ObjStruct* st = resolve_namespace_struct(current_module_alias, current_alias_len,
                                                 stmt->data.struct_decl.name.start,
                                                 stmt->data.struct_decl.name.length);
        if (!st) {
            st = resolve_struct(stmt->data.struct_decl.name.start,
                                stmt->data.struct_decl.name.length);
        }
        if (!st) {
            compiler_error_exit("VCP037", "Struct proto for %.*s not found.",
                                stmt->data.struct_decl.name.length, stmt->data.struct_decl.name.start);
        }

        int reg = cst.next_reg++;
        int ci = add_constant(current_chunk(), (Value){VAL_OBJ, {.obj = (Obj*)st}});
        write_chunk(current_chunk(), ENCODE_INST(OP_STRUCT, reg, ci, 0));
        
        // Register in local table if at top level so it acts like a global type name
        if (cst.scope_depth == 0) {
            add_local_typed(stmt->data.struct_decl.name.start,
                            stmt->data.struct_decl.name.length,
                            reg,
                            0,
                            false,
                            static_type_make(STATIC_TYPE_STRUCT_DEF, st));
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
                compiler_error_exit("VCP038", "Namespace alias is too long.");
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
            compiler_error_exit("VCP039", "Could not open module '%s' (resolved as '%s').",
                                raw_path, resolved_path);
        }
        remember_imported_module(resolved_path);

        path_dirname(resolved_path, module_dir, sizeof(module_dir));
        push_module_dir(module_dir);
        AstNode* mod_ast = parse(source);

        // --- NEW: Handle Aliased Prototype Registration ---
        char prev_alias[MAX_NAMESPACE_ALIAS_LEN];
        int prev_alias_len = current_alias_len;
        memcpy(prev_alias, current_module_alias, MAX_NAMESPACE_ALIAS_LEN);

        if (alias_len > 0) {
            memcpy(current_module_alias, alias_buf, alias_len + 1);
            current_alias_len = alias_len;
        } else {
            // If no alias, it's a flat import into current namespace (or global)
            // But we still need prototypes to be searchable.
            // For now, let's keep current alias if it's set.
        }

        register_struct_prototypes(mod_ast, current_module_alias, current_alias_len);
        register_function_prototypes(mod_ast, current_module_alias, current_alias_len);

        ExportDecl* exports = NULL;
        int export_count = 0;
        bool has_public = false;
        if (!collect_module_export_decls(mod_ast, &exports, &export_count, &has_public)) {
            compiler_error_exit("VCP040", "Out of memory while collecting exports for '%s'.",
                                resolved_path);
        }

        if (mod_ast->type == AST_PROGRAM) {
            for (int i = 0; i < mod_ast->data.block.count; i++) {
                compile_stmt(mod_ast->data.block.statements[i]);
            }
        }
        
        // Restore alias
        memcpy(current_module_alias, prev_alias, MAX_NAMESPACE_ALIAS_LEN);
        current_alias_len = prev_alias_len;
        
        pop_module_dir();

        bool apply_public_filter = is_package_import(raw_path) && has_public;
        if ((alias_len > 0) || apply_public_filter) {
            const char* alias = (alias_len > 0) ? alias_buf : NULL;
            if (!filter_or_namespace_imported_symbols(base_fn_count, base_st_count,
                                                      exports, export_count, has_public,
                                                      alias, alias_len,
                                                      apply_public_filter, resolved_path)) {
                free(exports);
                compiler_error_exit("VCP041", "Failed to apply import visibility for '%s'.",
                                    resolved_path);
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

static void register_struct_prototypes(AstNode* node, const char* alias, int alias_len) {
    if (!node) return;
    if (node->type == AST_PROGRAM || node->type == AST_BLOCK_STMT) {
        for (int i=0; i < node->data.block.count; i++) {
            register_struct_prototypes(node->data.block.statements[i], alias, alias_len);
        }
    } else if (node->type == AST_STRUCT_DECL) {
        const char** field_names = NULL;
        int* field_lens = NULL;

        if (node->data.struct_decl.field_count > 0) {
            field_names = (const char**)malloc(sizeof(char*) * (size_t)node->data.struct_decl.field_count);
            field_lens = (int*)malloc(sizeof(int) * (size_t)node->data.struct_decl.field_count);
            if (!field_names || !field_lens) compiler_fatal_oom();
            for (int i = 0; i < node->data.struct_decl.field_count; i++) {
                field_names[i] = node->data.struct_decl.fields[i].start;
                field_lens[i] = node->data.struct_decl.fields[i].length;
            }
        }

        ObjStruct* st = new_struct(node->data.struct_decl.name.start,
                                   node->data.struct_decl.name.length,
                                   node->data.struct_decl.field_count,
                                   field_names,
                                   field_lens);
        register_struct_symbol(st);
        register_namespace_symbol(alias, alias_len, ABI_KIND_STRUCT,
                                  st->name, st->name_len, (Obj*)st, "builtin");
    }
}

static void register_function_prototypes(AstNode* node, const char* alias, int alias_len) {
    if (!node) return;
    if (node->type == AST_PROGRAM || node->type == AST_BLOCK_STMT) {
        for (int i=0; i < node->data.block.count; i++) {
            register_function_prototypes(node->data.block.statements[i], alias, alias_len);
        }
    } else if (node->type == AST_FUNC_DECL) {
        ObjFunction* fn = new_function(node->data.func_decl.name.start, 
                                       node->data.func_decl.name.length,
                                       node->data.func_decl.param_count);
        register_function_symbol(fn);
        register_namespace_symbol(alias, alias_len, ABI_KIND_FUNCTION, fn->name, fn->name_len, (Obj*)fn, "builtin");

        FunctionTypeInfo* info = ensure_function_type_info(fn);
        clear_function_param_contract(info);
        if (node->data.func_decl.param_count > 0) {
            info->param_names = (const char**)calloc((size_t)node->data.func_decl.param_count, sizeof(const char*));
            info->param_name_lens = (int*)calloc((size_t)node->data.func_decl.param_count, sizeof(int));
            info->param_types = (StaticType*)calloc((size_t)node->data.func_decl.param_count, sizeof(StaticType));
            if (!info->param_names || !info->param_name_lens || !info->param_types) compiler_fatal_oom();
            info->param_count = node->data.func_decl.param_count;
            for (int i = 0; i < node->data.func_decl.param_count; i++) {
                info->param_names[i] = node->data.func_decl.params[i].start;
                info->param_name_lens[i] = node->data.func_decl.params[i].length;
                if (node->data.func_decl.param_types &&
                    node->data.func_decl.param_types[i].length > 0) {
                    info->param_types[i] = resolve_type_annotation_in_namespace(
                        node->data.func_decl.param_types[i], alias, alias_len);
                } else {
                    info->param_types[i] = static_type_unknown();
                }
            }
        }
        if (node->data.func_decl.return_type.length > 0) {
            info->has_return_type = true;
            info->return_type = resolve_type_annotation_in_namespace(node->data.func_decl.return_type,
                                                                     alias, alias_len);
        }
        info->declared_effects = declared_effect_mask(node->data.func_decl.effects,
                                                      node->data.func_decl.effect_count);
    }
}

ObjFunction* compile(AstNode* program) {
    if (!program) return NULL;
    clear_imported_modules();
    clear_module_dirs();
    clear_namespace_registry();
    fn_count = 0;
    st_count = 0;
    free_function_type_registry();
    global_var_count = 0;
    cst.local_cap = 0;
    cst.local_count = 0;
    if (cst.locals) {
        free(cst.locals);
        cst.locals = NULL;
    }
    
    current_module_alias[0] = '\0';
    current_alias_len = 0;
    
    // Pass 1: Register all top-level type/function prototypes before compilation.
    register_struct_prototypes(program, NULL, 0);
    register_function_prototypes(program, NULL, 0);

    detect_project_root();

    char entry_dir[MAX_PATH_LEN];
    if (entry_file_path[0] != '\0') {
        path_dirname(entry_file_path, entry_dir, sizeof(entry_dir));
    } else {
        copy_path(entry_dir, sizeof(entry_dir), ".");
    }
    push_module_dir(entry_dir);

    ObjFunction* main_fn = new_function("__main__", 8, 0);

    // Pass 2: Compile
    init_compiler(main_fn);
    if (program->type == AST_PROGRAM) {
        for (int i = 0; i < program->data.block.count; i++) {
            AstNode* stmt = program->data.block.statements[i];
            if (!emit_halt_enabled && i == program->data.block.count - 1 && stmt->type == AST_EXPR_STMT) {
                // For eval(), make sure the last expression result lands in R0
                int r = compile_expr(stmt->data.expr_stmt.expr);
                if (r != 0) {
                    write_chunk(current_chunk(), ENCODE_INST(OP_MOVE, 0, r, 0));
                }
            } else {
                compile_stmt(stmt);
            }
        }
    } else {
        compile_stmt(program);
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
