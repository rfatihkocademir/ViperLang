#include "indexer.h"

#include "crypto.h"
#include "parser.h"
#include "bytecode.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_PATH_LEN 1024
#define MAX_NAME_LEN 128

typedef struct {
    char** items;
    int count;
    int cap;
} StringList;

typedef struct {
    char name[MAX_NAME_LEN];
    char type_name[MAX_NAME_LEN];
    int line;
} IndexVar;

typedef struct {
    char target[MAX_NAME_LEN * 2];
    char qualifier[MAX_NAME_LEN];
    char name[MAX_NAME_LEN];
    int arg_count;
    int line;
} IndexCall;

typedef struct {
    char name[MAX_NAME_LEN];
    int arity;
    bool is_public;
    char return_type[MAX_NAME_LEN];
    int line;
    StringList params;
    StringList declared_effects;
    StringList inferred_effects;
    IndexCall* calls;
    int call_count;
    int call_cap;
} IndexFn;

typedef struct {
    char raw_path[MAX_PATH_LEN];
    char resolved_path[MAX_PATH_LEN];
    char alias[MAX_NAME_LEN];
    int line;
} IndexImport;

typedef struct {
    char name[MAX_NAME_LEN];
    bool is_public;
    int line;
    int field_count;
    int field_cap;
    char** fields;
} IndexStruct;

typedef struct {
    char path[MAX_PATH_LEN];

    IndexImport* imports;
    int import_count;
    int import_cap;

    IndexVar* variables;
    int variable_count;
    int variable_cap;

    IndexFn* functions;
    int function_count;
    int function_cap;

    IndexStruct* structs;
    int struct_count;
    int struct_cap;

    IndexCall* calls;
    int call_count;
    int call_cap;

    StringList inferred_effects;
} ModuleIndex;

typedef struct {
    ModuleIndex* modules;
    int module_count;
    int module_cap;
} ProjectIndex;

typedef struct {
    unsigned char include_module;
    unsigned char* imports;
    unsigned char* variables;
    unsigned char* functions;
    unsigned char* structs;
} FocusModule;

typedef struct {
    FocusModule* modules;
    int module_count;
    int seed_count;
} FocusSlice;

typedef struct {
    char* key;
    char* text;
} SemanticItem;

typedef struct {
    SemanticItem* items;
    int count;
    int cap;
} SemanticItemList;

typedef struct {
    bool parsed;
    bool is_public;
    char item_kind;
    char name[MAX_NAME_LEN];
    int arity;
    char signature[2048];
    char return_type[MAX_NAME_LEN];
    char raw_contract[4608];
    StringList declared_effects;
    StringList inferred_effects;
    StringList calls;
} CompactSymbolRow;

typedef struct {
    char root[MAX_PATH_LEN];
    char entry[MAX_PATH_LEN];
    char focus[MAX_NAME_LEN];
    bool include_impact;
    int semantic_item_count;
    char semantic_fingerprint[65];
    StringList tracked_paths;
    StringList tracked_hashes;
} ProjectStateManifest;

static char index_project_root[MAX_PATH_LEN];
static void path_dirname(const char* path, char* out, size_t out_size);
static bool ensure_capacity(void** ptr, int* cap, int count_needed, size_t elem_size);
static bool focus_module_has_content(const ModuleIndex* module, const FocusModule* focus_module);
static bool compute_module_hash_hex(const char* module_path, char out_hex[65]);
static bool collect_focus_semantic_items(const ProjectIndex* project, const FocusSlice* slice,
                                         SemanticItemList* out);
static bool collect_impact_semantic_items(const ProjectIndex* project, const FocusSlice* slice,
                                          SemanticItemList* out);
static bool emit_project_state_from_project(const ProjectIndex* project, const char* entry_path,
                                            const char* out_path, const char* focus_symbol,
                                            bool include_impact);
static int string_list_index_of(const StringList* list, const char* text);

static void copy_text(char* dst, size_t dst_size, const char* src) {
    if (dst_size == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n >= dst_size) n = dst_size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void token_to_text(Token token, char* out, size_t out_size) {
    if (out_size == 0) return;
    if (!token.start || token.length <= 0) {
        out[0] = '\0';
        return;
    }
    size_t n = (size_t)token.length;
    if (n >= out_size) n = out_size - 1;
    memcpy(out, token.start, n);
    out[n] = '\0';
}

static char* dup_text(const char* src) {
    if (!src) return NULL;
    size_t n = strlen(src);
    char* out = (char*)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, src, n + 1);
    return out;
}

static bool string_list_add_unique(StringList* list, const char* text) {
    if (!list || !text || text[0] == '\0') return true;
    for (int i = 0; i < list->count; i++) {
        if (strcmp(list->items[i], text) == 0) return true;
    }
    if (!ensure_capacity((void**)&list->items, &list->cap, list->count + 1, sizeof(char*))) {
        return false;
    }
    list->items[list->count] = dup_text(text);
    if (!list->items[list->count]) return false;
    list->count++;
    return true;
}

static bool string_list_append(StringList* list, const char* text) {
    if (!list || !text) return false;
    if (!ensure_capacity((void**)&list->items, &list->cap, list->count + 1, sizeof(char*))) {
        return false;
    }
    list->items[list->count] = dup_text(text);
    if (!list->items[list->count]) return false;
    list->count++;
    return true;
}

static bool string_list_add_token(StringList* list, Token token) {
    char name[MAX_NAME_LEN];
    token_to_text(token, name, sizeof(name));
    return string_list_add_unique(list, name);
}

static void free_string_list(StringList* list) {
    if (!list) return;
    for (int i = 0; i < list->count; i++) free(list->items[i]);
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->cap = 0;
}

static void free_semantic_items(SemanticItemList* list) {
    if (!list) return;
    for (int i = 0; i < list->count; i++) {
        free(list->items[i].key);
        free(list->items[i].text);
    }
    free(list->items);
    memset(list, 0, sizeof(*list));
}

static void free_project_state_manifest(ProjectStateManifest* manifest) {
    if (!manifest) return;
    free_string_list(&manifest->tracked_paths);
    free_string_list(&manifest->tracked_hashes);
    memset(manifest, 0, sizeof(*manifest));
}

static void free_compact_symbol_row(CompactSymbolRow* row) {
    if (!row) return;
    free_string_list(&row->declared_effects);
    free_string_list(&row->inferred_effects);
    free_string_list(&row->calls);
    memset(row, 0, sizeof(*row));
}

static bool semantic_items_add(SemanticItemList* list, const char* key, const char* text) {
    if (!list || !key || !text) return false;
    for (int i = 0; i < list->count; i++) {
        if (strcmp(list->items[i].key, key) == 0) return true;
    }
    if (!ensure_capacity((void**)&list->items, &list->cap, list->count + 1, sizeof(SemanticItem))) {
        return false;
    }
    SemanticItem* item = &list->items[list->count++];
    item->key = dup_text(key);
    item->text = dup_text(text);
    if (!item->key || !item->text) {
        free(item->key);
        free(item->text);
        list->count--;
        return false;
    }
    return true;
}

static bool semantic_items_contains(const SemanticItemList* list, const char* key) {
    if (!list || !key) return false;
    for (int i = 0; i < list->count; i++) {
        if (strcmp(list->items[i].key, key) == 0) return true;
    }
    return false;
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

static bool module_source_exists(const char* module_path) {
    if (file_exists(module_path)) return true;
    char vbc_path[MAX_PATH_LEN];
    if (vp_to_vbc_path(module_path, vbc_path, sizeof(vbc_path)) && file_exists(vbc_path)) {
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

static bool is_package_import(const char* raw_path) {
    return raw_path && raw_path[0] == '@';
}

static bool join_path(const char* base, const char* child, char* out, size_t out_size) {
    if (!base || !child || out_size == 0) return false;
    size_t base_len = strlen(base);
    size_t child_len = strlen(child);
    if (base_len + 1 + child_len + 1 > out_size) return false;
    memcpy(out, base, base_len);
    out[base_len] = '/';
    memcpy(out + base_len + 1, child, child_len);
    out[base_len + 1 + child_len] = '\0';
    return true;
}

static bool find_project_root_from_entry(const char* entry_path, char* out, size_t out_size) {
    char current[MAX_PATH_LEN];
    path_dirname(entry_path, current, sizeof(current));

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

    copy_text(out, out_size, ".");
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

static void decode_use_path(Token token, char* out, size_t out_size) {
    if (out_size == 0) return;

    const char* start = token.start;
    int length = token.length;

    if (start && length >= 2 && start[0] == '"' && start[length - 1] == '"') {
        start += 1;
        length -= 2;
    }

    if (!start || length <= 0) {
        out[0] = '\0';
        return;
    }

    size_t n = (size_t)length;
    if (n >= out_size) n = out_size - 1;
    memcpy(out, start, n);
    out[n] = '\0';
}

static bool resolve_entry_path(const char* path, char* out, size_t out_size) {
    if (!path || path[0] == '\0') return false;

    if (is_absolute_path(path)) {
        copy_text(out, out_size, path);
        return true;
    }

    char cwd[MAX_PATH_LEN];
    if (getcwd(cwd, sizeof(cwd)) == NULL) return false;
    return join_path(cwd, path, out, out_size);
}

static bool resolve_module_path(const char* current_module_path, const char* raw_import_path,
                                char* out, size_t out_size) {
    if (!raw_import_path || raw_import_path[0] == '\0') return false;

    if (is_package_import(raw_import_path)) {
        if (strstr(raw_import_path, "..") != NULL) return false;

        const char* slash = strchr(raw_import_path, '/');
        char package_name[MAX_PATH_LEN];
        const char* subpath = "index.vp";

        if (slash == NULL) {
            copy_text(package_name, sizeof(package_name), raw_import_path);
        } else {
            size_t pkg_len = (size_t)(slash - raw_import_path);
            if (pkg_len == 0 || pkg_len >= sizeof(package_name)) return false;
            memcpy(package_name, raw_import_path, pkg_len);
            package_name[pkg_len] = '\0';
            if (slash[1] != '\0') subpath = slash + 1;
        }

        char p1[MAX_PATH_LEN];
        char package_root[MAX_PATH_LEN];
        if (!join_path(index_project_root, ".viper/packages", p1, sizeof(p1)) ||
            !join_path(p1, package_name, package_root, sizeof(package_root))) {
            return false;
        }

        char current_file[MAX_PATH_LEN];
        char version[MAX_PATH_LEN];
        if (join_path(package_root, "current", current_file, sizeof(current_file)) &&
            read_first_line(current_file, version, sizeof(version))) {
            char versions_dir[MAX_PATH_LEN];
            char version_dir[MAX_PATH_LEN];
            char candidate[MAX_PATH_LEN];
            if (join_path(package_root, "versions", versions_dir, sizeof(versions_dir)) &&
                join_path(versions_dir, version, version_dir, sizeof(version_dir)) &&
                join_path(version_dir, subpath, candidate, sizeof(candidate)) &&
                module_source_exists(candidate)) {
                copy_text(out, out_size, candidate);
                return true;
            }
        }

        if (!join_path(package_root, subpath, out, out_size)) return false;
        return module_source_exists(out);
    }

    if (is_absolute_path(raw_import_path)) {
        copy_text(out, out_size, raw_import_path);
        return true;
    }

    char base_dir[MAX_PATH_LEN];
    path_dirname(current_module_path, base_dir, sizeof(base_dir));
    return join_path(base_dir, raw_import_path, out, out_size);
}

static char* read_file(const char* path) {
    char vbc_path[MAX_PATH_LEN];
    if (vp_to_vbc_path(path, vbc_path, sizeof(vbc_path)) && file_exists(vbc_path)) {
        char* vbc_source = read_source_cache_file(vbc_path);
        if (vbc_source) return vbc_source;
    }

    FILE* file = fopen(path, "rb");
    if (!file) {
        if (vp_to_vbc_path(path, vbc_path, sizeof(vbc_path)) && file_exists(vbc_path)) {
            char* vbc_source = read_source_cache_file(vbc_path);
            if (vbc_source) return vbc_source;
        }
        fprintf(stderr, "Indexer Error: Could not open file \"%s\".\n", path);
        return NULL;
    }

    if (fseek(file, 0L, SEEK_END) != 0) {
        fclose(file);
        fprintf(stderr, "Indexer Error: Could not seek file \"%s\".\n", path);
        return NULL;
    }

    long size = ftell(file);
    if (size < 0) {
        fclose(file);
        fprintf(stderr, "Indexer Error: Could not get file size \"%s\".\n", path);
        return NULL;
    }

    rewind(file);

    char* buffer = (char*)malloc((size_t)size + 1);
    if (!buffer) {
        fclose(file);
        fprintf(stderr, "Indexer Error: Not enough memory for \"%s\".\n", path);
        return NULL;
    }

    size_t bytes_read = fread(buffer, sizeof(char), (size_t)size, file);
    fclose(file);

    if (bytes_read < (size_t)size) {
        free(buffer);
        fprintf(stderr, "Indexer Error: Could not read file \"%s\".\n", path);
        return NULL;
    }

    buffer[bytes_read] = '\0';
    return buffer;
}

static bool ensure_capacity(void** ptr, int* cap, int count_needed, size_t elem_size) {
    if (*cap >= count_needed) return true;

    int next_cap = (*cap == 0) ? 8 : *cap;
    while (next_cap < count_needed) next_cap *= 2;

    void* grown = realloc(*ptr, (size_t)next_cap * elem_size);
    if (!grown) return false;

    *ptr = grown;
    *cap = next_cap;
    return true;
}

static bool module_add_var(ModuleIndex* module, Token name, Token type_annot) {
    if (!ensure_capacity((void**)&module->variables, &module->variable_cap,
                         module->variable_count + 1, sizeof(IndexVar))) {
        return false;
    }

    IndexVar* out = &module->variables[module->variable_count++];
    memset(out, 0, sizeof(*out));
    token_to_text(name, out->name, sizeof(out->name));
    if (type_annot.length > 0) {
        token_to_text(type_annot, out->type_name, sizeof(out->type_name));
    }
    out->line = name.line;
    return true;
}

static bool add_call_record(IndexCall** calls, int* call_count, int* call_cap, AstNode* call_expr) {
    if (!calls || !call_count || !call_cap || !call_expr || call_expr->type != AST_CALL_EXPR) return false;
    if (!ensure_capacity((void**)calls, call_cap, *call_count + 1, sizeof(IndexCall))) {
        return false;
    }

    IndexCall* out = &(*calls)[(*call_count)++];
    memset(out, 0, sizeof(*out));
    token_to_text(call_expr->data.call_expr.name, out->name, sizeof(out->name));
    out->arg_count = call_expr->data.call_expr.arg_count;
    out->line = call_expr->data.call_expr.name.line;

    AstNode* callee = call_expr->data.call_expr.callee;
    if (callee && callee->type == AST_GET_EXPR &&
        callee->data.get_expr.obj &&
        callee->data.get_expr.obj->type == AST_IDENTIFIER) {
        token_to_text(callee->data.get_expr.obj->data.identifier.name,
                      out->qualifier, sizeof(out->qualifier));
    }

    if (out->qualifier[0] != '\0') {
        snprintf(out->target, sizeof(out->target), "%s.%s", out->qualifier, out->name);
    } else {
        snprintf(out->target, sizeof(out->target), "%s", out->name);
    }
    return true;
}

static bool add_inferred_effect_for_call(StringList* effects, const IndexCall* call) {
    if (!effects || !call) return false;

    if (call->qualifier[0] != '\0') {
        if (strcmp(call->qualifier, "os") == 0) return string_list_add_unique(effects, "os");
        if (strcmp(call->qualifier, "io") == 0) return string_list_add_unique(effects, "fs");
        if (strcmp(call->qualifier, "web") == 0) return string_list_add_unique(effects, "web");
        if (strcmp(call->qualifier, "db") == 0) return string_list_add_unique(effects, "db");
        if (strcmp(call->qualifier, "cache") == 0) return string_list_add_unique(effects, "cache");
        if (strcmp(call->qualifier, "ai") == 0) return string_list_add_unique(effects, "ai");
        if (strcmp(call->qualifier, "meta") == 0) return string_list_add_unique(effects, "meta");
    }

    if (strncmp(call->name, "os_", 3) == 0) return string_list_add_unique(effects, "os");
    if (strncmp(call->name, "fs_", 3) == 0) return string_list_add_unique(effects, "fs");
    if (strncmp(call->name, "web_", 4) == 0 ||
        strcmp(call->name, "serve") == 0 ||
        strcmp(call->name, "fetch") == 0) {
        return string_list_add_unique(effects, "web");
    }
    if (strncmp(call->name, "vdb_", 4) == 0 || strcmp(call->name, "query") == 0) {
        return string_list_add_unique(effects, "db");
    }
    if (strncmp(call->name, "ai_", 3) == 0) return string_list_add_unique(effects, "ai");
    if (strncmp(call->name, "cache_", 6) == 0) return string_list_add_unique(effects, "cache");
    if (strncmp(call->name, "meta_", 5) == 0) return string_list_add_unique(effects, "meta");
    if (strcmp(call->name, "load_dl") == 0 || strcmp(call->name, "get_fn") == 0) {
        return string_list_add_unique(effects, "ffi");
    }
    if (strcmp(call->name, "panic") == 0 || strcmp(call->name, "recover") == 0) {
        return string_list_add_unique(effects, "panic");
    }
    return true;
}

static bool collect_expr_semantics(AstNode* expr, IndexCall** calls, int* call_count, int* call_cap,
                                   StringList* effects);
static bool collect_stmt_semantics(AstNode* stmt, IndexCall** calls, int* call_count, int* call_cap,
                                   StringList* effects, bool include_nested_functions);

static bool collect_expr_semantics(AstNode* expr, IndexCall** calls, int* call_count, int* call_cap,
                                   StringList* effects) {
    if (!expr) return true;

    switch (expr->type) {
        case AST_CALL_EXPR: {
            if (!add_call_record(calls, call_count, call_cap, expr)) return false;
            if (!add_inferred_effect_for_call(effects, &(*calls)[*call_count - 1])) return false;
            if (!collect_expr_semantics(expr->data.call_expr.callee, calls, call_count, call_cap, effects)) return false;
            for (int i = 0; i < expr->data.call_expr.arg_count; i++) {
                if (!collect_expr_semantics(expr->data.call_expr.args[i], calls, call_count, call_cap, effects)) {
                    return false;
                }
            }
            return true;
        }
        case AST_BINARY_EXPR:
            return collect_expr_semantics(expr->data.binary.left, calls, call_count, call_cap, effects) &&
                   collect_expr_semantics(expr->data.binary.right, calls, call_count, call_cap, effects);
        case AST_ASSIGN_EXPR:
            return collect_expr_semantics(expr->data.assign.value, calls, call_count, call_cap, effects);
        case AST_GET_EXPR:
        case AST_SAFE_GET_EXPR:
            return collect_expr_semantics(expr->data.get_expr.obj, calls, call_count, call_cap, effects);
        case AST_SET_EXPR:
            return collect_expr_semantics(expr->data.set_expr.obj, calls, call_count, call_cap, effects) &&
                   collect_expr_semantics(expr->data.set_expr.value, calls, call_count, call_cap, effects);
        case AST_INDEX_EXPR:
            return collect_expr_semantics(expr->data.index_expr.target, calls, call_count, call_cap, effects) &&
                   collect_expr_semantics(expr->data.index_expr.index, calls, call_count, call_cap, effects) &&
                   collect_expr_semantics(expr->data.index_expr.value, calls, call_count, call_cap, effects);
        case AST_ARRAY_EXPR:
            for (int i = 0; i < expr->data.array_expr.count; i++) {
                if (!collect_expr_semantics(expr->data.array_expr.elements[i], calls, call_count, call_cap, effects)) {
                    return false;
                }
            }
            return true;
        case AST_MATCH_EXPR:
            return collect_expr_semantics(expr->data.match_expr.left, calls, call_count, call_cap, effects) &&
                   collect_expr_semantics(expr->data.match_expr.right, calls, call_count, call_cap, effects);
        case AST_SPAWN_EXPR:
            return string_list_add_unique(effects, "async") &&
                   collect_expr_semantics(expr->data.spawn_expr.expr, calls, call_count, call_cap, effects);
        case AST_AWAIT_EXPR:
            return string_list_add_unique(effects, "async") &&
                   collect_expr_semantics(expr->data.await_expr.expr, calls, call_count, call_cap, effects);
        case AST_TRY_EXPR:
            return collect_expr_semantics(expr->data.try_expr.try_block, calls, call_count, call_cap, effects) &&
                   collect_expr_semantics(expr->data.try_expr.catch_block, calls, call_count, call_cap, effects);
        case AST_TYPEOF_EXPR:
            return collect_expr_semantics(expr->data.typeof_expr.expr, calls, call_count, call_cap, effects);
        case AST_CLONE_EXPR:
        case AST_EVAL_EXPR:
        case AST_KEYS_EXPR:
            if (expr->type == AST_EVAL_EXPR && !string_list_add_unique(effects, "dynamic")) return false;
            return collect_expr_semantics(expr->data.clone_expr.expr, calls, call_count, call_cap, effects);
        case AST_HAS_EXPR:
            return collect_expr_semantics(expr->data.has_expr.obj, calls, call_count, call_cap, effects) &&
                   collect_expr_semantics(expr->data.has_expr.prop, calls, call_count, call_cap, effects);
        case AST_ERROR_PROPAGATE_EXPR:
            return collect_expr_semantics(expr->data.error_propagate.expr, calls, call_count, call_cap, effects);
        default:
            return true;
    }
}

static bool collect_stmt_semantics(AstNode* stmt, IndexCall** calls, int* call_count, int* call_cap,
                                   StringList* effects, bool include_nested_functions) {
    if (!stmt) return true;

    switch (stmt->type) {
        case AST_PROGRAM:
        case AST_BLOCK_STMT:
            for (int i = 0; i < stmt->data.block.count; i++) {
                if (!collect_stmt_semantics(stmt->data.block.statements[i], calls, call_count,
                                            call_cap, effects, include_nested_functions)) {
                    return false;
                }
            }
            return true;
        case AST_VAR_DECL:
            return collect_expr_semantics(stmt->data.var_decl.initializer, calls, call_count, call_cap, effects);
        case AST_EXPR_STMT:
            return collect_expr_semantics(stmt->data.expr_stmt.expr, calls, call_count, call_cap, effects);
        case AST_IF_STMT:
            return collect_expr_semantics(stmt->data.if_stmt.condition, calls, call_count, call_cap, effects) &&
                   collect_stmt_semantics(stmt->data.if_stmt.then_branch, calls, call_count, call_cap, effects, include_nested_functions) &&
                   collect_stmt_semantics(stmt->data.if_stmt.else_branch, calls, call_count, call_cap, effects, include_nested_functions);
        case AST_WHILE_STMT:
            return collect_expr_semantics(stmt->data.while_stmt.condition, calls, call_count, call_cap, effects) &&
                   collect_stmt_semantics(stmt->data.while_stmt.body, calls, call_count, call_cap, effects, include_nested_functions);
        case AST_RETURN_STMT:
            return collect_expr_semantics(stmt->data.return_stmt.value, calls, call_count, call_cap, effects);
        case AST_SYNC_STMT:
            return string_list_add_unique(effects, "async") &&
                   collect_stmt_semantics(stmt->data.sync_stmt.body, calls, call_count, call_cap, effects, include_nested_functions);
        case AST_FUNC_DECL:
            if (!include_nested_functions) return true;
            return collect_stmt_semantics(stmt->data.func_decl.body, calls, call_count, call_cap, effects, true);
        default:
            return true;
    }
}

static IndexFn* module_add_fn(ModuleIndex* module, AstNode* fn_decl) {
    if (!ensure_capacity((void**)&module->functions, &module->function_cap,
                         module->function_count + 1, sizeof(IndexFn))) {
        return NULL;
    }

    IndexFn* out = &module->functions[module->function_count++];
    memset(out, 0, sizeof(*out));
    token_to_text(fn_decl->data.func_decl.name, out->name, sizeof(out->name));
    out->arity = fn_decl->data.func_decl.param_count;
    out->is_public = fn_decl->data.func_decl.is_public;
    out->line = fn_decl->data.func_decl.name.line;
    if (fn_decl->data.func_decl.return_type.length > 0) {
        token_to_text(fn_decl->data.func_decl.return_type, out->return_type, sizeof(out->return_type));
    }
    for (int i = 0; i < fn_decl->data.func_decl.param_count; i++) {
        if (!string_list_add_token(&out->params, fn_decl->data.func_decl.params[i])) return NULL;
    }
    for (int i = 0; i < fn_decl->data.func_decl.effect_count; i++) {
        if (!string_list_add_token(&out->declared_effects, fn_decl->data.func_decl.effects[i])) return NULL;
    }
    if (!collect_stmt_semantics(fn_decl->data.func_decl.body, &out->calls, &out->call_count, &out->call_cap,
                                &out->inferred_effects, true)) {
        return NULL;
    }
    return out;
}

static bool module_add_struct(ModuleIndex* module, AstNode* st_decl) {
    if (!ensure_capacity((void**)&module->structs, &module->struct_cap,
                         module->struct_count + 1, sizeof(IndexStruct))) {
        return false;
    }

    IndexStruct* out = &module->structs[module->struct_count++];
    memset(out, 0, sizeof(*out));
    token_to_text(st_decl->data.struct_decl.name, out->name, sizeof(out->name));
    out->is_public = st_decl->data.struct_decl.is_public;
    out->line = st_decl->data.struct_decl.name.line;

    if (st_decl->data.struct_decl.field_count <= 0) return true;
    if (!ensure_capacity((void**)&out->fields, &out->field_cap,
                         st_decl->data.struct_decl.field_count, sizeof(char*))) {
        return false;
    }

    out->field_count = st_decl->data.struct_decl.field_count;
    for (int i = 0; i < st_decl->data.struct_decl.field_count; i++) {
        char field_name[MAX_NAME_LEN];
        token_to_text(st_decl->data.struct_decl.fields[i], field_name, sizeof(field_name));
        out->fields[i] = dup_text(field_name);
        if (!out->fields[i]) return false;
    }

    return true;
}

static bool module_add_import(ModuleIndex* module, const char* raw_path,
                              const char* resolved_path, Token alias, int line) {
    if (!ensure_capacity((void**)&module->imports, &module->import_cap,
                         module->import_count + 1, sizeof(IndexImport))) {
        return false;
    }

    IndexImport* out = &module->imports[module->import_count++];
    memset(out, 0, sizeof(*out));
    copy_text(out->raw_path, sizeof(out->raw_path), raw_path);
    copy_text(out->resolved_path, sizeof(out->resolved_path), resolved_path);
    if (alias.length > 0) token_to_text(alias, out->alias, sizeof(out->alias));
    out->line = line;
    return true;
}

static void free_project(ProjectIndex* project) {
    if (!project || !project->modules) return;

    for (int i = 0; i < project->module_count; i++) {
        ModuleIndex* m = &project->modules[i];

        for (int j = 0; j < m->function_count; j++) {
            free_string_list(&m->functions[j].params);
            free_string_list(&m->functions[j].declared_effects);
            free_string_list(&m->functions[j].inferred_effects);
            free(m->functions[j].calls);
        }

        for (int j = 0; j < m->struct_count; j++) {
            for (int k = 0; k < m->structs[j].field_count; k++) {
                free(m->structs[j].fields[k]);
            }
            free(m->structs[j].fields);
        }

        free_string_list(&m->inferred_effects);
        free(m->imports);
        free(m->variables);
        free(m->functions);
        free(m->structs);
        free(m->calls);
    }

    free(project->modules);
    memset(project, 0, sizeof(*project));
}

static void free_focus_slice(FocusSlice* slice) {
    if (!slice || !slice->modules) return;

    for (int i = 0; i < slice->module_count; i++) {
        free(slice->modules[i].imports);
        free(slice->modules[i].variables);
        free(slice->modules[i].functions);
        free(slice->modules[i].structs);
    }

    free(slice->modules);
    memset(slice, 0, sizeof(*slice));
}

static bool init_focus_slice(FocusSlice* slice, const ProjectIndex* project) {
    if (!slice || !project) return false;
    memset(slice, 0, sizeof(*slice));

    slice->module_count = project->module_count;
    if (project->module_count == 0) return true;

    slice->modules = (FocusModule*)calloc((size_t)project->module_count, sizeof(FocusModule));
    if (!slice->modules) return false;

    for (int i = 0; i < project->module_count; i++) {
        const ModuleIndex* module = &project->modules[i];
        FocusModule* focus_module = &slice->modules[i];

        if (module->import_count > 0) {
            focus_module->imports = (unsigned char*)calloc((size_t)module->import_count, sizeof(unsigned char));
            if (!focus_module->imports) {
                free_focus_slice(slice);
                return false;
            }
        }
        if (module->variable_count > 0) {
            focus_module->variables = (unsigned char*)calloc((size_t)module->variable_count, sizeof(unsigned char));
            if (!focus_module->variables) {
                free_focus_slice(slice);
                return false;
            }
        }
        if (module->function_count > 0) {
            focus_module->functions = (unsigned char*)calloc((size_t)module->function_count, sizeof(unsigned char));
            if (!focus_module->functions) {
                free_focus_slice(slice);
                return false;
            }
        }
        if (module->struct_count > 0) {
            focus_module->structs = (unsigned char*)calloc((size_t)module->struct_count, sizeof(unsigned char));
            if (!focus_module->structs) {
                free_focus_slice(slice);
                return false;
            }
        }
    }

    return true;
}

static bool focus_mark_var(FocusSlice* slice, int module_index, int var_index) {
    if (!slice || module_index < 0 || module_index >= slice->module_count) return false;
    FocusModule* module = &slice->modules[module_index];
    if (!module->variables || module->variables[var_index]) return false;
    module->variables[var_index] = 1;
    module->include_module = 1;
    return true;
}

static bool focus_mark_struct(FocusSlice* slice, int module_index, int struct_index) {
    if (!slice || module_index < 0 || module_index >= slice->module_count) return false;
    FocusModule* module = &slice->modules[module_index];
    if (!module->structs || module->structs[struct_index]) return false;
    module->structs[struct_index] = 1;
    module->include_module = 1;
    return true;
}

static bool focus_mark_fn(FocusSlice* slice, int module_index, int fn_index) {
    if (!slice || module_index < 0 || module_index >= slice->module_count) return false;
    FocusModule* module = &slice->modules[module_index];
    if (!module->functions || module->functions[fn_index]) return false;
    module->functions[fn_index] = 1;
    module->include_module = 1;
    return true;
}

static bool focus_mark_import(FocusSlice* slice, int module_index, int import_index) {
    if (!slice || module_index < 0 || module_index >= slice->module_count) return false;
    FocusModule* module = &slice->modules[module_index];
    if (!module->imports || module->imports[import_index]) return false;
    module->imports[import_index] = 1;
    module->include_module = 1;
    return true;
}

static int find_module_index(const ProjectIndex* project, const char* path) {
    for (int i = 0; i < project->module_count; i++) {
        if (strcmp(project->modules[i].path, path) == 0) return i;
    }
    return -1;
}

static void basename_without_ext(const char* path, char* out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!path || path[0] == '\0') return;

    const char* slash = strrchr(path, '/');
    const char* base = slash ? slash + 1 : path;
    const char* dot = strrchr(base, '.');
    size_t n = dot ? (size_t)(dot - base) : strlen(base);
    if (n >= out_size) n = out_size - 1;
    memcpy(out, base, n);
    out[n] = '\0';
}

static void import_visible_name(const IndexImport* import_, char* out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!import_) return;

    if (import_->alias[0] != '\0') {
        copy_text(out, out_size, import_->alias);
        return;
    }

    if (import_->resolved_path[0] != '\0') {
        basename_without_ext(import_->resolved_path, out, out_size);
        if (out[0] != '\0' && strcmp(out, "index") != 0) return;
    }

    const char* raw = import_->raw_path;
    if (!raw || raw[0] == '\0') return;
    const char* slash = strrchr(raw, '/');
    const char* base = slash ? slash + 1 : raw;
    if (base[0] == '@') base++;
    basename_without_ext(base, out, out_size);
}

static int find_function_in_module(const ModuleIndex* module, const char* name) {
    if (!module || !name || name[0] == '\0') return -1;
    for (int i = 0; i < module->function_count; i++) {
        if (strcmp(module->functions[i].name, name) == 0) return i;
    }
    return -1;
}

static int find_import_in_module_by_qualifier(const ModuleIndex* module, const char* qualifier) {
    if (!module || !qualifier || qualifier[0] == '\0') return -1;
    for (int i = 0; i < module->import_count; i++) {
        char visible[MAX_NAME_LEN];
        import_visible_name(&module->imports[i], visible, sizeof(visible));
        if (visible[0] != '\0' && strcmp(visible, qualifier) == 0) return i;
    }
    return -1;
}

static int count_selected(const unsigned char* items, int count) {
    if (!items || count <= 0) return 0;
    int total = 0;
    for (int i = 0; i < count; i++) total += items[i] ? 1 : 0;
    return total;
}

static int count_selected_modules(const ProjectIndex* project, const FocusSlice* slice) {
    if (!project || !slice) return 0;
    int total = 0;
    for (int i = 0; i < project->module_count; i++) {
        if (focus_module_has_content(&project->modules[i], &slice->modules[i])) total++;
    }
    return total;
}

static bool call_targets_selected(const ProjectIndex* project, int caller_module_index,
                                  const IndexCall* call, const FocusSlice* selected,
                                  int* matched_import_index) {
    if (matched_import_index) *matched_import_index = -1;
    if (!project || !call || !selected ||
        caller_module_index < 0 || caller_module_index >= project->module_count) {
        return false;
    }

    const ModuleIndex* caller_module = &project->modules[caller_module_index];
    if (call->qualifier[0] == '\0') {
        int target_index = find_function_in_module(caller_module, call->name);
        if (target_index == -1) return false;
        return selected->modules[caller_module_index].functions &&
               selected->modules[caller_module_index].functions[target_index] != 0;
    }

    int import_index = find_import_in_module_by_qualifier(caller_module, call->qualifier);
    if (import_index == -1) return false;

    int target_module_index = find_module_index(project, caller_module->imports[import_index].resolved_path);
    if (target_module_index == -1) return false;

    int target_index = find_function_in_module(&project->modules[target_module_index], call->name);
    if (target_index == -1) return false;

    if (!selected->modules[target_module_index].functions ||
        selected->modules[target_module_index].functions[target_index] == 0) {
        return false;
    }

    if (matched_import_index) *matched_import_index = import_index;
    return true;
}

static bool buffer_appendf(char* out, size_t out_size, size_t* offset, const char* fmt, ...) {
    if (!out || !offset || !fmt || *offset >= out_size) return false;
    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(out + *offset, out_size - *offset, fmt, args);
    va_end(args);
    if (written < 0) return false;
    if ((size_t)written >= out_size - *offset) {
        *offset = out_size - 1;
        return false;
    }
    *offset += (size_t)written;
    return true;
}

static bool format_fn_contract(const IndexFn* fn, char* out, size_t out_size) {
    if (!fn || !out || out_size == 0) return false;
    size_t offset = 0;
    out[0] = '\0';

    if (!buffer_appendf(out, out_size, &offset, "%sfn %s(", fn->is_public ? "pub " : "", fn->name)) {
        return false;
    }
    for (int i = 0; i < fn->params.count; i++) {
        if (!buffer_appendf(out, out_size, &offset, "%s%s", i > 0 ? "," : "", fn->params.items[i])) {
            return false;
        }
    }
    if (!buffer_appendf(out, out_size, &offset, ")")) return false;
    if (fn->return_type[0] != '\0' &&
        !buffer_appendf(out, out_size, &offset, " -> %s", fn->return_type)) {
        return false;
    }
    if (!buffer_appendf(out, out_size, &offset, " effects=")) return false;
    if (fn->declared_effects.count == 0) {
        if (!buffer_appendf(out, out_size, &offset, "-")) return false;
    } else {
        for (int i = 0; i < fn->declared_effects.count; i++) {
            if (!buffer_appendf(out, out_size, &offset, "%s%s", i > 0 ? "," : "",
                                fn->declared_effects.items[i])) {
                return false;
            }
        }
    }
    if (!buffer_appendf(out, out_size, &offset, " inferred=")) return false;
    if (fn->inferred_effects.count == 0) {
        if (!buffer_appendf(out, out_size, &offset, "-")) return false;
    } else {
        for (int i = 0; i < fn->inferred_effects.count; i++) {
            if (!buffer_appendf(out, out_size, &offset, "%s%s", i > 0 ? "," : "",
                                fn->inferred_effects.items[i])) {
                return false;
            }
        }
    }
    if (!buffer_appendf(out, out_size, &offset, " calls=")) return false;
    if (fn->call_count == 0) {
        return buffer_appendf(out, out_size, &offset, "-");
    }
    for (int i = 0; i < fn->call_count; i++) {
        if (!buffer_appendf(out, out_size, &offset, "%s%s/%d", i > 0 ? "," : "",
                            fn->calls[i].target[0] ? fn->calls[i].target : fn->calls[i].name,
                            fn->calls[i].arg_count)) {
            return false;
        }
    }
    return true;
}

static bool format_var_contract(const IndexVar* var, char* out, size_t out_size) {
    if (!var || !out || out_size == 0) return false;
    if (var->type_name[0] != '\0') {
        return snprintf(out, out_size, "%s:%s", var->name, var->type_name) < (int)out_size;
    }
    return snprintf(out, out_size, "%s", var->name) < (int)out_size;
}

static bool format_struct_contract(const IndexStruct* st, char* out, size_t out_size) {
    if (!st || !out || out_size == 0) return false;
    size_t offset = 0;
    out[0] = '\0';
    if (!buffer_appendf(out, out_size, &offset, "%s%s(", st->is_public ? "pub " : "", st->name)) {
        return false;
    }
    for (int i = 0; i < st->field_count; i++) {
        if (!buffer_appendf(out, out_size, &offset, "%s%s", i > 0 ? "," : "", st->fields[i])) {
            return false;
        }
    }
    return buffer_appendf(out, out_size, &offset, ")");
}

static ModuleIndex* project_add_module(ProjectIndex* project, const char* path) {
    if (!ensure_capacity((void**)&project->modules, &project->module_cap,
                         project->module_count + 1, sizeof(ModuleIndex))) {
        return NULL;
    }

    ModuleIndex* module = &project->modules[project->module_count++];
    memset(module, 0, sizeof(*module));
    copy_text(module->path, sizeof(module->path), path);
    return module;
}

static bool index_module_ex(ProjectIndex* project, const char* module_path,
                            const StringList* allowed_paths) {
    if (find_module_index(project, module_path) != -1) return true;

    char* source = read_file(module_path);
    if (!source) return false;

    ModuleIndex* module = project_add_module(project, module_path);
    if (!module) {
        free(source);
        fprintf(stderr, "Indexer Error: Out of memory while adding module.\n");
        return false;
    }
    int module_index = project->module_count - 1;

    AstNode* ast = parse(source);
    if (!ast) {
        free(source);
        fprintf(stderr, "Indexer Error: Parse failed for \"%s\".\n", module_path);
        return false;
    }

    if (ast->type == AST_PROGRAM) {
        for (int i = 0; i < ast->data.block.count; i++) {
            AstNode* stmt = ast->data.block.statements[i];
            if (!stmt) continue;
            module = &project->modules[module_index];

            if (stmt->type == AST_VAR_DECL) {
                if (!module_add_var(module, stmt->data.var_decl.name, stmt->data.var_decl.type_annot)) {
                    free(source);
                    return false;
                }
            } else if (stmt->type == AST_FUNC_DECL) {
                IndexFn* fn = module_add_fn(module, stmt);
                if (!fn) {
                    free(source);
                    return false;
                }
                for (int j = 0; j < fn->declared_effects.count; j++) {
                    if (!string_list_add_unique(&module->inferred_effects, fn->declared_effects.items[j])) {
                        free(source);
                        return false;
                    }
                }
                for (int j = 0; j < fn->inferred_effects.count; j++) {
                    if (!string_list_add_unique(&module->inferred_effects, fn->inferred_effects.items[j])) {
                        free(source);
                        return false;
                    }
                }
            } else if (stmt->type == AST_STRUCT_DECL) {
                if (!module_add_struct(module, stmt)) {
                    free(source);
                    return false;
                }
            } else if (stmt->type == AST_USE_STMT) {
                char raw_path[MAX_PATH_LEN];
                char resolved_path[MAX_PATH_LEN];
                decode_use_path(stmt->data.use_stmt.path, raw_path, sizeof(raw_path));

                if (!resolve_module_path(module_path, raw_path, resolved_path, sizeof(resolved_path))) {
                    free(source);
                    fprintf(stderr, "Indexer Error: Could not resolve import in \"%s\".\n", module_path);
                    return false;
                }

                if (!module_add_import(module, raw_path, resolved_path,
                                       stmt->data.use_stmt.alias,
                                       stmt->data.use_stmt.path.line)) {
                    free(source);
                    return false;
                }

                if (allowed_paths && string_list_index_of(allowed_paths, resolved_path) == -1) {
                    continue;
                }

                if (!index_module_ex(project, resolved_path, allowed_paths)) {
                    free(source);
                    return false;
                }
            }

            module = &project->modules[module_index];
            if (!collect_stmt_semantics(stmt, &module->calls, &module->call_count, &module->call_cap,
                                        &module->inferred_effects, true)) {
                free(source);
                return false;
            }
        }
    } else {
        module = &project->modules[module_index];
        if (!collect_stmt_semantics(ast, &module->calls, &module->call_count, &module->call_cap,
                                    &module->inferred_effects, true)) {
            free(source);
            return false;
        }
    }

    free(source);
    return true;
}

static bool index_module(ProjectIndex* project, const char* module_path) {
    return index_module_ex(project, module_path, NULL);
}

static bool focus_module_has_content(const ModuleIndex* module, const FocusModule* focus_module) {
    if (!module || !focus_module) return false;
    if (count_selected(focus_module->variables, module->variable_count) > 0) return true;
    if (count_selected(focus_module->functions, module->function_count) > 0) return true;
    if (count_selected(focus_module->structs, module->struct_count) > 0) return true;
    if (count_selected(focus_module->imports, module->import_count) > 0) return true;
    return false;
}

static bool focus_collect_module_effects(const ModuleIndex* module, const FocusModule* focus_module,
                                         StringList* out) {
    if (!module || !focus_module || !out) return false;
    memset(out, 0, sizeof(*out));

    for (int i = 0; i < module->function_count; i++) {
        if (!focus_module->functions || !focus_module->functions[i]) continue;
        const IndexFn* fn = &module->functions[i];
        for (int j = 0; j < fn->declared_effects.count; j++) {
            if (!string_list_add_unique(out, fn->declared_effects.items[j])) {
                free_string_list(out);
                return false;
            }
        }
        for (int j = 0; j < fn->inferred_effects.count; j++) {
            if (!string_list_add_unique(out, fn->inferred_effects.items[j])) {
                free_string_list(out);
                return false;
            }
        }
    }

    return true;
}

static bool build_focus_slice_ex(const ProjectIndex* project, const char* focus_symbol,
                                 FocusSlice* slice, bool allow_missing) {
    if (!project || !focus_symbol || focus_symbol[0] == '\0' || !slice) return false;
    if (!init_focus_slice(slice, project)) {
        fprintf(stderr, "Indexer Error: Out of memory while building focus slice.\n");
        return false;
    }

    for (int i = 0; i < project->module_count; i++) {
        const ModuleIndex* module = &project->modules[i];

        for (int j = 0; j < module->variable_count; j++) {
            if (strcmp(module->variables[j].name, focus_symbol) == 0 && focus_mark_var(slice, i, j)) {
                slice->seed_count++;
            }
        }

        for (int j = 0; j < module->function_count; j++) {
            if (strcmp(module->functions[j].name, focus_symbol) == 0 && focus_mark_fn(slice, i, j)) {
                slice->seed_count++;
            }
        }

        for (int j = 0; j < module->struct_count; j++) {
            if (strcmp(module->structs[j].name, focus_symbol) == 0 && focus_mark_struct(slice, i, j)) {
                slice->seed_count++;
            }
        }
    }

    if (slice->seed_count == 0) {
        if (allow_missing) return true;
        fprintf(stderr, "Indexer Error: Focus symbol \"%s\" not found.\n", focus_symbol);
        free_focus_slice(slice);
        return false;
    }

    bool changed = true;
    while (changed) {
        changed = false;

        for (int i = 0; i < project->module_count; i++) {
            const ModuleIndex* module = &project->modules[i];
            FocusModule* focus_module = &slice->modules[i];

            for (int j = 0; j < module->function_count; j++) {
                if (!focus_module->functions || !focus_module->functions[j]) continue;
                const IndexFn* fn = &module->functions[j];

                for (int k = 0; k < fn->call_count; k++) {
                    const IndexCall* call = &fn->calls[k];

                    if (call->qualifier[0] == '\0') {
                        int callee_index = find_function_in_module(module, call->name);
                        if (callee_index != -1 && focus_mark_fn(slice, i, callee_index)) {
                            changed = true;
                        }
                        continue;
                    }

                    int import_index = find_import_in_module_by_qualifier(module, call->qualifier);
                    if (import_index == -1) continue;
                    if (focus_mark_import(slice, i, import_index)) changed = true;

                    int target_module_index =
                        find_module_index(project, module->imports[import_index].resolved_path);
                    if (target_module_index == -1) continue;

                    int callee_index =
                        find_function_in_module(&project->modules[target_module_index], call->name);
                    if (callee_index != -1 && focus_mark_fn(slice, target_module_index, callee_index)) {
                        changed = true;
                    }
                }
            }
        }
    }

    return true;
}

static bool build_focus_slice(const ProjectIndex* project, const char* focus_symbol, FocusSlice* slice) {
    return build_focus_slice_ex(project, focus_symbol, slice, false);
}

static bool build_impact_slice(const ProjectIndex* project, const char* focus_symbol, FocusSlice* slice) {
    if (!project || !slice) return false;
    if (!init_focus_slice(slice, project)) {
        fprintf(stderr, "Indexer Error: Out of memory while building impact slice.\n");
        return false;
    }

    FocusSlice active_targets;
    FocusSlice seed_targets;
    memset(&active_targets, 0, sizeof(active_targets));
    memset(&seed_targets, 0, sizeof(seed_targets));

    if (!init_focus_slice(&active_targets, project) || !init_focus_slice(&seed_targets, project)) {
        fprintf(stderr, "Indexer Error: Out of memory while building impact slice.\n");
        free_focus_slice(slice);
        free_focus_slice(&active_targets);
        free_focus_slice(&seed_targets);
        return false;
    }

    int seed_count = 0;
    for (int i = 0; i < project->module_count; i++) {
        const ModuleIndex* module = &project->modules[i];
        for (int j = 0; j < module->function_count; j++) {
            if (strcmp(module->functions[j].name, focus_symbol) != 0) continue;
            if (focus_mark_fn(&active_targets, i, j)) seed_count++;
            focus_mark_fn(&seed_targets, i, j);
        }
    }

    bool changed = seed_count > 0;
    while (changed) {
        changed = false;

        for (int i = 0; i < project->module_count; i++) {
            const ModuleIndex* module = &project->modules[i];

            for (int j = 0; j < module->function_count; j++) {
                const IndexFn* fn = &module->functions[j];
                bool is_seed = seed_targets.modules[i].functions &&
                               seed_targets.modules[i].functions[j] != 0;

                int matched_import_index = -1;
                bool matches_active = false;
                for (int k = 0; k < fn->call_count; k++) {
                    matched_import_index = -1;
                    if (!call_targets_selected(project, i, &fn->calls[k], &active_targets,
                                               &matched_import_index)) {
                        continue;
                    }
                    matches_active = true;
                    break;
                }

                if (!matches_active) continue;

                if (matched_import_index != -1) {
                    if (focus_mark_import(slice, i, matched_import_index)) changed = true;
                }

                if (is_seed) continue;

                if (focus_mark_fn(slice, i, j)) changed = true;
                if (focus_mark_fn(&active_targets, i, j)) changed = true;
            }
        }
    }

    free_focus_slice(&active_targets);
    free_focus_slice(&seed_targets);
    return true;
}

static void json_write_string(FILE* out, const char* text) {
    fputc('"', out);
    if (text) {
        for (const unsigned char* p = (const unsigned char*)text; *p; p++) {
            switch (*p) {
                case '"': fputs("\\\"", out); break;
                case '\\': fputs("\\\\", out); break;
                case '\n': fputs("\\n", out); break;
                case '\r': fputs("\\r", out); break;
                case '\t': fputs("\\t", out); break;
                default:
                    if (*p < 0x20) {
                        fprintf(out, "\\u%04x", *p);
                    } else {
                        fputc(*p, out);
                    }
                    break;
            }
        }
    }
    fputc('"', out);
}

static void json_write_string_array(FILE* out, const StringList* list) {
    fprintf(out, "[");
    if (list) {
        for (int i = 0; i < list->count; i++) {
            if (i > 0) fprintf(out, ", ");
            json_write_string(out, list->items[i]);
        }
    }
    fprintf(out, "]");
}

static void json_write_call_array(FILE* out, const IndexCall* calls, int count, const char* indent) {
    fprintf(out, "[");
    for (int i = 0; i < count; i++) {
        if (i == 0) fprintf(out, "\n");
        fprintf(out, "%s{\"target\": ", indent);
        json_write_string(out, calls[i].target);
        fprintf(out, ", \"name\": ");
        json_write_string(out, calls[i].name);
        fprintf(out, ", \"qualifier\": ");
        json_write_string(out, calls[i].qualifier);
        fprintf(out, ", \"argc\": %d, \"line\": %d}", calls[i].arg_count, calls[i].line);
        if (i + 1 < count) fprintf(out, ",");
        fprintf(out, "\n");
    }
    if (count > 0) {
        size_t len = strlen(indent);
        if (len >= 2) {
            char close_indent[64];
            size_t close_len = len - 2;
            if (close_len >= sizeof(close_indent)) close_len = sizeof(close_indent) - 1;
            memcpy(close_indent, indent, close_len);
            close_indent[close_len] = '\0';
            fprintf(out, "%s", close_indent);
        }
    }
    fprintf(out, "]");
}

static bool format_pack_path(const char* path, char* out, size_t out_size) {
    if (!path || !out || out_size == 0) return false;
    size_t root_len = strlen(index_project_root);
    if (root_len > 0 &&
        strncmp(path, index_project_root, root_len) == 0 &&
        (path[root_len] == '/' || path[root_len] == '\0')) {
        const char* rel = path + root_len;
        if (*rel == '/') rel++;
        snprintf(out, out_size, "%s", *rel ? rel : ".");
    } else {
        snprintf(out, out_size, "%s", path);
    }

    while (strstr(out, "/./") != NULL) {
        char* marker = strstr(out, "/./");
        memmove(marker + 1, marker + 3, strlen(marker + 3) + 1);
    }
    return true;
}

static bool resolve_state_path(const char* base_dir, const char* path, char* out, size_t out_size) {
    if (!path || !out || out_size == 0) return false;
    if (is_absolute_path(path)) {
        copy_text(out, out_size, path);
        return true;
    }
    if (!base_dir || base_dir[0] == '\0') return false;
    return join_path(base_dir, path, out, out_size);
}

static bool load_project_state_manifest(const char* state_path, ProjectStateManifest* manifest) {
    if (!state_path || !manifest) return false;
    memset(manifest, 0, sizeof(*manifest));

    FILE* file = fopen(state_path, "r");
    if (!file) {
        fprintf(stderr, "Indexer Error: Could not open state file \"%s\".\n", state_path);
        return false;
    }

    char line[4096];
    if (!fgets(line, sizeof(line), file)) {
        fclose(file);
        fprintf(stderr, "Indexer Error: Empty state file \"%s\".\n", state_path);
        return false;
    }
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
    if (strcmp(line, "PSTATEv1") != 0) {
        fclose(file);
        fprintf(stderr, "Indexer Error: Unsupported state file format \"%s\".\n", state_path);
        return false;
    }

    bool in_tracked_files = false;
    while (fgets(line, sizeof(line), file)) {
        n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';

        if (strcmp(line, "context:") == 0) break;
        if (strcmp(line, "tracked_files:") == 0) {
            in_tracked_files = true;
            continue;
        }

        if (in_tracked_files && strncmp(line, "  ", 2) == 0) {
            char* marker = strstr(line + 2, " sha256=");
            if (!marker) {
                fclose(file);
                free_project_state_manifest(manifest);
                fprintf(stderr, "Indexer Error: Malformed tracked file entry in \"%s\".\n", state_path);
                return false;
            }
            *marker = '\0';
            const char* rel_path = line + 2;
            const char* hash = marker + 8;
            if (!string_list_append(&manifest->tracked_paths, rel_path) ||
                !string_list_append(&manifest->tracked_hashes, hash)) {
                fclose(file);
                free_project_state_manifest(manifest);
                fprintf(stderr, "Indexer Error: Out of memory while reading state file.\n");
                return false;
            }
            continue;
        }

        in_tracked_files = false;
        if (strncmp(line, "root: ", 6) == 0) {
            copy_text(manifest->root, sizeof(manifest->root), line + 6);
        } else if (strncmp(line, "entry: ", 7) == 0) {
            copy_text(manifest->entry, sizeof(manifest->entry), line + 7);
        } else if (strncmp(line, "focus: ", 7) == 0) {
            if (strcmp(line + 7, "-") != 0) {
                copy_text(manifest->focus, sizeof(manifest->focus), line + 7);
            }
        } else if (strncmp(line, "impact: ", 8) == 0) {
            manifest->include_impact = strcmp(line + 8, "yes") == 0;
        } else if (strncmp(line, "semantic_items: ", 16) == 0) {
            manifest->semantic_item_count = atoi(line + 16);
        } else if (strncmp(line, "semantic_fingerprint: ", 22) == 0) {
            copy_text(manifest->semantic_fingerprint, sizeof(manifest->semantic_fingerprint), line + 22);
        }
    }

    fclose(file);
    if (manifest->root[0] == '\0' || manifest->entry[0] == '\0') {
        free_project_state_manifest(manifest);
        fprintf(stderr, "Indexer Error: Incomplete state file \"%s\".\n", state_path);
        return false;
    }
    if (manifest->tracked_paths.count != manifest->tracked_hashes.count) {
        free_project_state_manifest(manifest);
        fprintf(stderr, "Indexer Error: Corrupt tracked file list in \"%s\".\n", state_path);
        return false;
    }
    return true;
}

static bool stream_project_state_section(const char* state_path, const char* section_header,
                                         const char* stop_header, FILE* out) {
    if (!state_path || !section_header || !out) return false;
    FILE* file = fopen(state_path, "r");
    if (!file) return false;

    char line[4096];
    bool in_section = false;
    while (fgets(line, sizeof(line), file)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';

        if (!in_section) {
            if (strcmp(line, section_header) == 0) in_section = true;
            continue;
        }
        if (stop_header && strcmp(line, stop_header) == 0) break;

        if (n + 1 < sizeof(line)) {
            line[n] = '\n';
            line[n + 1] = '\0';
            fputs(line, out);
        } else {
            fputs(line, out);
            fputc('\n', out);
        }
    }

    fclose(file);
    return in_section;
}

static bool resolve_manifest_root(const char* state_path, const ProjectStateManifest* manifest,
                                  char* out, size_t out_size) {
    if (!state_path || !manifest || !out || out_size == 0) return false;
    if (is_absolute_path(manifest->root)) {
        copy_text(out, out_size, manifest->root);
        return true;
    }
    char state_dir[MAX_PATH_LEN];
    path_dirname(state_path, state_dir, sizeof(state_dir));
    return resolve_state_path(state_dir, manifest->root, out, out_size);
}

static bool check_project_state_manifest(const char* state_path, const ProjectStateManifest* manifest,
                                         StringList* stale_lines, int* stale_count) {
    if (!state_path || !manifest || !stale_count) return false;
    *stale_count = 0;

    char root_path[MAX_PATH_LEN];
    if (!resolve_manifest_root(state_path, manifest, root_path, sizeof(root_path))) return false;

    for (int i = 0; i < manifest->tracked_paths.count; i++) {
        char module_path[MAX_PATH_LEN];
        if (!resolve_state_path(root_path, manifest->tracked_paths.items[i], module_path, sizeof(module_path))) {
            return false;
        }

        char actual_hash[65];
        bool ok = compute_module_hash_hex(module_path, actual_hash);
        if (ok && strcmp(actual_hash, manifest->tracked_hashes.items[i]) == 0) continue;

        (*stale_count)++;
        if (stale_lines) {
            char line[2048];
            if (!ok) {
                if (snprintf(line, sizeof(line), "%s expected=%s actual=missing",
                             manifest->tracked_paths.items[i], manifest->tracked_hashes.items[i]) >=
                    (int)sizeof(line)) {
                    return false;
                }
            } else {
                if (snprintf(line, sizeof(line), "%s expected=%s actual=%s",
                             manifest->tracked_paths.items[i], manifest->tracked_hashes.items[i],
                             actual_hash) >= (int)sizeof(line)) {
                    return false;
                }
            }
            if (!string_list_add_unique(stale_lines, line)) return false;
        }
    }

    return true;
}

static void bytes_to_hex(const unsigned char* bytes, size_t len, char* out, size_t out_size) {
    if (!out || out_size == 0) return;
    if (!bytes || out_size < (len * 2) + 1) {
        out[0] = '\0';
        return;
    }
    for (size_t i = 0; i < len; i++) {
        snprintf(out + (i * 2), out_size - (i * 2), "%02x", bytes[i]);
    }
}

static bool compute_semantic_fingerprint(const SemanticItemList* items, char out_hex[65]) {
    if (!items || !out_hex) return false;
    size_t total_len = 0;
    for (int i = 0; i < items->count; i++) {
        total_len += strlen(items->items[i].text) + 1;
    }

    char* buffer = NULL;
    if (total_len > 0) {
        buffer = (char*)malloc(total_len);
        if (!buffer) return false;
        size_t offset = 0;
        for (int i = 0; i < items->count; i++) {
            size_t len = strlen(items->items[i].text);
            memcpy(buffer + offset, items->items[i].text, len);
            offset += len;
            buffer[offset++] = '\n';
        }
    }

    unsigned char digest[32];
    viper_sha256((const unsigned char*)(buffer ? buffer : ""),
                 buffer ? total_len : 0, digest);
    bytes_to_hex(digest, sizeof(digest), out_hex, 65);
    free(buffer);
    return out_hex[0] != '\0';
}

static bool collect_state_semantic_items(const ProjectIndex* project, const FocusSlice* focus_slice,
                                         const FocusSlice* impact_slice, bool include_impact,
                                         SemanticItemList* items) {
    if (!project || !items) return false;
    memset(items, 0, sizeof(*items));

    if (focus_slice) {
        if (!collect_focus_semantic_items(project, focus_slice, items)) {
            free_semantic_items(items);
            return false;
        }
    }

    if (include_impact && impact_slice) {
        if (!collect_impact_semantic_items(project, impact_slice, items)) {
            free_semantic_items(items);
            return false;
        }
    }

    return true;
}

static int string_list_index_of(const StringList* list, const char* text) {
    if (!list || !text) return -1;
    for (int i = 0; i < list->count; i++) {
        if (strcmp(list->items[i], text) == 0) return i;
    }
    return -1;
}

static bool build_stable_ref(const char* prefix, const char* source,
                             const StringList* used_refs, char* out, size_t out_size) {
    if (!prefix || !source || !out || out_size == 0) return false;

    unsigned char digest[32];
    char hex[65];
    viper_sha256((const unsigned char*)source, strlen(source), digest);
    bytes_to_hex(digest, sizeof(digest), hex, sizeof(hex));
    if (hex[0] == '\0') return false;

    size_t prefix_len = strlen(prefix);
    for (size_t take = 16; take <= 64; take += 8) {
        if (prefix_len + take + 1 > out_size) return false;
        if (snprintf(out, out_size, "%s%.*s", prefix, (int)take, hex) >= (int)out_size) {
            return false;
        }
        if (!used_refs || string_list_index_of(used_refs, out) == -1) return true;
    }

    return false;
}

static bool parse_semantic_item_text(const char* text, char* kind, size_t kind_size,
                                     char* module_path, size_t module_size,
                                     char* contract, size_t contract_size) {
    if (!text || !kind || !module_path || !contract ||
        kind_size == 0 || module_size == 0 || contract_size == 0) {
        return false;
    }

    const char* space = strchr(text, ' ');
    if (!space) return false;
    size_t kind_len = (size_t)(space - text);
    if (kind_len >= kind_size) kind_len = kind_size - 1;
    memcpy(kind, text, kind_len);
    kind[kind_len] = '\0';

    const char* payload = space + 1;
    const char* sep = strstr(payload, "::");
    if (!sep) return false;

    size_t module_len = (size_t)(sep - payload);
    if (module_len >= module_size) module_len = module_size - 1;
    memcpy(module_path, payload, module_len);
    module_path[module_len] = '\0';

    copy_text(contract, contract_size, sep + 2);
    return true;
}

static bool parse_csv_field(const char* text, StringList* out) {
    if (!text || !out) return false;
    if (strcmp(text, "-") == 0 || text[0] == '\0') return true;

    const char* start = text;
    while (*start) {
        const char* comma = strchr(start, ',');
        size_t len = comma ? (size_t)(comma - start) : strlen(start);
        char item[256];
        if (len >= sizeof(item)) len = sizeof(item) - 1;
        memcpy(item, start, len);
        item[len] = '\0';
        if (!string_list_append(out, item)) return false;
        if (!comma) break;
        start = comma + 1;
    }
    return true;
}

static bool parse_compact_symbol_row(const char* kind, const char* contract, CompactSymbolRow* row) {
    if (!kind || !contract || !row) return false;
    memset(row, 0, sizeof(*row));
    row->item_kind = (kind[0] == 't') ? 'T' : 'C';
    copy_text(row->raw_contract, sizeof(row->raw_contract), contract);

    const char* cursor = contract;
    if (strncmp(cursor, "pub fn ", 7) == 0) {
        row->parsed = true;
        row->is_public = true;
        cursor += 7;
    } else if (strncmp(cursor, "fn ", 3) == 0) {
        row->parsed = true;
        cursor += 3;
    } else {
        return true;
    }

    const char* open_paren = strchr(cursor, '(');
    const char* close_paren = open_paren ? strchr(open_paren, ')') : NULL;
    const char* effects_marker = close_paren ? strstr(close_paren, " effects=") : NULL;
    const char* inferred_marker = effects_marker ? strstr(effects_marker, " inferred=") : NULL;
    const char* calls_marker = inferred_marker ? strstr(inferred_marker, " calls=") : NULL;
    if (!open_paren || !close_paren || !effects_marker || !inferred_marker || !calls_marker) {
        row->parsed = false;
        return true;
    }

    size_t name_len = (size_t)(open_paren - cursor);
    if (name_len >= sizeof(row->name)) name_len = sizeof(row->name) - 1;
    memcpy(row->name, cursor, name_len);
    row->name[name_len] = '\0';
    size_t signature_len = (size_t)(close_paren - cursor + 1);
    if (signature_len >= sizeof(row->signature)) signature_len = sizeof(row->signature) - 1;
    memcpy(row->signature, cursor, signature_len);
    row->signature[signature_len] = '\0';

    if (close_paren > open_paren + 1) {
        row->arity = 1;
        for (const char* p = open_paren + 1; p < close_paren; p++) {
            if (*p == ',') row->arity++;
        }
    }

    const char* return_marker = strstr(close_paren, " -> ");
    if (return_marker && return_marker < effects_marker) {
        return_marker += 4;
        size_t return_len = (size_t)(effects_marker - return_marker);
        if (return_len >= sizeof(row->return_type)) return_len = sizeof(row->return_type) - 1;
        memcpy(row->return_type, return_marker, return_len);
        row->return_type[return_len] = '\0';
    }

    char declared[512];
    char inferred[512];
    char calls[1024];
    size_t declared_len = (size_t)(inferred_marker - (effects_marker + 9));
    if (declared_len >= sizeof(declared)) declared_len = sizeof(declared) - 1;
    memcpy(declared, effects_marker + 9, declared_len);
    declared[declared_len] = '\0';

    size_t inferred_len = (size_t)(calls_marker - (inferred_marker + 10));
    if (inferred_len >= sizeof(inferred)) inferred_len = sizeof(inferred) - 1;
    memcpy(inferred, inferred_marker + 10, inferred_len);
    inferred[inferred_len] = '\0';

    copy_text(calls, sizeof(calls), calls_marker + 7);

    return parse_csv_field(declared, &row->declared_effects) &&
           parse_csv_field(inferred, &row->inferred_effects) &&
           parse_csv_field(calls, &row->calls);
}

static bool parse_symbol_index_row(const char* line, char* symbol_id, size_t symbol_id_size,
                                   char* module_ref, size_t module_ref_size,
                                   char* kind, size_t kind_size,
                                   char* contract, size_t contract_size) {
    if (!line || !symbol_id || !module_ref || !kind || !contract ||
        symbol_id_size == 0 || module_ref_size == 0 || kind_size == 0 || contract_size == 0) {
        return false;
    }

    while (*line == ' ' || *line == '\t') line++;
    const char* symbol_sep = strchr(line, ' ');
    if (!symbol_sep) return false;
    size_t symbol_len = (size_t)(symbol_sep - line);
    if (symbol_len >= symbol_id_size) symbol_len = symbol_id_size - 1;
    memcpy(symbol_id, line, symbol_len);
    symbol_id[symbol_len] = '\0';

    const char* kind_start = symbol_sep + 1;
    const char* kind_sep = strchr(kind_start, ' ');
    if (!kind_sep) return false;
    size_t kind_len = (size_t)(kind_sep - kind_start);
    if (kind_len >= kind_size) kind_len = kind_size - 1;
    memcpy(kind, kind_start, kind_len);
    kind[kind_len] = '\0';

    const char* module_start = kind_sep + 1;
    const char* module_sep = strchr(module_start, ' ');
    if (!module_sep) return false;
    size_t module_len = (size_t)(module_sep - module_start);
    if (module_len >= module_ref_size) module_len = module_ref_size - 1;
    memcpy(module_ref, module_start, module_len);
    module_ref[module_len] = '\0';

    copy_text(contract, contract_size, module_sep + 1);
    return symbol_id[0] != '\0' && module_ref[0] != '\0' && kind[0] != '\0' && contract[0] != '\0';
}

static bool parse_symbol_module_row(const char* line, char* module_ref, size_t module_ref_size,
                                    char* module_path, size_t module_path_size) {
    if (!line || !module_ref || !module_path || module_ref_size == 0 || module_path_size == 0) {
        return false;
    }

    while (*line == ' ' || *line == '\t') line++;
    const char* ref_sep = strchr(line, ' ');
    if (!ref_sep) return false;

    size_t ref_len = (size_t)(ref_sep - line);
    if (ref_len >= module_ref_size) ref_len = module_ref_size - 1;
    memcpy(module_ref, line, ref_len);
    module_ref[ref_len] = '\0';

    copy_text(module_path, module_path_size, ref_sep + 1);
    return module_ref[0] != '\0' && module_path[0] != '\0';
}

static bool parse_stale_line_path(const char* line, char* path, size_t path_size) {
    if (!line || !path || path_size == 0) return false;
    const char* marker = strstr(line, " expected=");
    if (!marker) return false;

    size_t len = (size_t)(marker - line);
    if (len >= path_size) len = path_size - 1;
    memcpy(path, line, len);
    path[len] = '\0';
    return path[0] != '\0';
}

static bool collect_state_module_refs_for_paths(const char* state_path, const StringList* target_paths,
                                                StringList* module_refs, StringList* module_paths) {
    if (!state_path || !target_paths || !module_refs || !module_paths) return false;

    FILE* file = fopen(state_path, "r");
    if (!file) return false;

    char line[4096];
    bool in_modules = false;
    bool ok = true;
    while (fgets(line, sizeof(line), file)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';

        if (!in_modules) {
            if (strcmp(line, "symbol_modules:") == 0) in_modules = true;
            continue;
        }
        if (strcmp(line, "symbol_index:") == 0) break;
        if (line[0] == '\0') continue;

        char module_ref[32];
        char module_path[MAX_PATH_LEN];
        if (!parse_symbol_module_row(line, module_ref, sizeof(module_ref),
                                     module_path, sizeof(module_path))) {
            ok = false;
            break;
        }
        if (string_list_index_of(target_paths, module_path) == -1) continue;
        if (string_list_index_of(module_refs, module_ref) != -1) continue;
        if (!string_list_append(module_refs, module_ref) ||
            !string_list_append(module_paths, module_path)) {
            ok = false;
            break;
        }
    }

    fclose(file);
    if (!ok) {
        free_string_list(module_refs);
        free_string_list(module_paths);
    }
    return ok && in_modules;
}

static bool collect_stale_module_refs(const char* state_path, const StringList* stale_lines,
                                      StringList* module_refs, StringList* module_paths) {
    if (!state_path || !stale_lines || !module_refs || !module_paths) return false;

    StringList stale_paths;
    memset(&stale_paths, 0, sizeof(stale_paths));
    for (int i = 0; i < stale_lines->count; i++) {
        char path[MAX_PATH_LEN];
        if (!parse_stale_line_path(stale_lines->items[i], path, sizeof(path))) {
            free_string_list(&stale_paths);
            return false;
        }
        if (!string_list_add_unique(&stale_paths, path)) {
            free_string_list(&stale_paths);
            return false;
        }
    }

    bool ok = collect_state_module_refs_for_paths(state_path, &stale_paths, module_refs, module_paths);
    free_string_list(&stale_paths);
    return ok;
}

static bool emit_stale_module_section(FILE* out, const StringList* module_refs,
                                      const StringList* module_paths) {
    if (!out || !module_refs || !module_paths) return false;
    if (module_refs->count != module_paths->count || module_refs->count == 0) return true;

    fprintf(out, "stale_modules:\n");
    for (int i = 0; i < module_refs->count; i++) {
        fprintf(out, "  %s %s\n", module_refs->items[i], module_paths->items[i]);
    }
    return true;
}

static bool emit_stale_symbol_section(const char* state_path, FILE* out, const StringList* module_refs) {
    if (!state_path || !out || !module_refs) return false;
    if (module_refs->count == 0) return true;

    FILE* file = fopen(state_path, "r");
    if (!file) return false;

    fprintf(out, "stale_symbols:\n");
    char line[8192];
    bool in_index = false;
    bool ok = true;
    while (fgets(line, sizeof(line), file)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';

        if (!in_index) {
            if (strcmp(line, "symbol_index:") == 0) in_index = true;
            continue;
        }
        if (strcmp(line, "summary_items:") == 0) break;
        if (line[0] == '\0') continue;

        char symbol_id[32];
        char module_ref[32];
        char kind[16];
        char contract[4608];
        if (!parse_symbol_index_row(line, symbol_id, sizeof(symbol_id),
                                    module_ref, sizeof(module_ref),
                                    kind, sizeof(kind),
                                    contract, sizeof(contract))) {
            ok = false;
            break;
        }
        if (string_list_index_of(module_refs, module_ref) == -1) continue;
        fprintf(out, "  %s\n", symbol_id);
    }

    fclose(file);
    return ok && in_index;
}

static bool module_ref_selected(const char* module_ref, const StringList* module_refs) {
    if (!module_ref) return false;
    if (!module_refs || module_refs->count == 0) return true;
    return string_list_index_of(module_refs, module_ref) != -1;
}

static bool collect_resume_symbol_tables_filtered(const char* state_path, const StringList* module_refs,
                                                  StringList* returns, StringList* effects,
                                                  StringList* calls, int* symbol_count) {
    if (!state_path || !returns || !effects || !calls) return false;

    FILE* file = fopen(state_path, "r");
    if (!file) return false;

    char line[8192];
    bool in_index = false;
    bool ok = true;
    int collected = 0;
    while (fgets(line, sizeof(line), file)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';

        if (!in_index) {
            if (strcmp(line, "symbol_index:") == 0) in_index = true;
            continue;
        }
        if (strcmp(line, "summary_items:") == 0) break;
        if (line[0] == '\0') continue;

        char symbol_id[32];
        char module_ref[32];
        char kind[16];
        char contract[4608];
        if (!parse_symbol_index_row(line, symbol_id, sizeof(symbol_id),
                                    module_ref, sizeof(module_ref),
                                    kind, sizeof(kind),
                                    contract, sizeof(contract))) {
            ok = false;
            break;
        }
        if (!module_ref_selected(module_ref, module_refs)) continue;

        CompactSymbolRow row;
        if (!parse_compact_symbol_row(kind[0] == 'T' ? "target" : "caller", contract, &row)) {
            ok = false;
            break;
        }

        if (row.parsed) {
            collected++;
            if (row.return_type[0] != '\0' && !string_list_add_unique(returns, row.return_type)) ok = false;
            for (int i = 0; ok && i < row.declared_effects.count; i++) {
                if (!string_list_add_unique(effects, row.declared_effects.items[i])) ok = false;
            }
            for (int i = 0; ok && i < row.inferred_effects.count; i++) {
                if (!string_list_add_unique(effects, row.inferred_effects.items[i])) ok = false;
            }
            for (int i = 0; ok && i < row.calls.count; i++) {
                if (!string_list_add_unique(calls, row.calls.items[i])) ok = false;
            }
        }

        free_compact_symbol_row(&row);
        if (!ok) break;
    }

    fclose(file);
    if (symbol_count) *symbol_count = collected;
    return ok && in_index;
}

static bool emit_compact_symbol_refs(FILE* out, const char* label, char prefix, const StringList* items) {
    if (!out || !label || !items) return false;
    fprintf(out, "%s:\n", label);
    for (int i = 0; i < items->count; i++) {
        fprintf(out, "  %c%d=%s\n", prefix, i + 1, items->items[i]);
    }
    return true;
}

static bool emit_compact_symbol_id_list(FILE* out, char prefix,
                                        const StringList* values, const StringList* table) {
    if (!out || !values || !table) return false;
    if (values->count == 0) {
        fprintf(out, "-");
        return true;
    }
    for (int i = 0; i < values->count; i++) {
        int index = string_list_index_of(table, values->items[i]);
        if (index == -1) return false;
        fprintf(out, "%s%c%d", i > 0 ? "," : "", prefix, index + 1);
    }
    return true;
}

static bool emit_resume_symbol_ledger(const char* state_path, FILE* out) {
    if (!state_path || !out) return false;

    StringList returns;
    StringList effects;
    StringList calls;
    memset(&returns, 0, sizeof(returns));
    memset(&effects, 0, sizeof(effects));
    memset(&calls, 0, sizeof(calls));

    if (!collect_resume_symbol_tables_filtered(state_path, NULL, &returns, &effects, &calls, NULL)) {
        free_string_list(&returns);
        free_string_list(&effects);
        free_string_list(&calls);
        return false;
    }

    fprintf(out, "mods:\n");
    if (!stream_project_state_section(state_path, "symbol_modules:", "symbol_index:", out)) {
        free_string_list(&returns);
        free_string_list(&effects);
        free_string_list(&calls);
        return false;
    }
    if (!emit_compact_symbol_refs(out, "rets", 'r', &returns) ||
        !emit_compact_symbol_refs(out, "effs", 'e', &effects) ||
        !emit_compact_symbol_refs(out, "calls", 'c', &calls)) {
        free_string_list(&returns);
        free_string_list(&effects);
        free_string_list(&calls);
        return false;
    }

    FILE* file = fopen(state_path, "r");
    if (!file) {
        free_string_list(&returns);
        free_string_list(&effects);
        free_string_list(&calls);
        return false;
    }

    fprintf(out, "syms:\n");
    char line[8192];
    bool in_index = false;
    bool ok = true;
    while (fgets(line, sizeof(line), file)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';

        if (!in_index) {
            if (strcmp(line, "symbol_index:") == 0) in_index = true;
            continue;
        }
        if (strcmp(line, "summary_items:") == 0) break;
        if (line[0] == '\0') continue;

        char symbol_id[32];
        char module_ref[32];
        char kind[16];
        char contract[4608];
        if (!parse_symbol_index_row(line, symbol_id, sizeof(symbol_id),
                                    module_ref, sizeof(module_ref),
                                    kind, sizeof(kind),
                                    contract, sizeof(contract))) {
            ok = false;
            break;
        }

        CompactSymbolRow row;
        if (!parse_compact_symbol_row(kind[0] == 'T' ? "target" : "caller", contract, &row)) {
            ok = false;
            break;
        }

        if (!row.parsed) {
            fprintf(out, "  %s|%s|%s|raw|%s\n", symbol_id, kind, module_ref, row.raw_contract);
            free_compact_symbol_row(&row);
            continue;
        }

        fprintf(out, "  %s|%s|%s|%c|%s|", symbol_id, kind, module_ref,
                row.is_public ? '+' : '-',
                row.signature[0] != '\0' ? row.signature : row.name);
        if (row.return_type[0] == '\0') {
            fprintf(out, "-|");
        } else {
            int return_index = string_list_index_of(&returns, row.return_type);
            if (return_index == -1) {
                ok = false;
                free_compact_symbol_row(&row);
                break;
            }
            fprintf(out, "r%d|", return_index + 1);
        }
        if (!emit_compact_symbol_id_list(out, 'e', &row.declared_effects, &effects) ||
            fprintf(out, "|") < 0 ||
            !emit_compact_symbol_id_list(out, 'e', &row.inferred_effects, &effects) ||
            fprintf(out, "|") < 0 ||
            !emit_compact_symbol_id_list(out, 'c', &row.calls, &calls)) {
            ok = false;
            free_compact_symbol_row(&row);
            break;
        }
        fprintf(out, "\n");
        free_compact_symbol_row(&row);
    }

    fclose(file);
    free_string_list(&returns);
    free_string_list(&effects);
    free_string_list(&calls);
    return ok && in_index;
}

static bool emit_patch_symbol_ledger(const char* state_path, const StringList* module_refs,
                                     const StringList* module_paths, FILE* out, int* patched_symbols) {
    if (!state_path || !module_refs || !module_paths || !out) return false;
    if (patched_symbols) *patched_symbols = 0;
    if (module_refs->count == 0) return true;

    StringList returns;
    StringList effects;
    StringList calls;
    memset(&returns, 0, sizeof(returns));
    memset(&effects, 0, sizeof(effects));
    memset(&calls, 0, sizeof(calls));

    int symbol_count = 0;
    if (!collect_resume_symbol_tables_filtered(state_path, module_refs, &returns, &effects, &calls, &symbol_count)) {
        free_string_list(&returns);
        free_string_list(&effects);
        free_string_list(&calls);
        return false;
    }

    fprintf(out, "patch_mods:\n");
    for (int i = 0; i < module_refs->count; i++) {
        fprintf(out, "  %s %s\n", module_refs->items[i], module_paths->items[i]);
    }
    if (!emit_compact_symbol_refs(out, "patch_rets", 'r', &returns) ||
        !emit_compact_symbol_refs(out, "patch_effs", 'e', &effects) ||
        !emit_compact_symbol_refs(out, "patch_calls", 'c', &calls)) {
        free_string_list(&returns);
        free_string_list(&effects);
        free_string_list(&calls);
        return false;
    }

    FILE* file = fopen(state_path, "r");
    if (!file) {
        free_string_list(&returns);
        free_string_list(&effects);
        free_string_list(&calls);
        return false;
    }

    fprintf(out, "patch_syms:\n");
    char line[8192];
    bool in_index = false;
    bool ok = true;
    while (fgets(line, sizeof(line), file)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';

        if (!in_index) {
            if (strcmp(line, "symbol_index:") == 0) in_index = true;
            continue;
        }
        if (strcmp(line, "summary_items:") == 0) break;
        if (line[0] == '\0') continue;

        char symbol_id[32];
        char module_ref[32];
        char kind[16];
        char contract[4608];
        if (!parse_symbol_index_row(line, symbol_id, sizeof(symbol_id),
                                    module_ref, sizeof(module_ref),
                                    kind, sizeof(kind),
                                    contract, sizeof(contract))) {
            ok = false;
            break;
        }
        if (!module_ref_selected(module_ref, module_refs)) continue;

        CompactSymbolRow row;
        if (!parse_compact_symbol_row(kind[0] == 'T' ? "target" : "caller", contract, &row)) {
            ok = false;
            break;
        }

        if (!row.parsed) {
            fprintf(out, "  %s|%s|%s|raw|%s\n", symbol_id, kind, module_ref, row.raw_contract);
            free_compact_symbol_row(&row);
            continue;
        }

        fprintf(out, "  %s|%s|%s|%c|%s|", symbol_id, kind, module_ref,
                row.is_public ? '+' : '-',
                row.signature[0] != '\0' ? row.signature : row.name);
        if (row.return_type[0] == '\0') {
            fprintf(out, "-|");
        } else {
            int return_index = string_list_index_of(&returns, row.return_type);
            if (return_index == -1) {
                ok = false;
                free_compact_symbol_row(&row);
                break;
            }
            fprintf(out, "r%d|", return_index + 1);
        }
        if (!emit_compact_symbol_id_list(out, 'e', &row.declared_effects, &effects) ||
            fprintf(out, "|") < 0 ||
            !emit_compact_symbol_id_list(out, 'e', &row.inferred_effects, &effects) ||
            fprintf(out, "|") < 0 ||
            !emit_compact_symbol_id_list(out, 'c', &row.calls, &calls)) {
            ok = false;
            free_compact_symbol_row(&row);
            break;
        }
        fprintf(out, "\n");
        free_compact_symbol_row(&row);
    }

    fclose(file);
    free_string_list(&returns);
    free_string_list(&effects);
    free_string_list(&calls);
    if (patched_symbols) *patched_symbols = symbol_count;
    return ok && in_index;
}

static bool emit_symbol_index_sections(FILE* out, const SemanticItemList* items) {
    if (!out || !items) return false;

    StringList modules;
    StringList module_refs;
    StringList symbol_refs;
    memset(&modules, 0, sizeof(modules));
    memset(&module_refs, 0, sizeof(module_refs));
    memset(&symbol_refs, 0, sizeof(symbol_refs));
    for (int i = 0; i < items->count; i++) {
        char kind[16];
        char module_path[MAX_PATH_LEN];
        char contract[4608];
        if (!parse_semantic_item_text(items->items[i].text, kind, sizeof(kind),
                                      module_path, sizeof(module_path),
                                      contract, sizeof(contract))) {
            free_string_list(&modules);
            free_string_list(&module_refs);
            free_string_list(&symbol_refs);
            return false;
        }
        if (!string_list_add_unique(&modules, module_path)) {
            free_string_list(&modules);
            free_string_list(&module_refs);
            free_string_list(&symbol_refs);
            return false;
        }
    }

    for (int i = 0; i < modules.count; i++) {
        char module_ref[32];
        if (!build_stable_ref("m", modules.items[i], &module_refs, module_ref, sizeof(module_ref)) ||
            !string_list_append(&module_refs, module_ref)) {
            free_string_list(&modules);
            free_string_list(&module_refs);
            free_string_list(&symbol_refs);
            return false;
        }
    }

    fprintf(out, "symbol_modules:\n");
    for (int i = 0; i < modules.count; i++) {
        fprintf(out, "  %s %s\n", module_refs.items[i], modules.items[i]);
    }

    fprintf(out, "symbol_index:\n");
    for (int i = 0; i < items->count; i++) {
        char kind[16];
        char module_path[MAX_PATH_LEN];
        char contract[4608];
        if (!parse_semantic_item_text(items->items[i].text, kind, sizeof(kind),
                                      module_path, sizeof(module_path),
                                      contract, sizeof(contract))) {
            free_string_list(&modules);
            free_string_list(&module_refs);
            free_string_list(&symbol_refs);
            return false;
        }
        int module_index = string_list_index_of(&modules, module_path);
        if (module_index == -1) {
            free_string_list(&modules);
            free_string_list(&module_refs);
            free_string_list(&symbol_refs);
            return false;
        }
        char symbol_ref[32];
        if (!build_stable_ref("s", items->items[i].key, &symbol_refs, symbol_ref, sizeof(symbol_ref)) ||
            !string_list_append(&symbol_refs, symbol_ref)) {
            free_string_list(&modules);
            free_string_list(&module_refs);
            free_string_list(&symbol_refs);
            return false;
        }
        fprintf(out, "  %s %c %s %s\n", symbol_ref,
                strcmp(kind, "target") == 0 ? 'T' : 'C',
                module_refs.items[module_index], contract);
    }

    free_string_list(&modules);
    free_string_list(&module_refs);
    free_string_list(&symbol_refs);
    return true;
}

static bool compute_module_hash_hex(const char* module_path, char out_hex[65]) {
    if (!module_path || !out_hex) return false;
    char* source = read_file(module_path);
    if (!source) return false;
    unsigned char digest[32];
    viper_sha256((const unsigned char*)source, strlen(source), digest);
    bytes_to_hex(digest, sizeof(digest), out_hex, 65);
    free(source);
    return out_hex[0] != '\0';
}

static bool state_module_selected(const ProjectIndex* project, int module_index,
                                  const FocusSlice* focus_slice, const FocusSlice* impact_slice,
                                  bool focused) {
    if (!project || module_index < 0 || module_index >= project->module_count) return false;
    if (!focused) return true;
    if (focus_slice && focus_module_has_content(&project->modules[module_index], &focus_slice->modules[module_index])) {
        return true;
    }
    if (impact_slice && focus_module_has_content(&project->modules[module_index], &impact_slice->modules[module_index])) {
        return true;
    }
    return false;
}

static void pack_write_path(FILE* out, const char* path) {
    if (!path) return;
    char display_path[MAX_PATH_LEN];
    if (!format_pack_path(path, display_path, sizeof(display_path))) return;
    fprintf(out, "%s", display_path);
}

static void pack_write_csv(FILE* out, const StringList* list) {
    if (!list || list->count == 0) {
        fprintf(out, "-");
        return;
    }
    for (int i = 0; i < list->count; i++) {
        if (i > 0) fprintf(out, ",");
        fprintf(out, "%s", list->items[i]);
    }
}

static void pack_write_fn_line(FILE* out, const IndexFn* fn) {
    if (!out || !fn) return;
    char contract[4096];
    if (!format_fn_contract(fn, contract, sizeof(contract))) return;
    fprintf(out, "%s", contract);
}

static bool collect_focus_semantic_items(const ProjectIndex* project, const FocusSlice* slice,
                                         SemanticItemList* out) {
    if (!project || !slice || !out) return false;

    for (int i = 0; i < project->module_count; i++) {
        const ModuleIndex* module = &project->modules[i];
        const FocusModule* focus_module = &slice->modules[i];
        if (!focus_module_has_content(module, focus_module)) continue;

        char module_path[MAX_PATH_LEN];
        if (!format_pack_path(module->path, module_path, sizeof(module_path))) return false;

        for (int j = 0; j < module->variable_count; j++) {
            if (!focus_module->variables || !focus_module->variables[j]) continue;
            char contract[512];
            char key[640];
            char text[1024];
            if (!format_var_contract(&module->variables[j], contract, sizeof(contract))) return false;
            if (snprintf(key, sizeof(key), "target:var:%s", contract) >= (int)sizeof(key)) return false;
            if (snprintf(text, sizeof(text), "target %s::var %s", module_path, contract) >= (int)sizeof(text)) {
                return false;
            }
            if (!semantic_items_add(out, key, text)) return false;
        }

        for (int j = 0; j < module->struct_count; j++) {
            if (!focus_module->structs || !focus_module->structs[j]) continue;
            char contract[1024];
            char key[1152];
            char text[1536];
            if (!format_struct_contract(&module->structs[j], contract, sizeof(contract))) return false;
            if (snprintf(key, sizeof(key), "target:struct:%s", contract) >= (int)sizeof(key)) return false;
            if (snprintf(text, sizeof(text), "target %s::struct %s", module_path, contract) >= (int)sizeof(text)) {
                return false;
            }
            if (!semantic_items_add(out, key, text)) return false;
        }

        for (int j = 0; j < module->function_count; j++) {
            if (!focus_module->functions || !focus_module->functions[j]) continue;
            char contract[4096];
            char key[4352];
            char text[4608];
            if (!format_fn_contract(&module->functions[j], contract, sizeof(contract))) return false;
            if (snprintf(key, sizeof(key), "target:fn:%s", contract) >= (int)sizeof(key)) return false;
            if (snprintf(text, sizeof(text), "target %s::%s", module_path, contract) >= (int)sizeof(text)) {
                return false;
            }
            if (!semantic_items_add(out, key, text)) return false;
        }
    }

    return true;
}

static bool collect_impact_semantic_items(const ProjectIndex* project, const FocusSlice* slice,
                                          SemanticItemList* out) {
    if (!project || !slice || !out) return false;

    for (int i = 0; i < project->module_count; i++) {
        const ModuleIndex* module = &project->modules[i];
        const FocusModule* focus_module = &slice->modules[i];
        if (!focus_module->functions ||
            count_selected(focus_module->functions, module->function_count) == 0) {
            continue;
        }

        char module_path[MAX_PATH_LEN];
        if (!format_pack_path(module->path, module_path, sizeof(module_path))) return false;

        for (int j = 0; j < module->function_count; j++) {
            if (!focus_module->functions[j]) continue;
            char contract[4096];
            char key[4352];
            char text[4608];
            if (!format_fn_contract(&module->functions[j], contract, sizeof(contract))) return false;
            if (snprintf(key, sizeof(key), "caller:fn:%s", contract) >= (int)sizeof(key)) return false;
            if (snprintf(text, sizeof(text), "caller %s::%s", module_path, contract) >= (int)sizeof(text)) {
                return false;
            }
            if (!semantic_items_add(out, key, text)) return false;
        }
    }

    return true;
}

static void emit_index_json(FILE* out, const ProjectIndex* project, const char* entry_file) {
    int total_vars = 0;
    int total_fns = 0;
    int total_structs = 0;
    int total_calls = 0;
    int total_imports = 0;
    int total_effect_markers = 0;

    for (int i = 0; i < project->module_count; i++) {
        total_vars += project->modules[i].variable_count;
        total_fns += project->modules[i].function_count;
        total_structs += project->modules[i].struct_count;
        total_calls += project->modules[i].call_count;
        total_imports += project->modules[i].import_count;
        total_effect_markers += project->modules[i].inferred_effects.count;
    }

    fprintf(out, "{\n");
    fprintf(out, "  \"schema_version\": 2,\n");
    fprintf(out, "  \"entry_file\": ");
    json_write_string(out, entry_file);
    fprintf(out, ",\n");
    fprintf(out, "  \"modules\": [\n");

    for (int i = 0; i < project->module_count; i++) {
        const ModuleIndex* m = &project->modules[i];

        fprintf(out, "    {\n");
        fprintf(out, "      \"path\": ");
        json_write_string(out, m->path);
        fprintf(out, ",\n");

        fprintf(out, "      \"imports\": [");
        for (int j = 0; j < m->import_count; j++) {
            if (j == 0) fprintf(out, "\n");
            fprintf(out, "        {\"raw\": ");
            json_write_string(out, m->imports[j].raw_path);
            fprintf(out, ", \"resolved\": ");
            json_write_string(out, m->imports[j].resolved_path);
            fprintf(out, ", \"alias\": ");
            json_write_string(out, m->imports[j].alias);
            fprintf(out, ", \"line\": %d}", m->imports[j].line);
            if (j + 1 < m->import_count) fprintf(out, ",");
            fprintf(out, "\n");
        }
        if (m->import_count > 0) fprintf(out, "      ");
        fprintf(out, "],\n");

        fprintf(out, "      \"exports\": {\n");

        fprintf(out, "        \"variables\": [");
        for (int j = 0; j < m->variable_count; j++) {
            if (j == 0) fprintf(out, "\n");
            fprintf(out, "          {\"name\": ");
            json_write_string(out, m->variables[j].name);
            fprintf(out, ", \"type\": ");
            json_write_string(out, m->variables[j].type_name);
            fprintf(out, ", \"line\": %d}", m->variables[j].line);
            if (j + 1 < m->variable_count) fprintf(out, ",");
            fprintf(out, "\n");
        }
        if (m->variable_count > 0) fprintf(out, "        ");
        fprintf(out, "],\n");

        fprintf(out, "        \"functions\": [");
        for (int j = 0; j < m->function_count; j++) {
            if (j == 0) fprintf(out, "\n");
            fprintf(out, "          {\"name\": ");
            json_write_string(out, m->functions[j].name);
            fprintf(out, ", \"params\": ");
            json_write_string_array(out, &m->functions[j].params);
            fprintf(out, ", \"arity\": %d", m->functions[j].arity);
            fprintf(out, ", \"return_type\": ");
            json_write_string(out, m->functions[j].return_type);
            fprintf(out, ", \"public\": %s", m->functions[j].is_public ? "true" : "false");
            fprintf(out, ", \"declared_effects\": ");
            json_write_string_array(out, &m->functions[j].declared_effects);
            fprintf(out, ", \"inferred_effects\": ");
            json_write_string_array(out, &m->functions[j].inferred_effects);
            fprintf(out, ", \"calls\": ");
            json_write_call_array(out, m->functions[j].calls, m->functions[j].call_count, "            ");
            fprintf(out, ", \"line\": %d}", m->functions[j].line);
            if (j + 1 < m->function_count) fprintf(out, ",");
            fprintf(out, "\n");
        }
        if (m->function_count > 0) fprintf(out, "        ");
        fprintf(out, "],\n");

        fprintf(out, "        \"structs\": [");
        for (int j = 0; j < m->struct_count; j++) {
            if (j == 0) fprintf(out, "\n");
            fprintf(out, "          {\"name\": ");
            json_write_string(out, m->structs[j].name);
            fprintf(out, ", \"public\": %s, \"line\": %d, \"fields\": [",
                    m->structs[j].is_public ? "true" : "false", m->structs[j].line);
            for (int k = 0; k < m->structs[j].field_count; k++) {
                json_write_string(out, m->structs[j].fields[k]);
                if (k + 1 < m->structs[j].field_count) fprintf(out, ", ");
            }
            fprintf(out, "]}");
            if (j + 1 < m->struct_count) fprintf(out, ",");
            fprintf(out, "\n");
        }
        if (m->struct_count > 0) fprintf(out, "        ");
        fprintf(out, "]\n");

        fprintf(out, "      },\n");

        fprintf(out, "      \"effects\": ");
        json_write_string_array(out, &m->inferred_effects);
        fprintf(out, ",\n");

        fprintf(out, "      \"calls\": [");
        for (int j = 0; j < m->call_count; j++) {
            if (j == 0) fprintf(out, "\n");
            fprintf(out, "        {\"target\": ");
            json_write_string(out, m->calls[j].target);
            fprintf(out, ", \"name\": ");
            json_write_string(out, m->calls[j].name);
            fprintf(out, ", \"qualifier\": ");
            json_write_string(out, m->calls[j].qualifier);
            fprintf(out, ", \"argc\": %d, \"line\": %d}",
                    m->calls[j].arg_count, m->calls[j].line);
            if (j + 1 < m->call_count) fprintf(out, ",");
            fprintf(out, "\n");
        }
        if (m->call_count > 0) fprintf(out, "      ");
        fprintf(out, "]\n");

        fprintf(out, "    }");
        if (i + 1 < project->module_count) fprintf(out, ",");
        fprintf(out, "\n");
    }

    fprintf(out, "  ],\n");
    fprintf(out, "  \"summary\": {\n");
    fprintf(out, "    \"module_count\": %d,\n", project->module_count);
    fprintf(out, "    \"exported_variables\": %d,\n", total_vars);
    fprintf(out, "    \"exported_functions\": %d,\n", total_fns);
    fprintf(out, "    \"exported_structs\": %d,\n", total_structs);
    fprintf(out, "    \"imports\": %d,\n", total_imports);
    fprintf(out, "    \"calls\": %d,\n", total_calls);
    fprintf(out, "    \"effect_markers\": %d\n", total_effect_markers);
    fprintf(out, "  }\n");
    fprintf(out, "}\n");
}

static void emit_context_pack_text(FILE* out, const ProjectIndex* project, const char* entry_file,
                                   const FocusSlice* focus_slice, const FocusSlice* impact_slice,
                                   const char* focus_symbol, bool include_impact) {
    bool focused = focus_slice != NULL && focus_symbol != NULL && focus_symbol[0] != '\0';
    int total_modules = 0;
    int total_vars = 0;
    int total_fns = 0;
    int total_structs = 0;
    int total_calls = 0;
    int total_imports = 0;
    int total_effects = 0;

    for (int i = 0; i < project->module_count; i++) {
        const ModuleIndex* module = &project->modules[i];
        const FocusModule* focus_module = focused ? &focus_slice->modules[i] : NULL;
        if (focused && !focus_module_has_content(module, focus_module)) continue;

        total_modules++;
        if (focused) {
            total_vars += count_selected(focus_module->variables, module->variable_count);
            total_fns += count_selected(focus_module->functions, module->function_count);
            total_structs += count_selected(focus_module->structs, module->struct_count);
            total_imports += count_selected(focus_module->imports, module->import_count);

            for (int j = 0; j < module->function_count; j++) {
                if (focus_module->functions && focus_module->functions[j]) {
                    total_calls += module->functions[j].call_count;
                }
            }

            StringList focus_effects;
            if (focus_collect_module_effects(module, focus_module, &focus_effects)) {
                total_effects += focus_effects.count;
                free_string_list(&focus_effects);
            }
        } else {
            total_vars += module->variable_count;
            total_fns += module->function_count;
            total_structs += module->struct_count;
            total_calls += module->call_count;
            total_imports += module->import_count;
            total_effects += module->inferred_effects.count;
        }
    }

    fprintf(out, "CTXv1\n");
    fprintf(out, "entry: ");
    pack_write_path(out, entry_file);
    fprintf(out, "\n");
    if (focused) {
        fprintf(out, "focus: %s\n", focus_symbol);
    }
    fprintf(out, "summary: modules=%d vars=%d fns=%d structs=%d imports=%d calls=%d effects=%d\n",
            total_modules, total_vars, total_fns, total_structs, total_imports, total_calls, total_effects);
    if (focused && include_impact && impact_slice != NULL) {
        int impact_callers = 0;
        for (int i = 0; i < project->module_count; i++) {
            impact_callers += count_selected(impact_slice->modules[i].functions,
                                             project->modules[i].function_count);
        }
        fprintf(out, "impact: modules=%d callers=%d\n",
                count_selected_modules(project, impact_slice), impact_callers);
    }

    for (int i = 0; i < project->module_count; i++) {
        const ModuleIndex* m = &project->modules[i];
        const FocusModule* focus_module = focused ? &focus_slice->modules[i] : NULL;
        if (focused && !focus_module_has_content(m, focus_module)) continue;

        fprintf(out, "\nmodule: ");
        pack_write_path(out, m->path);
        fprintf(out, "\n");

        if (m->import_count > 0) {
            int selected_imports = focused ? count_selected(focus_module->imports, m->import_count)
                                           : m->import_count;
            if (selected_imports > 0) {
                fprintf(out, "imports: ");
                bool first = true;
                for (int j = 0; j < m->import_count; j++) {
                    if (focused && (!focus_module->imports || !focus_module->imports[j])) continue;
                    if (!first) fprintf(out, ", ");
                    if (m->imports[j].alias[0] != '\0') fprintf(out, "%s=", m->imports[j].alias);
                    pack_write_path(out, m->imports[j].resolved_path);
                    first = false;
                }
                fprintf(out, "\n");
            }
        }

        fprintf(out, "effects: ");
        if (focused) {
            StringList focus_effects;
            if (!focus_collect_module_effects(m, focus_module, &focus_effects)) {
                fprintf(out, "-\n");
            } else {
                pack_write_csv(out, &focus_effects);
                fprintf(out, "\n");
                free_string_list(&focus_effects);
            }
        } else {
            pack_write_csv(out, &m->inferred_effects);
            fprintf(out, "\n");
        }

        if (m->variable_count > 0) {
            int selected_vars = focused ? count_selected(focus_module->variables, m->variable_count)
                                        : m->variable_count;
            if (selected_vars > 0) {
                fprintf(out, "vars:\n");
                for (int j = 0; j < m->variable_count; j++) {
                    if (focused && (!focus_module->variables || !focus_module->variables[j])) continue;
                    fprintf(out, "  %s", m->variables[j].name);
                    if (m->variables[j].type_name[0] != '\0') {
                        fprintf(out, ":%s", m->variables[j].type_name);
                    }
                    fprintf(out, "\n");
                }
            }
        }

        if (m->struct_count > 0) {
            int selected_structs = focused ? count_selected(focus_module->structs, m->struct_count)
                                           : m->struct_count;
            if (selected_structs > 0) {
                fprintf(out, "structs:\n");
                for (int j = 0; j < m->struct_count; j++) {
                    if (focused && (!focus_module->structs || !focus_module->structs[j])) continue;
                    fprintf(out, "  %s%s(", m->structs[j].is_public ? "pub " : "", m->structs[j].name);
                    for (int k = 0; k < m->structs[j].field_count; k++) {
                        if (k > 0) fprintf(out, ",");
                        fprintf(out, "%s", m->structs[j].fields[k]);
                    }
                    fprintf(out, ")\n");
                }
            }
        }

        if (m->function_count > 0) {
            int selected_fns = focused ? count_selected(focus_module->functions, m->function_count)
                                       : m->function_count;
            if (selected_fns > 0) {
                fprintf(out, "fns:\n");
                for (int j = 0; j < m->function_count; j++) {
                    if (focused && (!focus_module->functions || !focus_module->functions[j])) continue;
                    const IndexFn* fn = &m->functions[j];
                    fprintf(out, "  ");
                    pack_write_fn_line(out, fn);
                    fprintf(out, "\n");
                }
            }
        }
    }

    if (focused && include_impact && impact_slice != NULL) {
        int impact_callers = 0;
        for (int i = 0; i < project->module_count; i++) {
            impact_callers += count_selected(impact_slice->modules[i].functions,
                                             project->modules[i].function_count);
        }

        if (impact_callers > 0) {
            fprintf(out, "\ncallers:\n");
            for (int i = 0; i < project->module_count; i++) {
                const ModuleIndex* module = &project->modules[i];
                const FocusModule* impact_module = &impact_slice->modules[i];
                if (!impact_module->functions ||
                    count_selected(impact_module->functions, module->function_count) == 0) {
                    continue;
                }

                for (int j = 0; j < module->function_count; j++) {
                    if (!impact_module->functions[j]) continue;
                    fprintf(out, "  ");
                    pack_write_path(out, module->path);
                    fprintf(out, "::");
                    pack_write_fn_line(out, &module->functions[j]);
                    fprintf(out, "\n");
                }
            }
        }
    }
}

static bool build_project_index(const char* entry_file, ProjectIndex* project,
                                char* entry_path, size_t entry_path_size) {
    if (!resolve_entry_path(entry_file, entry_path, entry_path_size)) {
        fprintf(stderr, "Indexer Error: Could not resolve entry file path.\n");
        return false;
    }

    if (!find_project_root_from_entry(entry_path, index_project_root, sizeof(index_project_root))) {
        fprintf(stderr, "Indexer Error: Could not resolve project root.\n");
        return false;
    }

    memset(project, 0, sizeof(*project));
    if (!index_module(project, entry_path)) {
        free_project(project);
        return false;
    }
    return true;
}

static bool resolve_manifest_tracked_paths(const char* state_path, const ProjectStateManifest* manifest,
                                           StringList* out_paths) {
    if (!state_path || !manifest || !out_paths) return false;

    char root_path[MAX_PATH_LEN];
    if (!resolve_manifest_root(state_path, manifest, root_path, sizeof(root_path))) return false;

    memset(out_paths, 0, sizeof(*out_paths));
    for (int i = 0; i < manifest->tracked_paths.count; i++) {
        char abs_path[MAX_PATH_LEN];
        if (!resolve_state_path(root_path, manifest->tracked_paths.items[i], abs_path, sizeof(abs_path)) ||
            !string_list_add_unique(out_paths, abs_path)) {
            free_string_list(out_paths);
            return false;
        }
    }
    return true;
}

static bool build_project_index_filtered(const char* root_path, const StringList* tracked_paths,
                                         ProjectIndex* project) {
    if (!root_path || !tracked_paths || !project) return false;
    copy_text(index_project_root, sizeof(index_project_root), root_path);
    memset(project, 0, sizeof(*project));

    for (int i = 0; i < tracked_paths->count; i++) {
        if (!index_module_ex(project, tracked_paths->items[i], tracked_paths)) {
            free_project(project);
            return false;
        }
    }
    return true;
}

static FILE* open_index_output(const char* out_path) {
    if (!out_path || out_path[0] == '\0') return stdout;
    FILE* out = fopen(out_path, "w");
    if (!out) {
        fprintf(stderr, "Indexer Error: Could not open output file \"%s\".\n", out_path);
        return NULL;
    }
    return out;
}

bool resume_project_state(const char* state_path, const char* out_path) {
    ProjectStateManifest manifest;
    if (!load_project_state_manifest(state_path, &manifest)) return false;

    StringList stale_lines;
    StringList stale_module_refs;
    StringList stale_module_paths;
    memset(&stale_lines, 0, sizeof(stale_lines));
    memset(&stale_module_refs, 0, sizeof(stale_module_refs));
    memset(&stale_module_paths, 0, sizeof(stale_module_paths));
    int stale_count = 0;
    bool ok = check_project_state_manifest(state_path, &manifest, &stale_lines, &stale_count);
    if (!ok) {
        free_string_list(&stale_lines);
        free_string_list(&stale_module_refs);
        free_string_list(&stale_module_paths);
        free_project_state_manifest(&manifest);
        fprintf(stderr, "Indexer Error: Could not resume from state file \"%s\".\n", state_path);
        return false;
    }
    if (stale_count > 0 &&
        !collect_stale_module_refs(state_path, &stale_lines, &stale_module_refs, &stale_module_paths)) {
        free_string_list(&stale_lines);
        free_string_list(&stale_module_refs);
        free_string_list(&stale_module_paths);
        free_project_state_manifest(&manifest);
        fprintf(stderr, "Indexer Error: Could not map stale modules in state file \"%s\".\n", state_path);
        return false;
    }

    FILE* out = open_index_output(out_path);
    if (!out) {
        free_string_list(&stale_lines);
        free_string_list(&stale_module_refs);
        free_string_list(&stale_module_paths);
        free_project_state_manifest(&manifest);
        return false;
    }

    char root_path[MAX_PATH_LEN];
    char entry_path[MAX_PATH_LEN];
    if (!resolve_manifest_root(state_path, &manifest, root_path, sizeof(root_path)) ||
        !resolve_state_path(root_path, manifest.entry, entry_path, sizeof(entry_path))) {
        if (out != stdout) fclose(out);
        free_string_list(&stale_lines);
        free_string_list(&stale_module_refs);
        free_string_list(&stale_module_paths);
        free_project_state_manifest(&manifest);
        return false;
    }

    char entry_display[MAX_PATH_LEN];
    format_pack_path(entry_path, entry_display, sizeof(entry_display));

    fprintf(out, "PRESUMEv4\n");
    fprintf(out, "state: %s\n", state_path);
    fprintf(out, "entry: %s\n", entry_display);
    fprintf(out, "focus: %s\n", manifest.focus[0] != '\0' ? manifest.focus : "-");
    fprintf(out, "impact: %s\n", manifest.include_impact ? "yes" : "no");
    fprintf(out, "valid: %s\n", stale_count == 0 ? "yes" : "no");
    fprintf(out, "changed_files: %d\n", stale_count);
    fprintf(out, "semantic_items: %d\n", manifest.semantic_item_count);
    fprintf(out, "semantic_fingerprint: %s\n",
            manifest.semantic_fingerprint[0] != '\0' ? manifest.semantic_fingerprint : "-");
    if (stale_count > 0) {
        fprintf(out, "stale_files:\n");
        for (int i = 0; i < stale_lines.count; i++) {
            fprintf(out, "  %s\n", stale_lines.items[i]);
        }
        if (!emit_stale_module_section(out, &stale_module_refs, &stale_module_paths) ||
            !emit_stale_symbol_section(state_path, out, &stale_module_refs)) {
            if (out != stdout) fclose(out);
            free_string_list(&stale_lines);
            free_string_list(&stale_module_refs);
            free_string_list(&stale_module_paths);
            free_project_state_manifest(&manifest);
            fprintf(stderr, "Indexer Error: Could not emit stale symbol map for \"%s\".\n", state_path);
            return false;
        }
    }
    if (!emit_resume_symbol_ledger(state_path, out)) {
        if (out != stdout) fclose(out);
        free_string_list(&stale_lines);
        free_string_list(&stale_module_refs);
        free_string_list(&stale_module_paths);
        free_project_state_manifest(&manifest);
        fprintf(stderr, "Indexer Error: Missing embedded symbol ledger in state file \"%s\".\n", state_path);
        return false;
    }

    if (out != stdout) fclose(out);
    free_string_list(&stale_lines);
    free_string_list(&stale_module_refs);
    free_string_list(&stale_module_paths);
    free_project_state_manifest(&manifest);
    return true;
}

bool verify_project_state(const char* state_path, const char* out_path) {
    ProjectStateManifest manifest;
    if (!load_project_state_manifest(state_path, &manifest)) return false;

    StringList stale_lines;
    StringList stale_module_refs;
    StringList stale_module_paths;
    memset(&stale_lines, 0, sizeof(stale_lines));
    memset(&stale_module_refs, 0, sizeof(stale_module_refs));
    memset(&stale_module_paths, 0, sizeof(stale_module_paths));
    int stale_count = 0;
    bool ok = check_project_state_manifest(state_path, &manifest, &stale_lines, &stale_count);
    if (!ok) {
        free_string_list(&stale_lines);
        free_string_list(&stale_module_refs);
        free_string_list(&stale_module_paths);
        free_project_state_manifest(&manifest);
        fprintf(stderr, "Indexer Error: Could not verify state file \"%s\".\n", state_path);
        return false;
    }
    if (stale_count > 0 &&
        !collect_stale_module_refs(state_path, &stale_lines, &stale_module_refs, &stale_module_paths)) {
        free_string_list(&stale_lines);
        free_string_list(&stale_module_refs);
        free_string_list(&stale_module_paths);
        free_project_state_manifest(&manifest);
        fprintf(stderr, "Indexer Error: Could not map stale modules in state file \"%s\".\n", state_path);
        return false;
    }

    FILE* out = open_index_output(out_path);
    if (!out) {
        free_string_list(&stale_lines);
        free_string_list(&stale_module_refs);
        free_string_list(&stale_module_paths);
        free_project_state_manifest(&manifest);
        return false;
    }

    char root_path[MAX_PATH_LEN];
    char entry_path[MAX_PATH_LEN];
    if (!resolve_manifest_root(state_path, &manifest, root_path, sizeof(root_path)) ||
        !resolve_state_path(root_path, manifest.entry, entry_path, sizeof(entry_path))) {
        if (out != stdout) fclose(out);
        free_string_list(&stale_lines);
        free_string_list(&stale_module_refs);
        free_string_list(&stale_module_paths);
        free_project_state_manifest(&manifest);
        return false;
    }

    char entry_display[MAX_PATH_LEN];
    format_pack_path(entry_path, entry_display, sizeof(entry_display));

    fprintf(out, "PSTATECHKv1\n");
    fprintf(out, "state: %s\n", state_path);
    fprintf(out, "entry: %s\n", entry_display);
    fprintf(out, "focus: %s\n", manifest.focus[0] != '\0' ? manifest.focus : "-");
    fprintf(out, "impact: %s\n", manifest.include_impact ? "yes" : "no");
    fprintf(out, "tracked_modules: %d\n", manifest.tracked_paths.count);
    fprintf(out, "valid: %s\n", stale_count == 0 ? "yes" : "no");
    fprintf(out, "changed_files: %d\n", stale_count);
    if (stale_count > 0) {
        fprintf(out, "stale_files:\n");
        for (int i = 0; i < stale_lines.count; i++) {
            fprintf(out, "  %s\n", stale_lines.items[i]);
        }
        if (!emit_stale_module_section(out, &stale_module_refs, &stale_module_paths) ||
            !emit_stale_symbol_section(state_path, out, &stale_module_refs)) {
            if (out != stdout) fclose(out);
            free_string_list(&stale_lines);
            free_string_list(&stale_module_refs);
            free_string_list(&stale_module_paths);
            free_project_state_manifest(&manifest);
            fprintf(stderr, "Indexer Error: Could not emit stale symbol map for \"%s\".\n", state_path);
            return false;
        }
    }

    if (out != stdout) fclose(out);
    free_string_list(&stale_lines);
    free_string_list(&stale_module_refs);
    free_string_list(&stale_module_paths);
    free_project_state_manifest(&manifest);
    return true;
}

bool refresh_project_state(const char* state_path, const char* out_path) {
    ProjectStateManifest manifest;
    if (!load_project_state_manifest(state_path, &manifest)) return false;

    StringList stale_lines;
    StringList stale_module_refs;
    StringList stale_module_paths;
    StringList refreshed_module_refs;
    StringList refreshed_module_paths;
    memset(&stale_lines, 0, sizeof(stale_lines));
    memset(&stale_module_refs, 0, sizeof(stale_module_refs));
    memset(&stale_module_paths, 0, sizeof(stale_module_paths));
    memset(&refreshed_module_refs, 0, sizeof(refreshed_module_refs));
    memset(&refreshed_module_paths, 0, sizeof(refreshed_module_paths));
    int stale_count = 0;
    if (!check_project_state_manifest(state_path, &manifest, &stale_lines, &stale_count)) {
        free_string_list(&stale_lines);
        free_string_list(&stale_module_refs);
        free_string_list(&stale_module_paths);
        free_string_list(&refreshed_module_refs);
        free_string_list(&refreshed_module_paths);
        free_project_state_manifest(&manifest);
        fprintf(stderr, "Indexer Error: Could not refresh state file \"%s\".\n", state_path);
        return false;
    }
    if (stale_count > 0 &&
        !collect_stale_module_refs(state_path, &stale_lines, &stale_module_refs, &stale_module_paths)) {
        free_string_list(&stale_lines);
        free_string_list(&stale_module_refs);
        free_string_list(&stale_module_paths);
        free_string_list(&refreshed_module_refs);
        free_string_list(&refreshed_module_paths);
        free_project_state_manifest(&manifest);
        fprintf(stderr, "Indexer Error: Could not map stale modules in state file \"%s\".\n", state_path);
        return false;
    }

    char root_path[MAX_PATH_LEN];
    char entry_path[MAX_PATH_LEN];
    if (!resolve_manifest_root(state_path, &manifest, root_path, sizeof(root_path)) ||
        !resolve_state_path(root_path, manifest.entry, entry_path, sizeof(entry_path))) {
        free_string_list(&stale_lines);
        free_string_list(&stale_module_refs);
        free_string_list(&stale_module_paths);
        free_string_list(&refreshed_module_refs);
        free_string_list(&refreshed_module_paths);
        free_project_state_manifest(&manifest);
        return false;
    }

    char temp_state_path[MAX_PATH_LEN];
    temp_state_path[0] = '\0';
    ProjectStateManifest refreshed_manifest;
    ProjectIndex refreshed_project;
    StringList tracked_module_paths;
    memset(&refreshed_manifest, 0, sizeof(refreshed_manifest));
    memset(&refreshed_project, 0, sizeof(refreshed_project));
    memset(&tracked_module_paths, 0, sizeof(tracked_module_paths));
    bool semantic_changed = false;

    if (stale_count > 0) {
        if (snprintf(temp_state_path, sizeof(temp_state_path), "%s.tmp-%ld",
                     state_path, (long)getpid()) >= (int)sizeof(temp_state_path)) {
            free_string_list(&stale_lines);
            free_string_list(&stale_module_refs);
            free_string_list(&stale_module_paths);
            free_string_list(&refreshed_module_refs);
            free_string_list(&refreshed_module_paths);
            free_project_state_manifest(&manifest);
            fprintf(stderr, "Indexer Error: Could not allocate temp state path for \"%s\".\n", state_path);
            return false;
        }
        unlink(temp_state_path);

        if (!resolve_manifest_tracked_paths(state_path, &manifest, &tracked_module_paths) ||
            !build_project_index_filtered(root_path, &tracked_module_paths, &refreshed_project) ||
            !emit_project_state_from_project(&refreshed_project, entry_path, temp_state_path,
                                             manifest.focus[0] != '\0' ? manifest.focus : NULL,
                                             manifest.include_impact)) {
            unlink(temp_state_path);
            free_string_list(&stale_lines);
            free_string_list(&stale_module_refs);
            free_string_list(&stale_module_paths);
            free_string_list(&refreshed_module_refs);
            free_string_list(&refreshed_module_paths);
            free_string_list(&tracked_module_paths);
            free_project(&refreshed_project);
            free_project_state_manifest(&manifest);
            return false;
        }
        if (!load_project_state_manifest(temp_state_path, &refreshed_manifest)) {
            unlink(temp_state_path);
            free_string_list(&stale_lines);
            free_string_list(&stale_module_refs);
            free_string_list(&stale_module_paths);
            free_string_list(&refreshed_module_refs);
            free_string_list(&refreshed_module_paths);
            free_string_list(&tracked_module_paths);
            free_project(&refreshed_project);
            free_project_state_manifest(&manifest);
            fprintf(stderr, "Indexer Error: Could not reload refreshed state for \"%s\".\n", state_path);
            return false;
        }
        semantic_changed = manifest.semantic_item_count != refreshed_manifest.semantic_item_count ||
                           strcmp(manifest.semantic_fingerprint, refreshed_manifest.semantic_fingerprint) != 0;

        if (semantic_changed &&
            !collect_state_module_refs_for_paths(temp_state_path, &stale_module_paths,
                                                 &refreshed_module_refs, &refreshed_module_paths)) {
            unlink(temp_state_path);
            free_string_list(&stale_lines);
            free_string_list(&stale_module_refs);
            free_string_list(&stale_module_paths);
            free_string_list(&refreshed_module_refs);
            free_string_list(&refreshed_module_paths);
            free_string_list(&tracked_module_paths);
            free_project(&refreshed_project);
            free_project_state_manifest(&refreshed_manifest);
            free_project_state_manifest(&manifest);
            fprintf(stderr, "Indexer Error: Could not map refreshed modules in state file \"%s\".\n", state_path);
            return false;
        }
        if (rename(temp_state_path, state_path) != 0) {
            unlink(temp_state_path);
            free_string_list(&stale_lines);
            free_string_list(&stale_module_refs);
            free_string_list(&stale_module_paths);
            free_string_list(&refreshed_module_refs);
            free_string_list(&refreshed_module_paths);
            free_string_list(&tracked_module_paths);
            free_project(&refreshed_project);
            free_project_state_manifest(&refreshed_manifest);
            free_project_state_manifest(&manifest);
            fprintf(stderr, "Indexer Error: Could not replace state file \"%s\".\n", state_path);
            return false;
        }
    }

    FILE* out = open_index_output(out_path);
    if (!out) {
        free_string_list(&stale_lines);
        free_string_list(&stale_module_refs);
        free_string_list(&stale_module_paths);
        free_string_list(&refreshed_module_refs);
        free_string_list(&refreshed_module_paths);
        free_project_state_manifest(&manifest);
        return false;
    }

    char entry_display[MAX_PATH_LEN];
    format_pack_path(entry_path, entry_display, sizeof(entry_display));

    fprintf(out, "PSTATEREFRESHv3\n");
    fprintf(out, "state: %s\n", state_path);
    fprintf(out, "entry: %s\n", entry_display);
    fprintf(out, "focus: %s\n", manifest.focus[0] != '\0' ? manifest.focus : "-");
    fprintf(out, "impact: %s\n", manifest.include_impact ? "yes" : "no");
    fprintf(out, "changed_files: %d\n", stale_count);
    fprintf(out, "status: %s\n",
            stale_count == 0 ? "unchanged" : (semantic_changed ? "refreshed" : "rehashed"));
    if (stale_count > 0) {
        int patched_symbols = 0;
        fprintf(out, "patched_modules: %d\n", semantic_changed ? refreshed_module_refs.count : 0);
        if (semantic_changed) {
            if (!emit_patch_symbol_ledger(state_path, &refreshed_module_refs, &refreshed_module_paths, out,
                                          &patched_symbols)) {
                if (out != stdout) fclose(out);
                free_string_list(&stale_lines);
                free_string_list(&stale_module_refs);
                free_string_list(&stale_module_paths);
                free_string_list(&refreshed_module_refs);
                free_string_list(&refreshed_module_paths);
                free_project_state_manifest(&refreshed_manifest);
                free_project_state_manifest(&manifest);
                fprintf(stderr, "Indexer Error: Could not emit refresh patch for \"%s\".\n", state_path);
                return false;
            }
        }
        fprintf(out, "patched_symbols: %d\n", patched_symbols);
    }

    if (out != stdout) fclose(out);
    free_string_list(&stale_lines);
    free_string_list(&stale_module_refs);
    free_string_list(&stale_module_paths);
    free_string_list(&refreshed_module_refs);
    free_string_list(&refreshed_module_paths);
    free_string_list(&tracked_module_paths);
    free_project(&refreshed_project);
    free_project_state_manifest(&refreshed_manifest);
    free_project_state_manifest(&manifest);
    return true;
}

static bool emit_project_state_from_project(const ProjectIndex* project, const char* entry_path,
                                            const char* out_path, const char* focus_symbol,
                                            bool include_impact) {
    if (!project || !entry_path) return false;
    FILE* out = open_index_output(out_path);
    if (!out) return false;

    FocusSlice focus_slice;
    FocusSlice* focus_slice_ptr = NULL;
    FocusSlice impact_slice;
    FocusSlice* impact_slice_ptr = NULL;
    SemanticItemList semantic_items;
    memset(&focus_slice, 0, sizeof(focus_slice));
    memset(&impact_slice, 0, sizeof(impact_slice));
    memset(&semantic_items, 0, sizeof(semantic_items));

    bool focused = focus_symbol && focus_symbol[0] != '\0';
    if (focused) {
        if (!build_focus_slice(project, focus_symbol, &focus_slice)) {
            if (out != stdout) fclose(out);
            return false;
        }
        focus_slice_ptr = &focus_slice;

        if (include_impact) {
            if (!build_impact_slice(project, focus_symbol, &impact_slice)) {
                if (out != stdout) fclose(out);
                free_focus_slice(&focus_slice);
                return false;
            }
            impact_slice_ptr = &impact_slice;
        }
    }

    if (!collect_state_semantic_items(project, focus_slice_ptr, impact_slice_ptr,
                                      include_impact, &semantic_items)) {
        if (out != stdout) fclose(out);
        free_focus_slice(&impact_slice);
        free_focus_slice(&focus_slice);
        return false;
    }

    int tracked_modules = 0;
    for (int i = 0; i < project->module_count; i++) {
        if (state_module_selected(project, i, focus_slice_ptr, impact_slice_ptr, focused)) {
            tracked_modules++;
        }
    }

    char entry_display[MAX_PATH_LEN];
    format_pack_path(entry_path, entry_display, sizeof(entry_display));
    char semantic_fingerprint[65];
    if (!compute_semantic_fingerprint(&semantic_items, semantic_fingerprint)) {
        if (out != stdout) fclose(out);
        free_semantic_items(&semantic_items);
        free_focus_slice(&impact_slice);
        free_focus_slice(&focus_slice);
        return false;
    }

    fprintf(out, "PSTATEv1\n");
    fprintf(out, "root: %s\n", index_project_root[0] != '\0' ? index_project_root : ".");
    fprintf(out, "entry: %s\n", entry_display);
    fprintf(out, "scope: %s\n", focused ? "focused" : "project");
    fprintf(out, "focus: %s\n", focused ? focus_symbol : "-");
    fprintf(out, "impact: %s\n", include_impact ? "yes" : "no");
    fprintf(out, "tracked_modules: %d\n", tracked_modules);
    fprintf(out, "semantic_items: %d\n", semantic_items.count);
    fprintf(out, "semantic_fingerprint: %s\n", semantic_fingerprint);
    fprintf(out, "tracked_files:\n");
    for (int i = 0; i < project->module_count; i++) {
        if (!state_module_selected(project, i, focus_slice_ptr, impact_slice_ptr, focused)) continue;

        char module_path[MAX_PATH_LEN];
        char hash_hex[65];
        if (!format_pack_path(project->modules[i].path, module_path, sizeof(module_path)) ||
            !compute_module_hash_hex(project->modules[i].path, hash_hex)) {
            if (out != stdout) fclose(out);
            free_focus_slice(&impact_slice);
            free_focus_slice(&focus_slice);
            return false;
        }
        fprintf(out, "  %s sha256=%s\n", module_path, hash_hex);
    }
    if (!emit_symbol_index_sections(out, &semantic_items)) {
        if (out != stdout) fclose(out);
        free_semantic_items(&semantic_items);
        free_focus_slice(&impact_slice);
        free_focus_slice(&focus_slice);
        return false;
    }
    fprintf(out, "summary_items:\n");
    for (int i = 0; i < semantic_items.count; i++) {
        fprintf(out, "  %s\n", semantic_items.items[i].text);
    }
    fprintf(out, "context:\n");
    emit_context_pack_text(out, project, entry_path, focus_slice_ptr, impact_slice_ptr,
                           focus_symbol, include_impact);

    if (out != stdout) fclose(out);
    free_semantic_items(&semantic_items);
    free_focus_slice(&impact_slice);
    free_focus_slice(&focus_slice);
    return true;
}

bool emit_project_state(const char* entry_file, const char* out_path,
                        const char* focus_symbol, bool include_impact) {
    char entry_path[MAX_PATH_LEN];
    ProjectIndex project;
    if (!build_project_index(entry_file, &project, entry_path, sizeof(entry_path))) return false;

    bool ok = emit_project_state_from_project(&project, entry_path, out_path, focus_symbol, include_impact);
    free_project(&project);
    return ok;
}

bool emit_semantic_diff(const char* before_entry, const char* after_entry, const char* out_path,
                        const char* focus_symbol, bool include_impact) {
    if (!before_entry || !after_entry || !focus_symbol || focus_symbol[0] == '\0') {
        fprintf(stderr, "Indexer Error: semantic diff requires before, after, and focus symbol.\n");
        return false;
    }

    char before_path[MAX_PATH_LEN];
    char after_path[MAX_PATH_LEN];
    ProjectIndex before_project;
    ProjectIndex after_project;
    memset(&before_project, 0, sizeof(before_project));
    memset(&after_project, 0, sizeof(after_project));

    if (!build_project_index(before_entry, &before_project, before_path, sizeof(before_path))) return false;
    if (!build_project_index(after_entry, &after_project, after_path, sizeof(after_path))) {
        free_project(&before_project);
        return false;
    }

    FocusSlice before_focus;
    FocusSlice after_focus;
    FocusSlice before_impact;
    FocusSlice after_impact;
    memset(&before_focus, 0, sizeof(before_focus));
    memset(&after_focus, 0, sizeof(after_focus));
    memset(&before_impact, 0, sizeof(before_impact));
    memset(&after_impact, 0, sizeof(after_impact));

    if (!build_focus_slice_ex(&before_project, focus_symbol, &before_focus, true) ||
        !build_focus_slice_ex(&after_project, focus_symbol, &after_focus, true)) {
        free_focus_slice(&before_focus);
        free_focus_slice(&after_focus);
        free_project(&before_project);
        free_project(&after_project);
        return false;
    }

    if (before_focus.seed_count == 0 && after_focus.seed_count == 0) {
        fprintf(stderr, "Indexer Error: Focus symbol \"%s\" not found in either diff input.\n", focus_symbol);
        free_focus_slice(&before_focus);
        free_focus_slice(&after_focus);
        free_project(&before_project);
        free_project(&after_project);
        return false;
    }

    if (include_impact) {
        if (!build_impact_slice(&before_project, focus_symbol, &before_impact) ||
            !build_impact_slice(&after_project, focus_symbol, &after_impact)) {
            free_focus_slice(&before_impact);
            free_focus_slice(&after_impact);
            free_focus_slice(&before_focus);
            free_focus_slice(&after_focus);
            free_project(&before_project);
            free_project(&after_project);
            return false;
        }
    }

    SemanticItemList before_items;
    SemanticItemList after_items;
    memset(&before_items, 0, sizeof(before_items));
    memset(&after_items, 0, sizeof(after_items));

    bool ok = collect_focus_semantic_items(&before_project, &before_focus, &before_items) &&
              collect_focus_semantic_items(&after_project, &after_focus, &after_items);
    if (ok && include_impact) {
        ok = collect_impact_semantic_items(&before_project, &before_impact, &before_items) &&
             collect_impact_semantic_items(&after_project, &after_impact, &after_items);
    }
    if (!ok) {
        fprintf(stderr, "Indexer Error: Could not build semantic diff items.\n");
        free_semantic_items(&before_items);
        free_semantic_items(&after_items);
        free_focus_slice(&before_impact);
        free_focus_slice(&after_impact);
        free_focus_slice(&before_focus);
        free_focus_slice(&after_focus);
        free_project(&before_project);
        free_project(&after_project);
        return false;
    }

    FILE* out = open_index_output(out_path);
    if (!out) {
        free_semantic_items(&before_items);
        free_semantic_items(&after_items);
        free_focus_slice(&before_impact);
        free_focus_slice(&after_impact);
        free_focus_slice(&before_focus);
        free_focus_slice(&after_focus);
        free_project(&before_project);
        free_project(&after_project);
        return false;
    }

    int removed_count = 0;
    int added_count = 0;
    int unchanged_count = 0;
    for (int i = 0; i < before_items.count; i++) {
        if (semantic_items_contains(&after_items, before_items.items[i].key)) unchanged_count++;
        else removed_count++;
    }
    for (int i = 0; i < after_items.count; i++) {
        if (!semantic_items_contains(&before_items, after_items.items[i].key)) added_count++;
    }

    char before_display[MAX_PATH_LEN];
    char after_display[MAX_PATH_LEN];
    format_pack_path(before_path, before_display, sizeof(before_display));
    format_pack_path(after_path, after_display, sizeof(after_display));

    fprintf(out, "SDIFFv1\n");
    fprintf(out, "before: %s\n", before_display);
    fprintf(out, "after: %s\n", after_display);
    fprintf(out, "focus: %s\n", focus_symbol);
    fprintf(out, "presence: before=%s after=%s\n",
            before_focus.seed_count > 0 ? "yes" : "no",
            after_focus.seed_count > 0 ? "yes" : "no");
    fprintf(out, "summary: before=%d after=%d added=%d removed=%d unchanged=%d\n",
            before_items.count, after_items.count, added_count, removed_count, unchanged_count);

    if (removed_count == 0 && added_count == 0) {
        fprintf(out, "status: unchanged\n");
    } else {
        if (removed_count > 0) {
            fprintf(out, "removed:\n");
            for (int i = 0; i < before_items.count; i++) {
                if (semantic_items_contains(&after_items, before_items.items[i].key)) continue;
                fprintf(out, "  %s\n", before_items.items[i].text);
            }
        }
        if (added_count > 0) {
            fprintf(out, "added:\n");
            for (int i = 0; i < after_items.count; i++) {
                if (semantic_items_contains(&before_items, after_items.items[i].key)) continue;
                fprintf(out, "  %s\n", after_items.items[i].text);
            }
        }
    }

    if (out != stdout) fclose(out);
    free_semantic_items(&before_items);
    free_semantic_items(&after_items);
    free_focus_slice(&before_impact);
    free_focus_slice(&after_impact);
    free_focus_slice(&before_focus);
    free_focus_slice(&after_focus);
    free_project(&before_project);
    free_project(&after_project);
    return true;
}

bool emit_semantic_index_json(const char* entry_file, const char* out_path) {
    char entry_path[MAX_PATH_LEN];
    ProjectIndex project;
    if (!build_project_index(entry_file, &project, entry_path, sizeof(entry_path))) return false;

    FILE* out = open_index_output(out_path);
    if (!out) {
        free_project(&project);
        return false;
    }

    emit_index_json(out, &project, entry_path);

    if (out != stdout) fclose(out);
    free_project(&project);
    return true;
}

bool emit_context_pack(const char* entry_file, const char* out_path,
                       const char* focus_symbol, bool include_impact) {
    char entry_path[MAX_PATH_LEN];
    ProjectIndex project;
    if (!build_project_index(entry_file, &project, entry_path, sizeof(entry_path))) return false;

    FILE* out = open_index_output(out_path);
    if (!out) {
        free_project(&project);
        return false;
    }

    FocusSlice focus_slice;
    FocusSlice* focus_slice_ptr = NULL;
    FocusSlice impact_slice;
    FocusSlice* impact_slice_ptr = NULL;
    memset(&focus_slice, 0, sizeof(focus_slice));
    memset(&impact_slice, 0, sizeof(impact_slice));
    if (focus_symbol && focus_symbol[0] != '\0') {
        if (!build_focus_slice(&project, focus_symbol, &focus_slice)) {
            if (out != stdout) fclose(out);
            free_project(&project);
            return false;
        }
        focus_slice_ptr = &focus_slice;

        if (include_impact) {
            if (!build_impact_slice(&project, focus_symbol, &impact_slice)) {
                if (out != stdout) fclose(out);
                free_focus_slice(&focus_slice);
                free_project(&project);
                return false;
            }
            impact_slice_ptr = &impact_slice;
        }
    }

    emit_context_pack_text(out, &project, entry_path, focus_slice_ptr, impact_slice_ptr,
                           focus_symbol, include_impact);

    if (out != stdout) fclose(out);
    free_focus_slice(&impact_slice);
    free_focus_slice(&focus_slice);
    free_project(&project);
    return true;
}
