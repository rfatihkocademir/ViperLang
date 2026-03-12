#define _GNU_SOURCE
#include "pkg.h"
#include "bytecode.h"
#include "compiler.h"
#include "native.h"
#include "parser.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX_PATH_LEN 1024
#define MAX_LINE_LEN 1024
#define MAX_NAME_LEN 256
#define MAX_VERSION_LEN 64
#define MAX_ABI_EFFECTS_LEN 256
#define ABI_KIND_FUNCTION 1
#define ABI_KIND_STRUCT 2

typedef struct {
    char name[MAX_NAME_LEN];
    char version[MAX_VERSION_LEN];
} Dependency;

typedef struct {
    int kind; // 1=function, 2=struct
    char name[MAX_NAME_LEN];
    int metric; // arity or field_count
    char return_type[MAX_NAME_LEN];
    char effects[MAX_ABI_EFFECTS_LEN];
} AbiSymbol;

typedef struct {
    char module[MAX_PATH_LEN];
    int kind;
    char name[MAX_NAME_LEN];
    int metric;
    char return_type[MAX_NAME_LEN];
    char effects[MAX_ABI_EFFECTS_LEN];
} AbiEntry;

static void copy_text(char* dst, size_t dst_size, const char* src) {
    if (dst_size == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, dst_size, "%s", src);
}

static bool append_fmt(char* out, size_t out_size, size_t* len, const char* fmt, ...);

static void copy_token_text(char* dst, size_t dst_size, Token token) {
    if (!dst || dst_size == 0) return;
    if (!token.start || token.length <= 0) {
        dst[0] = '\0';
        return;
    }
    size_t n = (size_t)token.length;
    if (n >= dst_size) n = dst_size - 1;
    memcpy(dst, token.start, n);
    dst[n] = '\0';
}

static bool format_ast_effects(Token* effects, int effect_count, char* out, size_t out_size) {
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    if (!effects || effect_count <= 0) {
        copy_text(out, out_size, "-");
        return true;
    }

    size_t len = 0;
    for (int i = 0; i < effect_count; i++) {
        if (!append_fmt(out, out_size, &len, "%s%.*s",
                        i > 0 ? "," : "",
                        effects[i].length, effects[i].start)) {
            return false;
        }
    }
    return true;
}

static bool append_fmt(char* out, size_t out_size, size_t* len, const char* fmt, ...) {
    if (!out || !len || !fmt || *len >= out_size) return false;
    va_list ap;
    va_start(ap, fmt);
    int wrote = vsnprintf(out + *len, out_size - *len, fmt, ap);
    va_end(ap);
    if (wrote < 0) return false;
    size_t n = (size_t)wrote;
    if (*len + n >= out_size) return false;
    *len += n;
    return true;
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

static bool join_path(const char* a, const char* b, char* out, size_t out_size) {
    if (!a || !b || out_size == 0) return false;
    size_t la = strlen(a);
    size_t lb = strlen(b);
    if (la + 1 + lb + 1 > out_size) return false;
    memcpy(out, a, la);
    out[la] = '/';
    memcpy(out + la + 1, b, lb);
    out[la + 1 + lb] = '\0';
    return true;
}

static void path_dirname(const char* path, char* out, size_t out_size) {
    if (!path || path[0] == '\0') {
        copy_text(out, out_size, ".");
        return;
    }
    const char* slash = strrchr(path, '/');
    if (!slash) {
        copy_text(out, out_size, ".");
        return;
    }
    if (slash == path) {
        copy_text(out, out_size, "/");
        return;
    }
    size_t len = (size_t)(slash - path);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, path, len);
    out[len] = '\0';
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

static bool is_directory(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return false;
    return S_ISDIR(st.st_mode);
}

static bool ensure_dir(const char* path) {
    struct stat st;
    if (stat(path, &st) == 0) return S_ISDIR(st.st_mode);
    if (mkdir(path, 0755) == 0) return true;
    return errno == EEXIST;
}

static bool remove_path_recursive(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return errno == ENOENT;

    if (S_ISDIR(st.st_mode)) {
        DIR* dir = opendir(path);
        if (!dir) return false;

        bool ok = true;
        struct dirent* entry = NULL;
        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

            char child[MAX_PATH_LEN];
            if (!join_path(path, entry->d_name, child, sizeof(child))) {
                ok = false;
                break;
            }
            if (!remove_path_recursive(child)) {
                ok = false;
                break;
            }
        }

        closedir(dir);
        if (!ok) return false;
        return rmdir(path) == 0 || errno == ENOENT;
    }

    return unlink(path) == 0 || errno == ENOENT;
}

static bool ensure_dir_recursive(const char* path) {
    if (!path || path[0] == '\0') return false;
    if (strcmp(path, "/") == 0) return true;

    char tmp[MAX_PATH_LEN];
    copy_text(tmp, sizeof(tmp), path);

    size_t len = strlen(tmp);
    if (len == 0) return false;
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';

    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (!ensure_dir(tmp)) return false;
            *p = '/';
        }
    }

    return ensure_dir(tmp);
}

static bool is_valid_version(const char* ver) {
    if (!ver || ver[0] == '\0') return false;
    for (const unsigned char* p = (const unsigned char*)ver; *p; p++) {
        if (isalnum(*p) || *p == '.' || *p == '-' || *p == '_' || *p == '+') continue;
        return false;
    }
    return true;
}

static bool is_valid_package_name(const char* name) {
    if (!name || name[0] == '\0') return false;
    if (strstr(name, "..") != NULL) return false;
    if (name[0] == '/' || name[0] == '\\') return false;

    for (const unsigned char* p = (const unsigned char*)name; *p; p++) {
        if (isalnum(*p) || *p == '@' || *p == '_' || *p == '-' || *p == '.' || *p == '/') continue;
        return false;
    }
    return true;
}

static const char* ltrim(const char* s) {
    while (*s && isspace((unsigned char)*s)) s++;
    return s;
}

static bool parse_use_line(const char* line, char* pkg, size_t pkg_size,
                           char* version, size_t version_size) {
    const char* p = ltrim(line);
    if (strncmp(p, "use", 3) != 0 || !isspace((unsigned char)p[3])) return false;
    p += 3;
    p = ltrim(p);

    const char* n0 = p;
    while (*p && !isspace((unsigned char)*p) && *p != '(') p++;
    if (p == n0) return false;

    size_t nlen = (size_t)(p - n0);
    if (nlen >= pkg_size) return false;
    memcpy(pkg, n0, nlen);
    pkg[nlen] = '\0';

    p = ltrim(p);
    if (*p != '(') return false;
    p++;

    const char* v0 = p;
    while (*p && *p != ')' && !isspace((unsigned char)*p)) p++;
    if (p == v0 || *p != ')') return false;

    size_t vlen = (size_t)(p - v0);
    if (vlen >= version_size) return false;
    memcpy(version, v0, vlen);
    version[vlen] = '\0';

    return is_valid_package_name(pkg) && is_valid_version(version);
}

static bool find_project_root_from(const char* start_dir, char* out, size_t out_size) {
    char current[MAX_PATH_LEN];
    copy_text(current, sizeof(current), start_dir);

    while (1) {
        char manifest[MAX_PATH_LEN];
        if (!join_path(current, "viper.vpmod", manifest, sizeof(manifest))) return false;
        if (file_exists(manifest)) {
            copy_text(out, out_size, current);
            return true;
        }

        if (strcmp(current, "/") == 0 || strcmp(current, ".") == 0) break;

        char parent[MAX_PATH_LEN];
        path_dirname(current, parent, sizeof(parent));
        if (strcmp(parent, current) == 0) break;
        copy_text(current, sizeof(current), parent);
    }

    return false;
}

static bool find_project_root(char* out, size_t out_size) {
    char cwd[MAX_PATH_LEN];
    if (getcwd(cwd, sizeof(cwd)) == NULL) return false;
    return find_project_root_from(cwd, out, out_size);
}

static bool read_text_file(const char* path, char** out) {
    *out = NULL;
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

    char* buf = (char*)malloc((size_t)size + 1);
    if (!buf) {
        fclose(file);
        return false;
    }

    size_t read_n = fread(buf, 1, (size_t)size, file);
    fclose(file);
    if (read_n < (size_t)size) {
        free(buf);
        return false;
    }

    buf[read_n] = '\0';
    *out = buf;
    return true;
}

static bool write_text_file(const char* path, const char* content) {
    FILE* file = fopen(path, "wb");
    if (!file) return false;
    size_t n = strlen(content);
    bool ok = fwrite(content, 1, n, file) == n;
    fclose(file);
    return ok;
}

static bool vp_to_vbc_path(const char* vp_path, char* out, size_t out_size) {
    if (!vp_path || !out || out_size == 0) return false;
    if (!has_suffix(vp_path, ".vp")) return false;

    size_t n = strlen(vp_path);
    if (n + 2 > out_size) return false;

    memcpy(out, vp_path, n - 2);
    out[n - 2] = 'v';
    out[n - 1] = 'b';
    out[n] = 'c';
    out[n + 1] = '\0';
    return true;
}

static bool vp_to_vbb_path(const char* vp_path, char* out, size_t out_size) {
    if (!vp_path || !out || out_size == 0) return false;
    if (!has_suffix(vp_path, ".vp")) return false;

    size_t n = strlen(vp_path);
    if (n + 2 > out_size) return false;

    memcpy(out, vp_path, n - 2);
    out[n - 2] = 'v';
    out[n - 1] = 'b';
    out[n] = 'b';
    out[n + 1] = '\0';
    return true;
}

static bool vp_to_vabi_path(const char* vp_path, char* out, size_t out_size) {
    if (!vp_path || !out || out_size == 0) return false;
    if (!has_suffix(vp_path, ".vp")) return false;

    size_t n = strlen(vp_path);
    if (n + 3 > out_size) return false;

    memcpy(out, vp_path, n - 2);
    out[n - 2] = 'v';
    out[n - 1] = 'a';
    out[n] = 'b';
    out[n + 1] = 'i';
    out[n + 2] = '\0';
    return true;
}

static bool append_abi_symbol(AbiSymbol** syms, int* count, int* cap,
                              int kind, const char* name, int name_len, int metric,
                              const char* return_type, const char* effects) {
    if (!name || name_len <= 0 || name_len >= MAX_NAME_LEN) return false;

    for (int i = 0; i < *count; i++) {
        if ((*syms)[i].kind == kind &&
            (int)strlen((*syms)[i].name) == name_len &&
            memcmp((*syms)[i].name, name, (size_t)name_len) == 0) {
            (*syms)[i].metric = metric;
            copy_text((*syms)[i].return_type, sizeof((*syms)[i].return_type), return_type ? return_type : "");
            copy_text((*syms)[i].effects, sizeof((*syms)[i].effects), effects ? effects : "-");
            return true;
        }
    }

    if (*count + 1 > *cap) {
        int next = (*cap == 0) ? 8 : *cap * 2;
        AbiSymbol* grown = (AbiSymbol*)realloc(*syms, sizeof(AbiSymbol) * (size_t)next);
        if (!grown) return false;
        *syms = grown;
        *cap = next;
    }

    AbiSymbol* out_sym = &(*syms)[*count];
    out_sym->kind = kind;
    memcpy(out_sym->name, name, (size_t)name_len);
    out_sym->name[name_len] = '\0';
    out_sym->metric = metric;
    copy_text(out_sym->return_type, sizeof(out_sym->return_type), return_type ? return_type : "");
    copy_text(out_sym->effects, sizeof(out_sym->effects), effects ? effects : "-");
    (*count)++;
    return true;
}

static int find_abi_symbol(const AbiSymbol* syms, int count, int kind, const char* name) {
    if (!syms || !name) return -1;
    for (int i = 0; i < count; i++) {
        if (syms[i].kind == kind && strcmp(syms[i].name, name) == 0) return i;
    }
    return -1;
}

static bool append_abi_symbol_strict(AbiSymbol** syms, int* count, int* cap,
                                     int kind, const char* name, int name_len, int metric,
                                     const char* return_type, const char* effects) {
    if (!name || name_len <= 0 || name_len >= MAX_NAME_LEN || metric < 0) return false;

    int existing = find_abi_symbol(*syms, *count, kind, name);
    if (existing >= 0) {
        if ((*syms)[existing].metric != metric ||
            strcmp((*syms)[existing].return_type, return_type ? return_type : "") != 0 ||
            strcmp((*syms)[existing].effects, effects ? effects : "-") != 0) {
            return false;
        }
        return true;
    }

    if (*count + 1 > *cap) {
        int next = (*cap == 0) ? 8 : *cap * 2;
        AbiSymbol* grown = (AbiSymbol*)realloc(*syms, sizeof(AbiSymbol) * (size_t)next);
        if (!grown) return false;
        *syms = grown;
        *cap = next;
    }

    AbiSymbol* out_sym = &(*syms)[*count];
    out_sym->kind = kind;
    memcpy(out_sym->name, name, (size_t)name_len);
    out_sym->name[name_len] = '\0';
    out_sym->metric = metric;
    copy_text(out_sym->return_type, sizeof(out_sym->return_type), return_type ? return_type : "");
    copy_text(out_sym->effects, sizeof(out_sym->effects), effects ? effects : "-");
    (*count)++;
    return true;
}

static int abi_symbol_cmp(const void* a, const void* b) {
    const AbiSymbol* lhs = (const AbiSymbol*)a;
    const AbiSymbol* rhs = (const AbiSymbol*)b;
    if (lhs->kind != rhs->kind) return lhs->kind - rhs->kind;
    int c = strcmp(lhs->name, rhs->name);
    if (c != 0) return c;
    return lhs->metric - rhs->metric;
}

static bool abi_symbol_has_name(const AbiSymbol* syms, int count, int kind,
                                const char* name, int name_len) {
    if (!syms || !name || name_len <= 0) return false;
    for (int i = 0; i < count; i++) {
        if (syms[i].kind != kind) continue;
        if ((int)strlen(syms[i].name) != name_len) continue;
        if (memcmp(syms[i].name, name, (size_t)name_len) == 0) return true;
    }
    return false;
}

static const AbiSymbol* find_abi_symbol_by_name_len(const AbiSymbol* syms, int count, int kind,
                                                    const char* name, int name_len) {
    if (!syms || !name || name_len <= 0) return NULL;
    for (int i = 0; i < count; i++) {
        if (syms[i].kind != kind) continue;
        if ((int)strlen(syms[i].name) != name_len) continue;
        if (memcmp(syms[i].name, name, (size_t)name_len) == 0) return &syms[i];
    }
    return NULL;
}

static bool collect_public_exports_from_ast(AstNode* ast, AbiSymbol** out_syms, int* out_count, bool* out_has_public) {
    *out_syms = NULL;
    *out_count = 0;
    *out_has_public = false;
    int cap = 0;

    if (!ast || ast->type != AST_PROGRAM) return true;
    for (int i = 0; i < ast->data.block.count; i++) {
        AstNode* stmt = ast->data.block.statements[i];
        if (!stmt) continue;

        if (stmt->type == AST_FUNC_DECL && stmt->data.func_decl.is_public) {
            *out_has_public = true;
            char return_type[MAX_NAME_LEN];
            char effects[MAX_ABI_EFFECTS_LEN];
            copy_token_text(return_type, sizeof(return_type), stmt->data.func_decl.return_type);
            if (!format_ast_effects(stmt->data.func_decl.effects, stmt->data.func_decl.effect_count,
                                    effects, sizeof(effects))) {
                free(*out_syms);
                *out_syms = NULL;
                *out_count = 0;
                return false;
            }
            if (!append_abi_symbol(out_syms, out_count, &cap, ABI_KIND_FUNCTION,
                                   stmt->data.func_decl.name.start,
                                   stmt->data.func_decl.name.length, 0,
                                   return_type, effects)) {
                free(*out_syms);
                *out_syms = NULL;
                *out_count = 0;
                return false;
            }
        } else if (stmt->type == AST_STRUCT_DECL && stmt->data.struct_decl.is_public) {
            *out_has_public = true;
            if (!append_abi_symbol(out_syms, out_count, &cap, ABI_KIND_STRUCT,
                                   stmt->data.struct_decl.name.start,
                                   stmt->data.struct_decl.name.length, 0,
                                   "", "-")) {
                free(*out_syms);
                *out_syms = NULL;
                *out_count = 0;
                return false;
            }
        }
    }
    return true;
}

static const char* abi_kind_tag(int kind) {
    return (kind == ABI_KIND_FUNCTION) ? "fn" : "st";
}

static const char* abi_kind_label(int kind) {
    return (kind == ABI_KIND_FUNCTION) ? "function" : "struct";
}

static bool abi_change_is_breaking(int kind, int old_metric, int new_metric, const char** out_reason) {
    (void)old_metric;
    (void)new_metric;
    if (out_reason) {
        if (kind == ABI_KIND_FUNCTION) {
            *out_reason = "function signature changed";
        } else {
            *out_reason = "struct layout changed";
        }
    }
    return true;
}

static bool abi_entry_contract_equal(const AbiEntry* lhs, const AbiEntry* rhs) {
    if (!lhs || !rhs) return false;
    return lhs->metric == rhs->metric &&
           strcmp(lhs->return_type, rhs->return_type) == 0 &&
           strcmp(lhs->effects, rhs->effects) == 0;
}

static bool format_abi_contract(const char* return_type, const char* effects,
                                char* out, size_t out_size) {
    size_t len = 0;
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    return append_fmt(out, out_size, &len, "ret=%s eff=%s",
                      (return_type && return_type[0] != '\0') ? return_type : "-",
                      (effects && effects[0] != '\0') ? effects : "-");
}

static bool parse_abi_optional_fields(char* token, char* out_return_type, size_t return_type_size,
                                      char* out_effects, size_t effects_size) {
    if (!out_return_type || !out_effects || return_type_size == 0 || effects_size == 0) return false;
    if (!token) return true;

    if (strncmp(token, "ret=", 4) == 0) {
        copy_text(out_return_type, return_type_size, token + 4);
        return true;
    }
    if (strncmp(token, "eff=", 4) == 0) {
        copy_text(out_effects, effects_size, token + 4);
        return true;
    }
    return false;
}

static bool write_module_abi_file(const char* path, ObjFunction* main_fn,
                                  const AbiSymbol* public_exports, int public_count, bool only_public) {
    if (!path || !main_fn) return false;

    AbiSymbol* syms = NULL;
    int sym_count = 0;
    int sym_cap = 0;

    for (int i = 0; i < main_fn->chunk.constant_count; i++) {
        Value v = main_fn->chunk.constants[i];
        if (v.type != VAL_OBJ || v.as.obj == NULL) continue;

        Obj* obj = v.as.obj;
        if (obj->type == OBJ_FUNCTION) {
            ObjFunction* fn = (ObjFunction*)obj;
            if (fn->name_len == 8 && memcmp(fn->name, "__main__", 8) == 0) continue;
            if (only_public &&
                !abi_symbol_has_name(public_exports, public_count, ABI_KIND_FUNCTION,
                                     fn->name, fn->name_len)) {
                continue;
            }
            const AbiSymbol* meta = find_abi_symbol_by_name_len(public_exports, public_count,
                                                                ABI_KIND_FUNCTION, fn->name, fn->name_len);
            if (!append_abi_symbol(&syms, &sym_count, &sym_cap, ABI_KIND_FUNCTION,
                                   fn->name, fn->name_len, fn->arity,
                                   meta ? meta->return_type : "",
                                   meta ? meta->effects : "-")) {
                free(syms);
                return false;
            }
        } else if (obj->type == OBJ_STRUCT) {
            ObjStruct* st = (ObjStruct*)obj;
            if (only_public &&
                !abi_symbol_has_name(public_exports, public_count, ABI_KIND_STRUCT,
                                     st->name, st->name_len)) {
                continue;
            }
            if (!append_abi_symbol(&syms, &sym_count, &sym_cap, ABI_KIND_STRUCT,
                                   st->name, st->name_len, st->field_count, "", "-")) {
                free(syms);
                return false;
            }
        }
    }

    if (sym_count > 1) {
        qsort(syms, (size_t)sym_count, sizeof(AbiSymbol), abi_symbol_cmp);
    }

    FILE* out = fopen(path, "wb");
    if (!out) {
        free(syms);
        return false;
    }

    bool ok = fprintf(out, "# viper abi v2\n") > 0;
    for (int i = 0; ok && i < sym_count; i++) {
        if (syms[i].kind == ABI_KIND_FUNCTION) {
            ok = fprintf(out, "fn %s %d ret=%s eff=%s\n",
                         syms[i].name, syms[i].metric,
                         syms[i].return_type[0] != '\0' ? syms[i].return_type : "-",
                         syms[i].effects[0] != '\0' ? syms[i].effects : "-") > 0;
        } else {
            ok = fprintf(out, "st %s %d\n", syms[i].name, syms[i].metric) > 0;
        }
    }

    fclose(out);
    free(syms);
    return ok;
}

static bool build_package_artifacts_for_source_file(const char* vp_path) {
    char* source = NULL;
    if (!read_text_file(vp_path, &source)) return false;

    char vbc_path[MAX_PATH_LEN];
    char vbb_path[MAX_PATH_LEN];
    char vabi_path[MAX_PATH_LEN];
    if (!vp_to_vbc_path(vp_path, vbc_path, sizeof(vbc_path)) ||
        !vp_to_vbb_path(vp_path, vbb_path, sizeof(vbb_path)) ||
        !vp_to_vabi_path(vp_path, vabi_path, sizeof(vabi_path))) {
        free(source);
        return false;
    }

    bool ok = write_source_cache_file(vbc_path, source);
    if (!ok) {
        free(source);
        return false;
    }

    // Build executable module bytecode for parse-free package imports.
    compiler_set_contract_output(false);
    compiler_set_emit_halt(false);
    compiler_set_entry_file(vp_path);
    AstNode* ast = parse(source);
    AbiSymbol* public_exports = NULL;
    int public_count = 0;
    bool has_public = false;
    if (!collect_public_exports_from_ast(ast, &public_exports, &public_count, &has_public)) {
        compiler_set_emit_halt(true);
        compiler_set_contract_output(true);
        free(source);
        return false;
    }
    init_native_core();
    ObjFunction* main_fn = compile(ast);
    compiler_set_emit_halt(true);
    compiler_set_contract_output(true);
    ok = write_bytecode_file(vbb_path, main_fn);
    if (ok) {
        ok = write_module_abi_file(vabi_path, main_fn, public_exports, public_count, has_public);
    }

    free(public_exports);
    free(source);
    return ok;
}

static bool build_package_artifacts_tree(const char* root_dir, int* built_count) {
    DIR* dir = opendir(root_dir);
    if (!dir) return false;

    bool ok = true;
    struct dirent* entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

        char path[MAX_PATH_LEN];
        if (!join_path(root_dir, entry->d_name, path, sizeof(path))) {
            ok = false;
            break;
        }

        if (is_directory(path)) {
            if (!build_package_artifacts_tree(path, built_count)) {
                ok = false;
                break;
            }
            continue;
        }

        if (has_suffix(path, ".vp")) {
            if (!build_package_artifacts_for_source_file(path)) {
                ok = false;
                break;
            }
            (*built_count)++;
        }
    }

    closedir(dir);
    return ok;
}

static bool build_package_artifacts_for_dependencies(const char* project_root, const Dependency* deps,
                                                     int dep_count, int* built_count) {
    *built_count = 0;
    for (int i = 0; i < dep_count; i++) {
        char p1[MAX_PATH_LEN];
        char p2[MAX_PATH_LEN];
        char p3[MAX_PATH_LEN];
        char version_dir[MAX_PATH_LEN];
        if (!join_path(project_root, ".viper", p1, sizeof(p1)) ||
            !join_path(p1, "packages", p2, sizeof(p2)) ||
            !join_path(p2, deps[i].name, p3, sizeof(p3)) ||
            !join_path(p3, "versions", p1, sizeof(p1)) ||
            !join_path(p1, deps[i].version, version_dir, sizeof(version_dir))) {
            return false;
        }

        if (!is_directory(version_dir)) {
            return false;
        }

        if (!build_package_artifacts_tree(version_dir, built_count)) {
            return false;
        }
    }
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

static bool load_abi_file_symbols(const char* path, AbiSymbol** out_syms, int* out_count) {
    *out_syms = NULL;
    *out_count = 0;
    int cap = 0;

    FILE* file = fopen(path, "r");
    if (!file) return false;

    bool ok = true;
    int line_no = 0;
    char line[MAX_LINE_LEN];
    while (fgets(line, sizeof(line), file) != NULL) {
        line_no++;
        char* p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '\0' || *p == '#') continue;

        char* kind_tok = strtok(p, " \t\r\n");
        char* name_tok = strtok(NULL, " \t\r\n");
        char* metric_tok = strtok(NULL, " \t\r\n");
        if (!kind_tok || !name_tok || !metric_tok) {
            fprintf(stderr, "vpm: invalid ABI line at %s:%d\n", path, line_no);
            ok = false;
            break;
        }

        int kind = 0;
        if (strcmp(kind_tok, "fn") == 0) {
            kind = ABI_KIND_FUNCTION;
        } else if (strcmp(kind_tok, "st") == 0) {
            kind = ABI_KIND_STRUCT;
        } else {
            fprintf(stderr, "vpm: invalid ABI symbol kind at %s:%d\n", path, line_no);
            ok = false;
            break;
        }

        int metric = 0;
        if (!parse_metric_token(metric_tok, &metric)) {
            fprintf(stderr, "vpm: invalid ABI metric at %s:%d\n", path, line_no);
            ok = false;
            break;
        }

        char return_type[MAX_NAME_LEN];
        char effects[MAX_ABI_EFFECTS_LEN];
        return_type[0] = '\0';
        copy_text(effects, sizeof(effects), "-");
        char* extra_tok = NULL;
        while ((extra_tok = strtok(NULL, " \t\r\n")) != NULL) {
            if (!parse_abi_optional_fields(extra_tok, return_type, sizeof(return_type),
                                           effects, sizeof(effects))) {
                fprintf(stderr, "vpm: invalid ABI line at %s:%d\n", path, line_no);
                ok = false;
                break;
            }
        }
        if (!ok) break;

        int name_len = (int)strlen(name_tok);
        if (!append_abi_symbol_strict(out_syms, out_count, &cap, kind, name_tok, name_len, metric,
                                      return_type, effects)) {
            fprintf(stderr, "vpm: conflicting ABI symbol at %s:%d\n", path, line_no);
            ok = false;
            break;
        }
    }

    if (ok && ferror(file)) ok = false;
    fclose(file);
    if (!ok) {
        free(*out_syms);
        *out_syms = NULL;
        *out_count = 0;
    }
    return ok;
}

static bool collect_bytecode_symbols(ObjFunction* root, AbiSymbol** out_syms, int* out_count) {
    *out_syms = NULL;
    *out_count = 0;
    int sym_cap = 0;

    Obj** queue = NULL;
    int q_count = 0;
    int q_cap = 0;

    if (!ensure_capacity((void**)&queue, &q_cap, 1, sizeof(Obj*))) {
        free(queue);
        return false;
    }
    queue[q_count++] = (Obj*)root;

    bool ok = true;
    for (int i = 0; i < q_count; i++) {
        Obj* obj = queue[i];
        if (!obj) continue;

        if (obj->type == OBJ_FUNCTION) {
            ObjFunction* fn = (ObjFunction*)obj;
            bool is_main = fn->name_len == 8 && memcmp(fn->name, "__main__", 8) == 0;
            if (!is_main &&
                !append_abi_symbol_strict(out_syms, out_count, &sym_cap,
                                          ABI_KIND_FUNCTION, fn->name, fn->name_len, fn->arity,
                                          "", "-")) {
                ok = false;
                break;
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
                    ok = false;
                    break;
                }
                queue[q_count++] = dep;
            }
        } else if (obj->type == OBJ_STRUCT) {
            ObjStruct* st = (ObjStruct*)obj;
            if (!append_abi_symbol_strict(out_syms, out_count, &sym_cap,
                                          ABI_KIND_STRUCT, st->name, st->name_len, st->field_count,
                                          "", "-")) {
                ok = false;
                break;
            }
        }
    }

    free(queue);
    if (!ok) {
        free(*out_syms);
        *out_syms = NULL;
        *out_count = 0;
    }
    return ok;
}

static bool verify_module_abi_pair(const char* vbb_path, const char* vabi_path) {
    AbiSymbol* expected = NULL;
    int expected_count = 0;
    if (!load_abi_file_symbols(vabi_path, &expected, &expected_count)) {
        fprintf(stderr, "vpm: failed to parse ABI file %s\n", vabi_path);
        return false;
    }

    ObjFunction* root = read_bytecode_file(vbb_path);
    if (!root) {
        fprintf(stderr, "vpm: failed to read bytecode file %s\n", vbb_path);
        free(expected);
        return false;
    }

    AbiSymbol* actual = NULL;
    int actual_count = 0;
    if (!collect_bytecode_symbols(root, &actual, &actual_count)) {
        fprintf(stderr, "vpm: failed to collect bytecode symbols from %s\n", vbb_path);
        free(expected);
        return false;
    }

    bool ok = true;
    for (int i = 0; i < expected_count; i++) {
        int found = find_abi_symbol(actual, actual_count, expected[i].kind, expected[i].name);
        if (found < 0) {
            fprintf(stderr, "vpm: ABI mismatch (%s): missing %s '%s'\n",
                    vbb_path, abi_kind_label(expected[i].kind), expected[i].name);
            ok = false;
            continue;
        }
        if (actual[found].metric != expected[i].metric) {
            fprintf(stderr, "vpm: ABI mismatch (%s): %s '%s' metric %d != %d\n",
                    vbb_path, abi_kind_label(expected[i].kind), expected[i].name,
                    actual[found].metric, expected[i].metric);
            ok = false;
        }
    }

    free(expected);
    free(actual);
    return ok;
}

static bool vbb_to_vabi_path(const char* vbb_path, char* out, size_t out_size) {
    if (!vbb_path || !out || out_size == 0) return false;
    if (!has_suffix(vbb_path, ".vbb")) return false;

    size_t n = strlen(vbb_path);
    if (n + 2 > out_size) return false; // replace ".vbb" with ".vabi"
    memcpy(out, vbb_path, n - 3);
    out[n - 3] = 'v';
    out[n - 2] = 'a';
    out[n - 1] = 'b';
    out[n] = 'i';
    out[n + 1] = '\0';
    return true;
}

static bool check_abi_tree(const char* root_dir, int* checked_count, int* failed_count) {
    DIR* dir = opendir(root_dir);
    if (!dir) return false;

    bool ok = true;
    struct dirent* entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

        char path[MAX_PATH_LEN];
        if (!join_path(root_dir, entry->d_name, path, sizeof(path))) {
            ok = false;
            break;
        }

        if (is_directory(path)) {
            if (!check_abi_tree(path, checked_count, failed_count)) {
                ok = false;
                break;
            }
            continue;
        }

        if (!has_suffix(path, ".vbb")) continue;

        (*checked_count)++;
        char vabi_path[MAX_PATH_LEN];
        if (!vbb_to_vabi_path(path, vabi_path, sizeof(vabi_path)) || !file_exists(vabi_path)) {
            fprintf(stderr, "vpm: missing ABI file for module %s\n", path);
            (*failed_count)++;
            continue;
        }
        if (!verify_module_abi_pair(path, vabi_path)) {
            (*failed_count)++;
        }
    }

    closedir(dir);
    return ok;
}

static bool dependency_version_dir(const char* project_root, const char* pkg, const char* ver,
                                   char* out, size_t out_size) {
    char p1[MAX_PATH_LEN];
    char p2[MAX_PATH_LEN];
    char p3[MAX_PATH_LEN];
    if (!join_path(project_root, ".viper", p1, sizeof(p1)) ||
        !join_path(p1, "packages", p2, sizeof(p2)) ||
        !join_path(p2, pkg, p3, sizeof(p3)) ||
        !join_path(p3, "versions", p1, sizeof(p1)) ||
        !join_path(p1, ver, out, out_size)) {
        return false;
    }
    return true;
}

static int find_abi_entry(const AbiEntry* entries, int count, const char* module, int kind, const char* name) {
    if (!entries || !module || !name) return -1;
    for (int i = 0; i < count; i++) {
        if (entries[i].kind != kind) continue;
        if (strcmp(entries[i].module, module) != 0) continue;
        if (strcmp(entries[i].name, name) != 0) continue;
        return i;
    }
    return -1;
}

static bool append_abi_entry(AbiEntry** entries, int* count, int* cap,
                             const char* module, int kind, const char* name, int metric,
                             const char* return_type, const char* effects) {
    if (!entries || !count || !cap || !module || !name || metric < 0) return false;
    if (strlen(module) >= MAX_PATH_LEN || strlen(name) >= MAX_NAME_LEN) return false;

    int existing = find_abi_entry(*entries, *count, module, kind, name);
    if (existing >= 0) {
        if ((*entries)[existing].metric != metric ||
            strcmp((*entries)[existing].return_type, return_type ? return_type : "") != 0 ||
            strcmp((*entries)[existing].effects, effects ? effects : "-") != 0) {
            return false;
        }
        return true;
    }

    if (*count + 1 > *cap) {
        int next = (*cap == 0) ? 8 : *cap * 2;
        AbiEntry* grown = (AbiEntry*)realloc(*entries, sizeof(AbiEntry) * (size_t)next);
        if (!grown) return false;
        *entries = grown;
        *cap = next;
    }

    AbiEntry* out = &(*entries)[*count];
    copy_text(out->module, sizeof(out->module), module);
    out->kind = kind;
    copy_text(out->name, sizeof(out->name), name);
    out->metric = metric;
    copy_text(out->return_type, sizeof(out->return_type), return_type ? return_type : "");
    copy_text(out->effects, sizeof(out->effects), effects ? effects : "-");
    (*count)++;
    return true;
}

static int abi_entry_cmp(const void* a, const void* b) {
    const AbiEntry* lhs = (const AbiEntry*)a;
    const AbiEntry* rhs = (const AbiEntry*)b;
    int c = strcmp(lhs->module, rhs->module);
    if (c != 0) return c;
    if (lhs->kind != rhs->kind) return lhs->kind - rhs->kind;
    c = strcmp(lhs->name, rhs->name);
    if (c != 0) return c;
    return lhs->metric - rhs->metric;
}

static bool relative_path(const char* base, const char* path, char* out, size_t out_size) {
    if (!base || !path || !out || out_size == 0) return false;
    size_t base_len = strlen(base);
    if (strncmp(path, base, base_len) == 0) {
        const char* rel = path + base_len;
        if (*rel == '/') rel++;
        if (*rel != '\0') {
            copy_text(out, out_size, rel);
            return true;
        }
    }
    copy_text(out, out_size, path);
    return true;
}

static bool collect_package_abi_entries_tree(const char* root_dir, const char* base_dir,
                                             AbiEntry** entries, int* count, int* cap) {
    DIR* dir = opendir(root_dir);
    if (!dir) return false;

    bool ok = true;
    struct dirent* entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

        char path[MAX_PATH_LEN];
        if (!join_path(root_dir, entry->d_name, path, sizeof(path))) {
            ok = false;
            break;
        }

        if (is_directory(path)) {
            if (!collect_package_abi_entries_tree(path, base_dir, entries, count, cap)) {
                ok = false;
                break;
            }
            continue;
        }

        if (!has_suffix(path, ".vabi")) continue;

        AbiSymbol* symbols = NULL;
        int symbol_count = 0;
        if (!load_abi_file_symbols(path, &symbols, &symbol_count)) {
            ok = false;
            break;
        }

        char module_rel[MAX_PATH_LEN];
        if (!relative_path(base_dir, path, module_rel, sizeof(module_rel))) {
            free(symbols);
            ok = false;
            break;
        }

        for (int i = 0; i < symbol_count; i++) {
            if (!append_abi_entry(entries, count, cap, module_rel,
                                  symbols[i].kind, symbols[i].name, symbols[i].metric,
                                  symbols[i].return_type, symbols[i].effects)) {
                fprintf(stderr, "vpm: conflicting ABI entry in %s\n", path);
                free(symbols);
                ok = false;
                break;
            }
        }
        free(symbols);
        if (!ok) break;
    }

    closedir(dir);
    return ok;
}

static bool collect_package_abi_entries(const char* version_dir, AbiEntry** entries, int* count) {
    *entries = NULL;
    *count = 0;
    int cap = 0;
    if (!collect_package_abi_entries_tree(version_dir, version_dir, entries, count, &cap)) {
        free(*entries);
        *entries = NULL;
        *count = 0;
        return false;
    }
    if (*count > 1) {
        qsort(*entries, (size_t)(*count), sizeof(AbiEntry), abi_entry_cmp);
    }
    return true;
}

static bool append_text(char** buf, size_t* len, size_t* cap, const char* text) {
    size_t n = strlen(text);
    size_t need = *len + n + 1;
    if (need > *cap) {
        size_t next = (*cap == 0) ? 256 : *cap;
        while (next < need) next *= 2;
        char* grown = (char*)realloc(*buf, next);
        if (!grown) return false;
        *buf = grown;
        *cap = next;
    }
    memcpy(*buf + *len, text, n);
    *len += n;
    (*buf)[*len] = '\0';
    return true;
}

static int dependency_cmp(const void* a, const void* b) {
    const Dependency* lhs = (const Dependency*)a;
    const Dependency* rhs = (const Dependency*)b;
    return strcmp(lhs->name, rhs->name);
}

static void sort_dependencies(Dependency* deps, int dep_count) {
    if (!deps || dep_count <= 1) return;
    qsort(deps, (size_t)dep_count, sizeof(Dependency), dependency_cmp);
}

static int find_dependency_index(const Dependency* deps, int dep_count, const char* pkg) {
    if (!deps || !pkg) return -1;
    for (int i = 0; i < dep_count; i++) {
        if (strcmp(deps[i].name, pkg) == 0) return i;
    }
    return -1;
}

static bool load_manifest_dependencies(const char* manifest_path, Dependency** out_deps, int* out_count) {
    *out_deps = NULL;
    *out_count = 0;

    char* text = NULL;
    if (!read_text_file(manifest_path, &text)) return false;

    int cap = 0;
    Dependency* deps = NULL;

    for (char* line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
        char pkg[MAX_NAME_LEN];
        char ver[MAX_VERSION_LEN];
        if (!parse_use_line(line, pkg, sizeof(pkg), ver, sizeof(ver))) continue;

        int idx = -1;
        for (int i = 0; i < *out_count; i++) {
            if (strcmp(deps[i].name, pkg) == 0) {
                idx = i;
                break;
            }
        }

        if (idx >= 0) {
            copy_text(deps[idx].version, sizeof(deps[idx].version), ver);
            continue;
        }

        if (*out_count + 1 > cap) {
            int next = (cap == 0) ? 8 : cap * 2;
            Dependency* grown = (Dependency*)realloc(deps, sizeof(Dependency) * (size_t)next);
            if (!grown) {
                free(deps);
                free(text);
                return false;
            }
            deps = grown;
            cap = next;
        }

        copy_text(deps[*out_count].name, sizeof(deps[*out_count].name), pkg);
        copy_text(deps[*out_count].version, sizeof(deps[*out_count].version), ver);
        (*out_count)++;
    }

    free(text);
    sort_dependencies(deps, *out_count);
    *out_deps = deps;
    return true;
}

static bool save_manifest_set_dependency(const char* manifest_path, const char* pkg, const char* ver) {
    char* old = NULL;
    if (!read_text_file(manifest_path, &old)) return false;

    char* out = NULL;
    size_t len = 0;
    size_t cap = 0;
    bool replaced = false;

    for (char* line = strtok(old, "\n"); line; line = strtok(NULL, "\n")) {
        char lpkg[MAX_NAME_LEN];
        char lver[MAX_VERSION_LEN];
        char newline[MAX_LINE_LEN];

        if (parse_use_line(line, lpkg, sizeof(lpkg), lver, sizeof(lver)) && strcmp(lpkg, pkg) == 0) {
            snprintf(newline, sizeof(newline), "use %s(%s)\n", pkg, ver);
            replaced = true;
        } else {
            snprintf(newline, sizeof(newline), "%s\n", line);
        }

        if (!append_text(&out, &len, &cap, newline)) {
            free(old);
            free(out);
            return false;
        }
    }

    if (!replaced) {
        char addline[MAX_LINE_LEN];
        snprintf(addline, sizeof(addline), "use %s(%s)\n", pkg, ver);
        if (!append_text(&out, &len, &cap, addline)) {
            free(old);
            free(out);
            return false;
        }
    }

    bool ok = write_text_file(manifest_path, out ? out : "");
    free(old);
    free(out);
    return ok;
}

static bool save_manifest_remove_dependency(const char* manifest_path, const char* pkg, bool* removed) {
    *removed = false;

    char* old = NULL;
    if (!read_text_file(manifest_path, &old)) return false;

    char* out = NULL;
    size_t len = 0;
    size_t cap = 0;

    for (char* line = strtok(old, "\n"); line; line = strtok(NULL, "\n")) {
        char lpkg[MAX_NAME_LEN];
        char lver[MAX_VERSION_LEN];
        if (parse_use_line(line, lpkg, sizeof(lpkg), lver, sizeof(lver)) && strcmp(lpkg, pkg) == 0) {
            *removed = true;
            continue;
        }

        char newline[MAX_LINE_LEN];
        snprintf(newline, sizeof(newline), "%s\n", line);
        if (!append_text(&out, &len, &cap, newline)) {
            free(old);
            free(out);
            return false;
        }
    }

    bool ok = write_text_file(manifest_path, out ? out : "");
    free(old);
    free(out);
    return ok;
}

static bool write_lockfile(const char* project_root, const Dependency* deps, int dep_count) {
    char lock_path[MAX_PATH_LEN];
    if (!join_path(project_root, "viper.lock", lock_path, sizeof(lock_path))) return false;

    char* out = NULL;
    size_t len = 0;
    size_t cap = 0;

    if (!append_text(&out, &len, &cap, "# viper lockfile v1\n")) {
        free(out);
        return false;
    }

    for (int i = 0; i < dep_count; i++) {
        char line[MAX_LINE_LEN];
        snprintf(line, sizeof(line), "%s %s\n", deps[i].name, deps[i].version);
        if (!append_text(&out, &len, &cap, line)) {
            free(out);
            return false;
        }
    }

    bool ok = write_text_file(lock_path, out ? out : "");
    free(out);
    return ok;
}

static bool install_dependency_stub(const char* project_root, const char* pkg, const char* ver) {
    char p1[MAX_PATH_LEN];
    char p2[MAX_PATH_LEN];
    char pkg_root[MAX_PATH_LEN];
    char versions_dir[MAX_PATH_LEN];
    char version_dir[MAX_PATH_LEN];

    if (!join_path(project_root, ".viper", p1, sizeof(p1)) ||
        !join_path(p1, "packages", p2, sizeof(p2)) ||
        !join_path(p2, pkg, pkg_root, sizeof(pkg_root)) ||
        !join_path(pkg_root, "versions", versions_dir, sizeof(versions_dir)) ||
        !join_path(versions_dir, ver, version_dir, sizeof(version_dir))) {
        return false;
    }

    if (!ensure_dir_recursive(version_dir)) return false;

    char index_file[MAX_PATH_LEN];
    if (!join_path(version_dir, "index.vp", index_file, sizeof(index_file))) return false;

    if (strstr(pkg, "github.com/") != NULL || strstr(pkg, "gitlab.com/") != NULL || strncmp(pkg, "http", 4) == 0) {
        if (!file_exists(index_file)) {
            char clone_cmd[2048];
            char url[1024];
            char q_branch[128];
            char q_url[1200];
            char q_version_dir[1200];
            if (strncmp(pkg, "http", 4) == 0) snprintf(url, sizeof(url), "%s", pkg);
            else snprintf(url, sizeof(url), "https://%s", pkg);
            const char* primary_branch = (strcmp(ver, "latest") == 0) ? "main" : ver;
            const char* fallback_branch = (strcmp(ver, "latest") == 0) ? "master" : ver;

            if (!shell_quote_double(primary_branch, q_branch, sizeof(q_branch)) ||
                !shell_quote_double(url, q_url, sizeof(q_url)) ||
                !shell_quote_double(version_dir, q_version_dir, sizeof(q_version_dir))) {
                fprintf(stderr, "vpm: package path is too long for shell command.\n");
                return false;
            }
            
            size_t clone_len = 0;
            if (!append_fmt(clone_cmd, sizeof(clone_cmd), &clone_len,
                            "git clone --depth 1 --branch %s %s %s",
                            q_branch, q_url, q_version_dir)) {
                fprintf(stderr, "vpm: clone command is too long.\n");
                return false;
            }
            printf("vpm: Fetching remote package %s...\n", pkg);
            int res = system(clone_cmd);
            if (res != 0) {
                // fallback to master if main fails
                if (!shell_quote_double(fallback_branch, q_branch, sizeof(q_branch))) {
                    fprintf(stderr, "vpm: package branch is too long for shell command.\n");
                    return false;
                }
                clone_len = 0;
                if (!append_fmt(clone_cmd, sizeof(clone_cmd), &clone_len,
                                "git clone --depth 1 --branch %s %s %s",
                                q_branch, q_url, q_version_dir)) {
                    fprintf(stderr, "vpm: clone command is too long.\n");
                    return false;
                }
                res = system(clone_cmd);
            }
            if (res != 0) {
                fprintf(stderr, "vpm: Failed to fetch %s\n", pkg);
                return false;
            }
        }
    } else {
        if (!file_exists(index_file)) {
            char stub[2048];
            snprintf(stub, sizeof(stub),
                     "// package: %s\n"
                     "// version: %s\n"
                     "// stub module generated by vpm install\n",
                     pkg, ver);
            if (!write_text_file(index_file, stub)) return false;
        }
    }

    char current_file[MAX_PATH_LEN];
    if (!join_path(pkg_root, "current", current_file, sizeof(current_file))) return false;
    if (!write_text_file(current_file, ver)) return false;

    return true;
}

static bool prune_package_versions(const char* project_root, const char* pkg, const char* keep_version) {
    char p1[MAX_PATH_LEN];
    char p2[MAX_PATH_LEN];
    char package_root[MAX_PATH_LEN];
    char versions_dir[MAX_PATH_LEN];

    if (!join_path(project_root, ".viper", p1, sizeof(p1)) ||
        !join_path(p1, "packages", p2, sizeof(p2)) ||
        !join_path(p2, pkg, package_root, sizeof(package_root)) ||
        !join_path(package_root, "versions", versions_dir, sizeof(versions_dir))) {
        return false;
    }

    if (!is_directory(versions_dir)) return true;

    DIR* dir = opendir(versions_dir);
    if (!dir) return false;

    bool ok = true;
    struct dirent* entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        if (strcmp(entry->d_name, keep_version) == 0) continue;

        char stale_version_path[MAX_PATH_LEN];
        if (!join_path(versions_dir, entry->d_name, stale_version_path, sizeof(stale_version_path)) ||
            !remove_path_recursive(stale_version_path)) {
            ok = false;
            break;
        }
    }

    closedir(dir);
    return ok;
}

static bool prune_package_store(const char* project_root, const Dependency* deps, int dep_count) {
    char p1[MAX_PATH_LEN];
    char packages_root[MAX_PATH_LEN];
    if (!join_path(project_root, ".viper", p1, sizeof(p1)) ||
        !join_path(p1, "packages", packages_root, sizeof(packages_root))) {
        return false;
    }

    if (!is_directory(packages_root)) return true;

    DIR* dir = opendir(packages_root);
    if (!dir) return false;

    bool ok = true;
    struct dirent* entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

        char package_path[MAX_PATH_LEN];
        if (!join_path(packages_root, entry->d_name, package_path, sizeof(package_path))) {
            ok = false;
            break;
        }

        if (!is_directory(package_path)) continue;
        if (find_dependency_index(deps, dep_count, entry->d_name) >= 0) continue;

        if (!remove_path_recursive(package_path)) {
            ok = false;
            break;
        }
    }

    closedir(dir);
    return ok;
}

static bool run_install(const char* project_root, const char* manifest_path, int* installed_count) {
    *installed_count = 0;

    Dependency* deps = NULL;
    int dep_count = 0;
    if (!load_manifest_dependencies(manifest_path, &deps, &dep_count)) return false;

    char p1[MAX_PATH_LEN];
    char packages_root[MAX_PATH_LEN];
    if (!join_path(project_root, ".viper", p1, sizeof(p1)) ||
        !join_path(p1, "packages", packages_root, sizeof(packages_root)) ||
        !ensure_dir_recursive(packages_root)) {
        free(deps);
        return false;
    }

    if (!prune_package_store(project_root, deps, dep_count)) {
        free(deps);
        return false;
    }

    for (int i = 0; i < dep_count; i++) {
        if (!install_dependency_stub(project_root, deps[i].name, deps[i].version) ||
            !prune_package_versions(project_root, deps[i].name, deps[i].version)) {
            free(deps);
            return false;
        }
        (*installed_count)++;
    }

    bool ok = write_lockfile(project_root, deps, dep_count);
    if (ok) {
        int built_count = 0;
        ok = build_package_artifacts_for_dependencies(project_root, deps, dep_count, &built_count);
    }
    free(deps);
    return ok;
}

static void print_pkg_usage(void) {
    printf("Usage:\n");
    printf("  viper pkg init [project_name]\n");
    printf("  viper pkg add <package> [version]\n");
    printf("  viper pkg remove <package>\n");
    printf("  viper pkg install\n");
    printf("  viper pkg lock\n");
    printf("  viper pkg list\n");
    printf("  viper pkg build\n");
    printf("  viper pkg abi check\n");
    printf("  viper pkg abi diff <package> <from_version> <to_version> [--fail-on-breaking]\n");
}

static bool copy_file_if_exists(const char* src, const char* dst) {
    FILE* in = fopen(src, "rb");
    if (!in) return false;
    FILE* out = fopen(dst, "wb");
    if (!out) { fclose(in); return false; }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        fwrite(buf, 1, n, out);
    }
    fclose(in);
    fclose(out);
    return true;
}

static int cmd_init(const char* project_name) {
    char cwd[MAX_PATH_LEN];
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        fprintf(stderr, "vpm: could not resolve current directory.\n");
        return 1;
    }

    char manifest[MAX_PATH_LEN];
    if (!join_path(cwd, "viper.vpmod", manifest, sizeof(manifest))) {
        fprintf(stderr, "vpm: path is too long.\n");
        return 1;
    }

    if (!file_exists(manifest)) {
        const char* name = (project_name && project_name[0] != '\0') ? project_name : "viper_project";
        char content[2048];
        snprintf(content, sizeof(content),
                 "@project(\"%s\" v:0.1.0)\n"
                 "\n"
                 "// Managed by Viper Package Manager\n",
                 name);
        if (!write_text_file(manifest, content)) {
            fprintf(stderr, "vpm: failed to write viper.vpmod.\n");
            return 1;
        }
        printf("vpm: created viper.vpmod\n");
    } else {
        printf("vpm: viper.vpmod already exists\n");
    }

    char packages_dir[MAX_PATH_LEN];
    if (!join_path(cwd, ".viper/packages", packages_dir, sizeof(packages_dir))) {
        fprintf(stderr, "vpm: path is too long.\n");
        return 1;
    }

    if (!ensure_dir_recursive(packages_dir)) {
        fprintf(stderr, "vpm: failed to create .viper/packages\n");
        return 1;
    }

    char lock_path[MAX_PATH_LEN];
    if (!join_path(cwd, "viper.lock", lock_path, sizeof(lock_path))) {
        fprintf(stderr, "vpm: path is too long.\n");
        return 1;
    }
    if (!file_exists(lock_path)) {
        if (!write_text_file(lock_path, "# viper lockfile v1\n")) {
            fprintf(stderr, "vpm: failed to initialize viper.lock\n");
            return 1;
        }
        printf("vpm: created viper.lock\n");
    }

    printf("vpm: initialized package store at .viper/packages\n");

    // Auto-generate LLM_REFERENCE.md for AI assistants
    char llm_ref_dst[MAX_PATH_LEN];
    if (join_path(cwd, "LLM_REFERENCE.md", llm_ref_dst, sizeof(llm_ref_dst)) &&
        !file_exists(llm_ref_dst)) {
        bool copied = false;
        // Try 1: system install path
        copied = copy_file_if_exists("/usr/local/lib/viper/LLM_REFERENCE.md", llm_ref_dst);
        // Try 2: relative to CWD
        if (!copied) copied = copy_file_if_exists("docs/LLM_REFERENCE.md", llm_ref_dst);
        // Try 3: relative to executable
        if (!copied) {
            char exe_path[MAX_PATH_LEN];
            ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
            if (len > 0) {
                exe_path[len] = '\0';
                char exe_dir[MAX_PATH_LEN];
                path_dirname(exe_path, exe_dir, sizeof(exe_dir));
                char ref_path[MAX_PATH_LEN];
                if (join_path(exe_dir, "docs/LLM_REFERENCE.md", ref_path, sizeof(ref_path))) {
                    copied = copy_file_if_exists(ref_path, llm_ref_dst);
                }
            }
        }
        if (copied) {
            printf("vpm: created LLM_REFERENCE.md (AI language guide)\n");
        }
    }

    return 0;
}

static int cmd_add(const char* pkg, const char* version) {
    if (!is_valid_package_name(pkg)) {
        fprintf(stderr, "vpm: invalid package name '%s'\n", pkg ? pkg : "");
        return 1;
    }

    const char* ver = (version && version[0] != '\0') ? version : "latest";
    if (!is_valid_version(ver)) {
        fprintf(stderr, "vpm: invalid version '%s'\n", ver);
        return 1;
    }

    char root[MAX_PATH_LEN];
    if (!find_project_root(root, sizeof(root))) {
        fprintf(stderr, "vpm: no viper.vpmod found (run 'viper pkg init').\n");
        return 1;
    }

    char manifest[MAX_PATH_LEN];
    if (!join_path(root, "viper.vpmod", manifest, sizeof(manifest))) {
        fprintf(stderr, "vpm: path is too long.\n");
        return 1;
    }

    if (!save_manifest_set_dependency(manifest, pkg, ver)) {
        fprintf(stderr, "vpm: failed to update manifest.\n");
        return 1;
    }

    int installed = 0;
    if (!run_install(root, manifest, &installed)) {
        fprintf(stderr, "vpm: install failed after add.\n");
        return 1;
    }

    printf("vpm: added %s (%s)\n", pkg, ver);
    printf("vpm: installed %d package(s)\n", installed);
    return 0;
}

static int cmd_remove(const char* pkg) {
    if (!is_valid_package_name(pkg)) {
        fprintf(stderr, "vpm: invalid package name '%s'\n", pkg ? pkg : "");
        return 1;
    }

    char root[MAX_PATH_LEN];
    if (!find_project_root(root, sizeof(root))) {
        fprintf(stderr, "vpm: no viper.vpmod found (run 'viper pkg init').\n");
        return 1;
    }

    char manifest[MAX_PATH_LEN];
    if (!join_path(root, "viper.vpmod", manifest, sizeof(manifest))) {
        fprintf(stderr, "vpm: path is too long.\n");
        return 1;
    }

    bool removed = false;
    if (!save_manifest_remove_dependency(manifest, pkg, &removed)) {
        fprintf(stderr, "vpm: failed to update manifest.\n");
        return 1;
    }

    int installed = 0;
    if (!run_install(root, manifest, &installed)) {
        fprintf(stderr, "vpm: failed to refresh lock/install state.\n");
        return 1;
    }

    if (removed) {
        printf("vpm: removed %s from manifest\n", pkg);
    } else {
        printf("vpm: package %s not found in manifest\n", pkg);
    }
    return 0;
}

static int cmd_install(void) {
    char root[MAX_PATH_LEN];
    if (!find_project_root(root, sizeof(root))) {
        fprintf(stderr, "vpm: no viper.vpmod found (run 'viper pkg init').\n");
        return 1;
    }

    char manifest[MAX_PATH_LEN];
    if (!join_path(root, "viper.vpmod", manifest, sizeof(manifest))) {
        fprintf(stderr, "vpm: path is too long.\n");
        return 1;
    }

    int installed = 0;
    if (!run_install(root, manifest, &installed)) {
        fprintf(stderr, "vpm: install failed.\n");
        return 1;
    }

    printf("vpm: installed %d package(s)\n", installed);
    return 0;
}

static int cmd_lock(void) {
    char root[MAX_PATH_LEN];
    if (!find_project_root(root, sizeof(root))) {
        fprintf(stderr, "vpm: no viper.vpmod found (run 'viper pkg init').\n");
        return 1;
    }

    char manifest[MAX_PATH_LEN];
    if (!join_path(root, "viper.vpmod", manifest, sizeof(manifest))) {
        fprintf(stderr, "vpm: path is too long.\n");
        return 1;
    }

    Dependency* deps = NULL;
    int dep_count = 0;
    if (!load_manifest_dependencies(manifest, &deps, &dep_count)) {
        fprintf(stderr, "vpm: failed to read dependencies from manifest.\n");
        return 1;
    }

    bool ok = write_lockfile(root, deps, dep_count);
    free(deps);

    if (!ok) {
        fprintf(stderr, "vpm: failed to write lockfile.\n");
        return 1;
    }

    printf("vpm: wrote viper.lock with %d package(s)\n", dep_count);
    return 0;
}

static int cmd_list(void) {
    char root[MAX_PATH_LEN];
    if (!find_project_root(root, sizeof(root))) {
        fprintf(stderr, "vpm: no viper.vpmod found (run 'viper pkg init').\n");
        return 1;
    }

    char manifest[MAX_PATH_LEN];
    if (!join_path(root, "viper.vpmod", manifest, sizeof(manifest))) {
        fprintf(stderr, "vpm: path is too long.\n");
        return 1;
    }

    Dependency* deps = NULL;
    int dep_count = 0;
    if (!load_manifest_dependencies(manifest, &deps, &dep_count)) {
        fprintf(stderr, "vpm: could not read manifest.\n");
        return 1;
    }

    if (dep_count == 0) {
        printf("(no packages)\n");
    } else {
        for (int i = 0; i < dep_count; i++) {
            printf("%s(%s)\n", deps[i].name, deps[i].version);
        }
    }

    free(deps);
    return 0;
}

static int cmd_build(void) {
    char root[MAX_PATH_LEN];
    if (!find_project_root(root, sizeof(root))) {
        fprintf(stderr, "vpm: no viper.vpmod found (run 'viper pkg init').\n");
        return 1;
    }

    char manifest[MAX_PATH_LEN];
    if (!join_path(root, "viper.vpmod", manifest, sizeof(manifest))) {
        fprintf(stderr, "vpm: path is too long.\n");
        return 1;
    }

    Dependency* deps = NULL;
    int dep_count = 0;
    if (!load_manifest_dependencies(manifest, &deps, &dep_count)) {
        fprintf(stderr, "vpm: could not read manifest.\n");
        return 1;
    }

    if (dep_count == 0) {
        free(deps);
        printf("vpm: no packages to build\n");
        return 0;
    }

    int built_count = 0;
    bool ok = build_package_artifacts_for_dependencies(root, deps, dep_count, &built_count);
    free(deps);

    if (!ok) {
        fprintf(stderr, "vpm: package build failed (run 'viper pkg install' first).\n");
        return 1;
    }

    printf("vpm: built %d package module artifact(s) (.vbc + .vbb + .vabi)\n", built_count);
    return 0;
}

static int cmd_abi_check(void) {
    char root[MAX_PATH_LEN];
    if (!find_project_root(root, sizeof(root))) {
        fprintf(stderr, "vpm: no viper.vpmod found (run 'viper pkg init').\n");
        return 1;
    }

    char manifest[MAX_PATH_LEN];
    if (!join_path(root, "viper.vpmod", manifest, sizeof(manifest))) {
        fprintf(stderr, "vpm: path is too long.\n");
        return 1;
    }

    Dependency* deps = NULL;
    int dep_count = 0;
    if (!load_manifest_dependencies(manifest, &deps, &dep_count)) {
        fprintf(stderr, "vpm: could not read manifest.\n");
        return 1;
    }

    if (dep_count == 0) {
        free(deps);
        printf("vpm: no packages to check\n");
        return 0;
    }

    int checked = 0;
    int failed = 0;
    bool ok = true;
    for (int i = 0; i < dep_count; i++) {
        char version_dir[MAX_PATH_LEN];
        if (!dependency_version_dir(root, deps[i].name, deps[i].version, version_dir, sizeof(version_dir))) {
            ok = false;
            break;
        }
        if (!is_directory(version_dir)) {
            fprintf(stderr, "vpm: installed package path missing: %s(%s)\n", deps[i].name, deps[i].version);
            failed++;
            continue;
        }
        if (!check_abi_tree(version_dir, &checked, &failed)) {
            ok = false;
            break;
        }
    }
    free(deps);

    if (!ok) {
        fprintf(stderr, "vpm: abi check failed due to filesystem error.\n");
        return 1;
    }
    if (failed > 0) {
        fprintf(stderr, "vpm: abi check failed (%d/%d module artifact mismatch)\n", failed, checked);
        return 1;
    }

    printf("vpm: abi check passed for %d module artifact(s)\n", checked);
    return 0;
}

static int cmd_abi_diff(const char* pkg, const char* from_ver, const char* to_ver, bool fail_on_breaking) {
    if (!is_valid_package_name(pkg)) {
        fprintf(stderr, "vpm: invalid package name '%s'\n", pkg ? pkg : "");
        return 1;
    }
    if (!is_valid_version(from_ver) || !is_valid_version(to_ver)) {
        fprintf(stderr, "vpm: invalid version range '%s' -> '%s'\n",
                from_ver ? from_ver : "", to_ver ? to_ver : "");
        return 1;
    }

    char root[MAX_PATH_LEN];
    if (!find_project_root(root, sizeof(root))) {
        fprintf(stderr, "vpm: no viper.vpmod found (run 'viper pkg init').\n");
        return 1;
    }

    char from_dir[MAX_PATH_LEN];
    char to_dir[MAX_PATH_LEN];
    if (!dependency_version_dir(root, pkg, from_ver, from_dir, sizeof(from_dir)) ||
        !dependency_version_dir(root, pkg, to_ver, to_dir, sizeof(to_dir))) {
        fprintf(stderr, "vpm: path is too long.\n");
        return 1;
    }
    if (!is_directory(from_dir)) {
        fprintf(stderr, "vpm: package version not found: %s(%s)\n", pkg, from_ver);
        return 1;
    }
    if (!is_directory(to_dir)) {
        fprintf(stderr, "vpm: package version not found: %s(%s)\n", pkg, to_ver);
        return 1;
    }

    AbiEntry* lhs = NULL;
    AbiEntry* rhs = NULL;
    int lhs_count = 0;
    int rhs_count = 0;
    if (!collect_package_abi_entries(from_dir, &lhs, &lhs_count) ||
        !collect_package_abi_entries(to_dir, &rhs, &rhs_count)) {
        free(lhs);
        free(rhs);
        fprintf(stderr, "vpm: failed to read ABI entries for diff.\n");
        return 1;
    }

    int added = 0;
    int removed = 0;
    int changed = 0;
    int breaking = 0;
    int non_breaking = 0;
    printf("vpm: abi diff %s %s -> %s\n", pkg, from_ver, to_ver);

    for (int i = 0; i < lhs_count; i++) {
        int j = find_abi_entry(rhs, rhs_count, lhs[i].module, lhs[i].kind, lhs[i].name);
        if (j < 0) {
            printf("BREAKING REMOVED %s %s %s %d\n",
                   lhs[i].module, abi_kind_tag(lhs[i].kind), lhs[i].name, lhs[i].metric);
            removed++;
            breaking++;
            continue;
        }
        if (!abi_entry_contract_equal(&lhs[i], &rhs[j])) {
            const char* reason = NULL;
            bool is_breaking = abi_change_is_breaking(lhs[i].kind, lhs[i].metric, rhs[j].metric, &reason);
            char lhs_contract[512];
            char rhs_contract[512];
            format_abi_contract(lhs[i].return_type, lhs[i].effects, lhs_contract, sizeof(lhs_contract));
            format_abi_contract(rhs[j].return_type, rhs[j].effects, rhs_contract, sizeof(rhs_contract));
            printf("%s CHANGED %s %s %s %d -> %d [%s -> %s]",
                   is_breaking ? "BREAKING" : "NON_BREAKING",
                   lhs[i].module, abi_kind_tag(lhs[i].kind),
                   lhs[i].name, lhs[i].metric, rhs[j].metric,
                   lhs_contract, rhs_contract);
            if (reason && reason[0] != '\0') printf(" (%s)", reason);
            printf("\n");
            changed++;
            if (is_breaking) breaking++;
            else non_breaking++;
        }
    }

    for (int i = 0; i < rhs_count; i++) {
        int j = find_abi_entry(lhs, lhs_count, rhs[i].module, rhs[i].kind, rhs[i].name);
        if (j < 0) {
            char rhs_contract[512];
            format_abi_contract(rhs[i].return_type, rhs[i].effects, rhs_contract, sizeof(rhs_contract));
            printf("NON_BREAKING ADDED %s %s %s %d [%s]\n",
                   rhs[i].module, abi_kind_tag(rhs[i].kind), rhs[i].name, rhs[i].metric,
                   rhs_contract);
            added++;
            non_breaking++;
        }
    }

    if (added == 0 && removed == 0 && changed == 0) {
        printf("vpm: no ABI changes.\n");
    }
    printf("vpm: abi diff summary added=%d removed=%d changed=%d breaking=%d non_breaking=%d\n",
           added, removed, changed, breaking, non_breaking);

    if (fail_on_breaking && breaking > 0) {
        fprintf(stderr, "vpm: breaking changes detected.\n");
        free(lhs);
        free(rhs);
        return 2;
    }

    free(lhs);
    free(rhs);
    return 0;
}

int vpm_handle_cli(int argc, const char* argv[]) {
    if (argc < 1) {
        print_pkg_usage();
        return 64;
    }

    const char* cmd = argv[0];
    if (strcmp(cmd, "init") == 0) {
        const char* name = (argc >= 2) ? argv[1] : NULL;
        return cmd_init(name);
    }
    if (strcmp(cmd, "add") == 0) {
        if (argc < 2) {
            print_pkg_usage();
            return 64;
        }
        const char* ver = (argc >= 3) ? argv[2] : NULL;
        return cmd_add(argv[1], ver);
    }
    if (strcmp(cmd, "remove") == 0) {
        if (argc < 2) {
            print_pkg_usage();
            return 64;
        }
        return cmd_remove(argv[1]);
    }
    if (strcmp(cmd, "install") == 0) {
        return cmd_install();
    }
    if (strcmp(cmd, "lock") == 0) {
        return cmd_lock();
    }
    if (strcmp(cmd, "list") == 0) {
        return cmd_list();
    }
    if (strcmp(cmd, "build") == 0) {
        return cmd_build();
    }
    if (strcmp(cmd, "abi") == 0) {
        if (argc < 2) {
            print_pkg_usage();
            return 64;
        }
        if (strcmp(argv[1], "check") == 0) {
            if (argc != 2) {
                print_pkg_usage();
                return 64;
            }
            return cmd_abi_check();
        }
        if (strcmp(argv[1], "diff") == 0) {
            if (argc != 5 && argc != 6) {
                print_pkg_usage();
                return 64;
            }
            bool fail_on_breaking = false;
            if (argc == 6) {
                if (strcmp(argv[5], "--fail-on-breaking") != 0) {
                    print_pkg_usage();
                    return 64;
                }
                fail_on_breaking = true;
            }
            return cmd_abi_diff(argv[2], argv[3], argv[4], fail_on_breaking);
        }
        print_pkg_usage();
        return 64;
    }

    print_pkg_usage();
    return 64;
}
