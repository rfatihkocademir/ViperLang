#include "indexer.h"

#include "parser.h"
#include "bytecode.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_PATH_LEN 1024
#define MAX_NAME_LEN 128

typedef struct {
    char name[MAX_NAME_LEN];
    int line;
} IndexVar;

typedef struct {
    char name[MAX_NAME_LEN];
    int arity;
    int line;
} IndexFn;

typedef struct {
    char raw_path[MAX_PATH_LEN];
    char resolved_path[MAX_PATH_LEN];
    int line;
} IndexImport;

typedef struct {
    char name[MAX_NAME_LEN];
    int arg_count;
    int line;
} IndexCall;

typedef struct {
    char name[MAX_NAME_LEN];
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
} ModuleIndex;

typedef struct {
    ModuleIndex* modules;
    int module_count;
    int module_cap;
} ProjectIndex;

static char index_project_root[MAX_PATH_LEN];
static void path_dirname(const char* path, char* out, size_t out_size);

static void copy_text(char* dst, size_t dst_size, const char* src) {
    if (dst_size == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, dst_size, "%s", src);
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

static bool module_add_var(ModuleIndex* module, Token name) {
    if (!ensure_capacity((void**)&module->variables, &module->variable_cap,
                         module->variable_count + 1, sizeof(IndexVar))) {
        return false;
    }

    IndexVar* out = &module->variables[module->variable_count++];
    token_to_text(name, out->name, sizeof(out->name));
    out->line = name.line;
    return true;
}

static bool module_add_fn(ModuleIndex* module, Token name, int arity) {
    if (!ensure_capacity((void**)&module->functions, &module->function_cap,
                         module->function_count + 1, sizeof(IndexFn))) {
        return false;
    }

    IndexFn* out = &module->functions[module->function_count++];
    token_to_text(name, out->name, sizeof(out->name));
    out->arity = arity;
    out->line = name.line;
    return true;
}

static bool module_add_struct(ModuleIndex* module, Token name, Token* fields, int field_count) {
    if (!ensure_capacity((void**)&module->structs, &module->struct_cap,
                         module->struct_count + 1, sizeof(IndexStruct))) {
        return false;
    }

    IndexStruct* out = &module->structs[module->struct_count++];
    memset(out, 0, sizeof(*out));
    token_to_text(name, out->name, sizeof(out->name));
    out->line = name.line;

    if (field_count <= 0) return true;
    if (!ensure_capacity((void**)&out->fields, &out->field_cap, field_count, sizeof(char*))) {
        return false;
    }

    out->field_count = field_count;
    for (int i = 0; i < field_count; i++) {
        char field_name[MAX_NAME_LEN];
        token_to_text(fields[i], field_name, sizeof(field_name));
        out->fields[i] = dup_text(field_name);
        if (!out->fields[i]) return false;
    }

    return true;
}

static bool module_add_import(ModuleIndex* module, const char* raw_path,
                              const char* resolved_path, int line) {
    if (!ensure_capacity((void**)&module->imports, &module->import_cap,
                         module->import_count + 1, sizeof(IndexImport))) {
        return false;
    }

    IndexImport* out = &module->imports[module->import_count++];
    copy_text(out->raw_path, sizeof(out->raw_path), raw_path);
    copy_text(out->resolved_path, sizeof(out->resolved_path), resolved_path);
    out->line = line;
    return true;
}

static bool module_add_call(ModuleIndex* module, Token name, int arg_count) {
    if (!ensure_capacity((void**)&module->calls, &module->call_cap,
                         module->call_count + 1, sizeof(IndexCall))) {
        return false;
    }

    IndexCall* out = &module->calls[module->call_count++];
    token_to_text(name, out->name, sizeof(out->name));
    out->arg_count = arg_count;
    out->line = name.line;
    return true;
}

static void free_project(ProjectIndex* project) {
    if (!project || !project->modules) return;

    for (int i = 0; i < project->module_count; i++) {
        ModuleIndex* m = &project->modules[i];

        for (int j = 0; j < m->struct_count; j++) {
            for (int k = 0; k < m->structs[j].field_count; k++) {
                free(m->structs[j].fields[k]);
            }
            free(m->structs[j].fields);
        }

        free(m->imports);
        free(m->variables);
        free(m->functions);
        free(m->structs);
        free(m->calls);
    }

    free(project->modules);
    memset(project, 0, sizeof(*project));
}

static int find_module_index(const ProjectIndex* project, const char* path) {
    for (int i = 0; i < project->module_count; i++) {
        if (strcmp(project->modules[i].path, path) == 0) return i;
    }
    return -1;
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

static bool collect_expr_calls(AstNode* expr, ModuleIndex* module);
static bool collect_stmt_calls(AstNode* stmt, ModuleIndex* module);

static bool collect_expr_calls(AstNode* expr, ModuleIndex* module) {
    if (!expr) return true;

    switch (expr->type) {
        case AST_CALL_EXPR:
            if (!module_add_call(module, expr->data.call_expr.name, expr->data.call_expr.arg_count)) {
                return false;
            }
            if (!collect_expr_calls(expr->data.call_expr.callee, module)) return false;
            for (int i = 0; i < expr->data.call_expr.arg_count; i++) {
                if (!collect_expr_calls(expr->data.call_expr.args[i], module)) return false;
            }
            return true;

        case AST_BINARY_EXPR:
            return collect_expr_calls(expr->data.binary.left, module) &&
                   collect_expr_calls(expr->data.binary.right, module);

        case AST_ASSIGN_EXPR:
            return collect_expr_calls(expr->data.assign.value, module);

        case AST_GET_EXPR:
            return collect_expr_calls(expr->data.get_expr.obj, module);

        case AST_SET_EXPR:
            return collect_expr_calls(expr->data.set_expr.obj, module) &&
                   collect_expr_calls(expr->data.set_expr.value, module);

        default:
            return true;
    }
}

static bool collect_stmt_calls(AstNode* stmt, ModuleIndex* module) {
    if (!stmt) return true;

    switch (stmt->type) {
        case AST_PROGRAM:
        case AST_BLOCK_STMT:
            for (int i = 0; i < stmt->data.block.count; i++) {
                if (!collect_stmt_calls(stmt->data.block.statements[i], module)) return false;
            }
            return true;

        case AST_VAR_DECL:
            return collect_expr_calls(stmt->data.var_decl.initializer, module);

        case AST_EXPR_STMT:
            return collect_expr_calls(stmt->data.expr_stmt.expr, module);

        case AST_IF_STMT:
            return collect_expr_calls(stmt->data.if_stmt.condition, module) &&
                   collect_stmt_calls(stmt->data.if_stmt.then_branch, module) &&
                   collect_stmt_calls(stmt->data.if_stmt.else_branch, module);

        case AST_WHILE_STMT:
            return collect_expr_calls(stmt->data.while_stmt.condition, module) &&
                   collect_stmt_calls(stmt->data.while_stmt.body, module);

        case AST_RETURN_STMT:
            return collect_expr_calls(stmt->data.return_stmt.value, module);

        case AST_FUNC_DECL:
            return collect_stmt_calls(stmt->data.func_decl.body, module);

        default:
            return true;
    }
}

static bool index_module(ProjectIndex* project, const char* module_path) {
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
                if (!module_add_var(module, stmt->data.var_decl.name)) {
                    free(source);
                    return false;
                }
            } else if (stmt->type == AST_FUNC_DECL) {
                if (!module_add_fn(module, stmt->data.func_decl.name,
                                   stmt->data.func_decl.param_count)) {
                    free(source);
                    return false;
                }
            } else if (stmt->type == AST_STRUCT_DECL) {
                if (!module_add_struct(module, stmt->data.struct_decl.name,
                                       stmt->data.struct_decl.fields,
                                       stmt->data.struct_decl.field_count)) {
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

                if (!module_add_import(module, raw_path, resolved_path, stmt->data.use_stmt.path.line)) {
                    free(source);
                    return false;
                }

                if (!index_module(project, resolved_path)) {
                    free(source);
                    return false;
                }
            }

            module = &project->modules[module_index];
            if (!collect_stmt_calls(stmt, module)) {
                free(source);
                return false;
            }
        }
    } else {
        module = &project->modules[module_index];
        if (!collect_stmt_calls(ast, module)) {
            free(source);
            return false;
        }
    }

    free(source);
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

static void emit_index_json(FILE* out, const ProjectIndex* project, const char* entry_file) {
    int total_vars = 0;
    int total_fns = 0;
    int total_structs = 0;
    int total_calls = 0;
    int total_imports = 0;

    for (int i = 0; i < project->module_count; i++) {
        total_vars += project->modules[i].variable_count;
        total_fns += project->modules[i].function_count;
        total_structs += project->modules[i].struct_count;
        total_calls += project->modules[i].call_count;
        total_imports += project->modules[i].import_count;
    }

    fprintf(out, "{\n");
    fprintf(out, "  \"schema_version\": 1,\n");
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
            fprintf(out, ", \"arity\": %d, \"line\": %d}",
                    m->functions[j].arity, m->functions[j].line);
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
            fprintf(out, ", \"line\": %d, \"fields\": [", m->structs[j].line);
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

        fprintf(out, "      \"calls\": [");
        for (int j = 0; j < m->call_count; j++) {
            if (j == 0) fprintf(out, "\n");
            fprintf(out, "        {\"name\": ");
            json_write_string(out, m->calls[j].name);
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
    fprintf(out, "    \"calls\": %d\n", total_calls);
    fprintf(out, "  }\n");
    fprintf(out, "}\n");
}

bool emit_semantic_index_json(const char* entry_file, const char* out_path) {
    char entry_path[MAX_PATH_LEN];
    if (!resolve_entry_path(entry_file, entry_path, sizeof(entry_path))) {
        fprintf(stderr, "Indexer Error: Could not resolve entry file path.\n");
        return false;
    }

    if (!find_project_root_from_entry(entry_path, index_project_root, sizeof(index_project_root))) {
        fprintf(stderr, "Indexer Error: Could not resolve project root.\n");
        return false;
    }

    ProjectIndex project;
    memset(&project, 0, sizeof(project));

    bool ok = index_module(&project, entry_path);
    if (!ok) {
        free_project(&project);
        return false;
    }

    FILE* out = stdout;
    if (out_path && out_path[0] != '\0') {
        out = fopen(out_path, "w");
        if (!out) {
            fprintf(stderr, "Indexer Error: Could not open output file \"%s\".\n", out_path);
            free_project(&project);
            return false;
        }
    }

    emit_index_json(out, &project, entry_path);

    if (out != stdout) fclose(out);
    free_project(&project);
    return true;
}
