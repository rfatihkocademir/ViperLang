#include "indexer.h"

#include "crypto.h"
#include "parser.h"
#include "bytecode.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
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

static void indexer_error(const char* code, const char* fmt, ...) {
    va_list args;
    fprintf(stderr, "Indexer Error [%s]: ", code);
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

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
    char symbol_id[32];
    char module_ref[32];
    char kind[16];
    char contract[4608];
} StateSymbolRow;

typedef struct {
    StateSymbolRow* items;
    int count;
    int cap;
} StateSymbolRowList;

typedef struct {
    char name[MAX_NAME_LEN];
    bool is_public;
    StringList call_basenames;
    StringList self_effects;
} QueryGraphNode;

typedef struct {
    QueryGraphNode* nodes;
    int* scores;
    int* blast;
    int* dep_effect_counts;
    StringList* dep_effects;
    int count;
} QueryRiskStats;

typedef struct {
    char diff_identity[256];
    char status[16];
    int before_index;
    int after_index;
} SemanticChangePlanItem;

typedef struct {
    char diff_identity[256];
    char module_path[MAX_PATH_LEN];
    char module_symbol_identity[4608];
    char name[MAX_NAME_LEN];
    char status[16];
    int score;
    int blast;
    int dep_effect_count;
} SemanticChangeCandidate;

typedef struct {
    char path[MAX_PATH_LEN];
    char hash_hex[65];
    StringList covered_symbol_ids;
} StateTestLink;

typedef struct {
    StateTestLink* items;
    int count;
    int cap;
} StateTestLinkList;

typedef struct {
    char path[MAX_PATH_LEN];
    int hits;
    int max_score;
    bool has_changed;
    bool has_added;
    bool has_removed;
    bool has_effects;
    bool has_callers;
    StringList symbols;
} StatePlanTestCandidate;

typedef struct {
    StatePlanTestCandidate* items;
    int count;
    int cap;
} StatePlanTestCandidateList;

typedef struct {
    char root[MAX_PATH_LEN];
    char entry[MAX_PATH_LEN];
    char focus[MAX_NAME_LEN];
    bool include_impact;
    int semantic_item_count;
    char semantic_fingerprint[65];
    StringList tracked_paths;
    StringList tracked_hashes;
    StringList tracked_test_paths;
    StringList tracked_test_hashes;
} ProjectStateManifest;

typedef struct {
    bool present;
    bool matches_state;
    char focus[MAX_NAME_LEN];
    bool include_impact;
    int tests_planned;
    int tests_failed;
    char semantic_fingerprint[65];
    StringList executed_tests;
} StateProofSummary;

typedef struct {
    long brief_bytes;
    long full_bytes;
    long saved_bytes;
    long saved_pct;
    long brief_tokens_est;
    long full_tokens_est;
    long saved_tokens_est;
    unsigned long long brief_emit_us;
    unsigned long long full_emit_us;
} StatePayloadMetrics;

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
static bool build_project_index(const char* entry_file, ProjectIndex* project,
                                char* entry_path_out, size_t entry_path_out_size);
static int string_list_index_of(const StringList* list, const char* text);
static bool parse_compact_symbol_row(const char* kind, const char* contract, CompactSymbolRow* row);
static bool collect_symbol_tables_from_rows(const StateSymbolRowList* rows,
                                            StringList* returns, StringList* effects,
                                            StringList* calls, int* symbol_count);
static bool emit_compact_symbol_row_from_state(FILE* out, const StateSymbolRow* state_row,
                                               const StringList* returns, const StringList* effects,
                                               const StringList* calls);
static bool state_symbol_row_diff_identity(const StateSymbolRow* row, char* out, size_t out_size);
static int state_symbol_rows_index_of_diff_identity(const StateSymbolRowList* rows,
                                                    const char* diff_identity);
static bool build_module_symbol_identity(const char* module_path, const char* contract,
                                         char* out, size_t out_size);
static bool collect_project_symbol_identities(const ProjectIndex* project, StringList* out);
static bool collect_candidate_test_files(const char* project_root, StringList* out);
static bool canonicalize_existing_path(const char* path, char* out, size_t out_size);
static bool collect_module_rows_from_semantic_items(const SemanticItemList* items,
                                                    StringList* module_refs, StringList* module_paths);
static bool collect_state_test_links(const char* project_root, const SemanticItemList* items,
                                     StateTestLinkList* out);
static bool emit_state_test_sections(FILE* out, const StateTestLinkList* links);
static bool emit_state_linked_tests(const char* state_path, const StateSymbolRowList* selected_rows,
                                    FILE* out);
static bool emit_state_brief_rows(FILE* out, const StateSymbolRowList* rows);
static bool emit_state_module_section_from_rows(const char* state_path, const StateSymbolRowList* rows,
                                                FILE* out);
static long measure_state_payload_bytes(const char* state_path, const StateSymbolRowList* rows,
                                        bool brief_output, bool include_tests);
static bool compute_state_payload_metrics(const char* state_path, const StateSymbolRowList* rows,
                                          bool include_tests, StatePayloadMetrics* metrics);
static bool emit_state_verification_summary(const char* state_path,
                                            const ProjectStateManifest* manifest,
                                            const StringList* stale_lines,
                                            const StringList* stale_module_refs,
                                            FILE* out);
static bool load_state_semantic_items(const char* state_path, SemanticItemList* items);
static bool emit_state_test_plan(FILE* out, const char* before_state_path, const char* after_state_path,
                                 const SemanticItemList* before_items, const SemanticItemList* after_items,
                                 const StateSymbolRowList* before_rows, const StateSymbolRowList* after_rows);
static bool build_state_test_plan_candidates(const char* before_state_path, const char* after_state_path,
                                             const SemanticItemList* before_items, const SemanticItemList* after_items,
                                             const StateSymbolRowList* before_rows, const StateSymbolRowList* after_rows,
                                             StatePlanTestCandidateList* out);
static bool collect_state_linked_test_candidates(const char* state_path, const StateSymbolRowList* selected_rows,
                                                 StatePlanTestCandidateList* out);
static bool stale_lines_include_tracked_test_change(const ProjectStateManifest* manifest,
                                                    const StringList* stale_lines);
static bool write_state_linked_proof(const char* before_state, const char* after_state,
                                     const char* out_path, int* tests_planned_out,
                                     int* tests_failed_out);

static unsigned long long monotonic_time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (unsigned long long)tv.tv_sec * 1000000ULL + (unsigned long long)tv.tv_usec;
}

static long estimate_payload_tokens(long bytes) {
    if (bytes <= 0) return 0;
    return (bytes + 3) / 4;
}

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

static bool string_list_add_param(StringList* list, Token name, Token type_annot) {
    if (!list || name.length <= 0) return false;
    if (type_annot.length <= 0) return string_list_add_token(list, name);

    char text[MAX_NAME_LEN * 2];
    int written = snprintf(text, sizeof(text), "%.*s:%.*s",
                           name.length, name.start,
                           type_annot.length, type_annot.start);
    if (written < 0 || (size_t)written >= sizeof(text)) return false;
    return string_list_add_unique(list, text);
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
    free_string_list(&manifest->tracked_test_paths);
    free_string_list(&manifest->tracked_test_hashes);
    memset(manifest, 0, sizeof(*manifest));
}

static void free_compact_symbol_row(CompactSymbolRow* row) {
    if (!row) return;
    free_string_list(&row->declared_effects);
    free_string_list(&row->inferred_effects);
    free_string_list(&row->calls);
    memset(row, 0, sizeof(*row));
}

static void free_state_symbol_rows(StateSymbolRowList* list) {
    if (!list) return;
    free(list->items);
    memset(list, 0, sizeof(*list));
}

static void free_state_test_links(StateTestLinkList* list) {
    if (!list) return;
    for (int i = 0; i < list->count; i++) {
        free_string_list(&list->items[i].covered_symbol_ids);
    }
    free(list->items);
    memset(list, 0, sizeof(*list));
}

static void free_state_proof_summary(StateProofSummary* summary) {
    if (!summary) return;
    free_string_list(&summary->executed_tests);
    memset(summary, 0, sizeof(*summary));
}

static void free_state_plan_test_candidates(StatePlanTestCandidateList* list) {
    if (!list) return;
    for (int i = 0; i < list->count; i++) {
        free_string_list(&list->items[i].symbols);
    }
    free(list->items);
    memset(list, 0, sizeof(*list));
}

static void free_query_graph_nodes(QueryGraphNode* nodes, int count) {
    if (!nodes) return;
    for (int i = 0; i < count; i++) {
        free_string_list(&nodes[i].call_basenames);
        free_string_list(&nodes[i].self_effects);
    }
    free(nodes);
}

static void free_query_risk_stats(QueryRiskStats* stats) {
    if (!stats) return;
    if (stats->dep_effects) {
        for (int i = 0; i < stats->count; i++) {
            free_string_list(&stats->dep_effects[i]);
        }
    }
    free(stats->dep_effects);
    free(stats->scores);
    free(stats->blast);
    free(stats->dep_effect_counts);
    free_query_graph_nodes(stats->nodes, stats->count);
    memset(stats, 0, sizeof(*stats));
}

static bool string_list_append_unique(StringList* list, const char* text) {
    if (!list || !text || text[0] == '\0') return false;
    if (string_list_index_of(list, text) != -1) return true;
    return string_list_append(list, text);
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

static bool state_symbol_rows_add(StateSymbolRowList* list, const char* symbol_id,
                                  const char* module_ref, const char* kind, const char* contract) {
    if (!list || !symbol_id || !module_ref || !kind || !contract) return false;
    if (!ensure_capacity((void**)&list->items, &list->cap, list->count + 1, sizeof(StateSymbolRow))) {
        return false;
    }
    StateSymbolRow* row = &list->items[list->count++];
    memset(row, 0, sizeof(*row));
    copy_text(row->symbol_id, sizeof(row->symbol_id), symbol_id);
    copy_text(row->module_ref, sizeof(row->module_ref), module_ref);
    copy_text(row->kind, sizeof(row->kind), kind);
    copy_text(row->contract, sizeof(row->contract), contract);
    return row->symbol_id[0] != '\0' && row->module_ref[0] != '\0' &&
           row->kind[0] != '\0' && row->contract[0] != '\0';
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

static bool resolve_std_module_path(const char* raw_import_path, char* out, size_t out_size) {
    if (strncmp(raw_import_path, "@std/", 5) != 0) return false;

    const char* subpath = raw_import_path + 5;
    char filename[MAX_PATH_LEN];
    int written = snprintf(filename, sizeof(filename), "%s.vp", subpath);
    if (written < 0 || (size_t)written >= sizeof(filename)) return false;

    const char* env_std = getenv("VIPER_STD_PATH");
    if (env_std && env_std[0] != '\0' &&
        join_path(env_std, filename, out, out_size) &&
        module_source_exists(out)) {
        return true;
    }

    char dev_std_dir[MAX_PATH_LEN];
    if (join_path(index_project_root, "lib/std", dev_std_dir, sizeof(dev_std_dir)) &&
        join_path(dev_std_dir, filename, out, out_size) &&
        module_source_exists(out)) {
        return true;
    }

    char sys_path[MAX_PATH_LEN];
    if (join_path("/usr/local/lib/viper/std", filename, sys_path, sizeof(sys_path)) &&
        module_source_exists(sys_path)) {
        copy_text(out, out_size, sys_path);
        return true;
    }

    return false;
}

static bool resolve_module_path(const char* current_module_path, const char* raw_import_path,
                                char* out, size_t out_size) {
    if (!raw_import_path || raw_import_path[0] == '\0') return false;

    if (strncmp(raw_import_path, "@std/", 5) == 0) {
        return resolve_std_module_path(raw_import_path, out, out_size);
    }

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
        indexer_error("VIX001", "Could not open file \"%s\".", path);
        return NULL;
    }

    if (fseek(file, 0L, SEEK_END) != 0) {
        fclose(file);
        indexer_error("VIX002", "Could not seek file \"%s\".", path);
        return NULL;
    }

    long size = ftell(file);
    if (size < 0) {
        fclose(file);
        indexer_error("VIX003", "Could not get file size \"%s\".", path);
        return NULL;
    }

    rewind(file);

    char* buffer = (char*)malloc((size_t)size + 1);
    if (!buffer) {
        fclose(file);
        indexer_error("VIX004", "Not enough memory for \"%s\".", path);
        return NULL;
    }

    size_t bytes_read = fread(buffer, sizeof(char), (size_t)size, file);
    fclose(file);

    if (bytes_read < (size_t)size) {
        free(buffer);
        indexer_error("VIX005", "Could not read file \"%s\".", path);
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
        Token type_annot = {0};
        if (fn_decl->data.func_decl.param_types) {
            type_annot = fn_decl->data.func_decl.param_types[i];
        }
        if (!string_list_add_param(&out->params, fn_decl->data.func_decl.params[i], type_annot)) return NULL;
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
        indexer_error("VIX006", "Out of memory while adding module.");
        return false;
    }
    int module_index = project->module_count - 1;

    AstNode* ast = parse(source);
    if (!ast) {
        free(source);
        indexer_error("VIX007", "Parse failed for \"%s\".", module_path);
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
                    indexer_error("VIX008", "Could not resolve import in \"%s\".", module_path);
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
        indexer_error("VIX009", "Out of memory while building focus slice.");
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
        indexer_error("VIX010", "Focus symbol \"%s\" not found.", focus_symbol);
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
        indexer_error("VIX011", "Out of memory while building impact slice.");
        return false;
    }

    FocusSlice active_targets;
    FocusSlice seed_targets;
    memset(&active_targets, 0, sizeof(active_targets));
    memset(&seed_targets, 0, sizeof(seed_targets));

    if (!init_focus_slice(&active_targets, project) || !init_focus_slice(&seed_targets, project)) {
        indexer_error("VIX011", "Out of memory while building impact slice.");
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

static bool build_state_proof_path(const char* state_path, char* out, size_t out_size) {
    if (!state_path || !out || out_size == 0) return false;
    return snprintf(out, out_size, "%s.vproof", state_path) < (int)out_size;
}

static bool load_project_state_manifest(const char* state_path, ProjectStateManifest* manifest) {
    if (!state_path || !manifest) return false;
    memset(manifest, 0, sizeof(*manifest));

    FILE* file = fopen(state_path, "r");
    if (!file) {
        indexer_error("VIX012", "Could not open state file \"%s\".", state_path);
        return false;
    }

    char line[4096];
    if (!fgets(line, sizeof(line), file)) {
        fclose(file);
        indexer_error("VIX013", "Empty state file \"%s\".", state_path);
        return false;
    }
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
    if (strcmp(line, "PSTATEv1") != 0) {
        fclose(file);
        indexer_error("VIX014", "Unsupported state file format \"%s\".", state_path);
        return false;
    }

    bool in_tracked_files = false;
    bool in_tracked_tests = false;
    while (fgets(line, sizeof(line), file)) {
        n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';

        if (strcmp(line, "context:") == 0) break;
        if (strcmp(line, "tracked_files:") == 0) {
            in_tracked_files = true;
            in_tracked_tests = false;
            continue;
        }
        if (strcmp(line, "tracked_tests:") == 0) {
            in_tracked_files = false;
            in_tracked_tests = true;
            continue;
        }

        if (in_tracked_files && strncmp(line, "  ", 2) == 0) {
            char* marker = strstr(line + 2, " sha256=");
            if (!marker) {
                fclose(file);
                free_project_state_manifest(manifest);
                indexer_error("VIX015", "Malformed tracked file entry in \"%s\".", state_path);
                return false;
            }
            *marker = '\0';
            const char* rel_path = line + 2;
            const char* hash = marker + 8;
            if (!string_list_append(&manifest->tracked_paths, rel_path) ||
                !string_list_append(&manifest->tracked_hashes, hash)) {
                fclose(file);
                free_project_state_manifest(manifest);
                indexer_error("VIX016", "Out of memory while reading state file.");
                return false;
            }
            continue;
        }

        if (in_tracked_tests && strncmp(line, "  ", 2) == 0) {
            char* marker = strstr(line + 2, " sha256=");
            if (!marker) {
                fclose(file);
                free_project_state_manifest(manifest);
                indexer_error("VIX017", "Malformed tracked test entry in \"%s\".", state_path);
                return false;
            }
            *marker = '\0';
            const char* rel_path = line + 2;
            const char* hash = marker + 8;
            if (!string_list_append(&manifest->tracked_test_paths, rel_path) ||
                !string_list_append(&manifest->tracked_test_hashes, hash)) {
                fclose(file);
                free_project_state_manifest(manifest);
                indexer_error("VIX016", "Out of memory while reading state file.");
                return false;
            }
            continue;
        }

        in_tracked_files = false;
        in_tracked_tests = false;
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
        indexer_error("VIX018", "Incomplete state file \"%s\".", state_path);
        return false;
    }
    if (manifest->tracked_paths.count != manifest->tracked_hashes.count) {
        free_project_state_manifest(manifest);
        indexer_error("VIX019", "Corrupt tracked file list in \"%s\".", state_path);
        return false;
    }
    if (manifest->tracked_test_paths.count != manifest->tracked_test_hashes.count) {
        free_project_state_manifest(manifest);
        indexer_error("VIX020", "Corrupt tracked test list in \"%s\".", state_path);
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

    for (int i = 0; i < manifest->tracked_test_paths.count; i++) {
        char test_path[MAX_PATH_LEN];
        if (!resolve_state_path(root_path, manifest->tracked_test_paths.items[i], test_path, sizeof(test_path))) {
            return false;
        }

        char actual_hash[65];
        bool ok = compute_module_hash_hex(test_path, actual_hash);
        if (ok && strcmp(actual_hash, manifest->tracked_test_hashes.items[i]) == 0) continue;

        (*stale_count)++;
        if (stale_lines) {
            char line[2048];
            if (!ok) {
                if (snprintf(line, sizeof(line), "%s expected=%s actual=missing",
                             manifest->tracked_test_paths.items[i], manifest->tracked_test_hashes.items[i]) >=
                    (int)sizeof(line)) {
                    return false;
                }
            } else {
                if (snprintf(line, sizeof(line), "%s expected=%s actual=%s",
                             manifest->tracked_test_paths.items[i], manifest->tracked_test_hashes.items[i],
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

static bool build_symbol_identity_source(const char* kind, const char* module_path,
                                         const char* contract, char* out, size_t out_size) {
    if (!kind || !module_path || !contract || !out || out_size == 0) return false;

    if (strncmp(contract, "var ", 4) == 0) {
        const char* start = contract + 4;
        const char* end = strchr(start, ':');
        size_t len = end ? (size_t)(end - start) : strlen(start);
        char name[MAX_NAME_LEN];
        if (len == 0 || len >= sizeof(name)) return false;
        memcpy(name, start, len);
        name[len] = '\0';
        return snprintf(out, out_size, "%s:var:%s::%s", kind, module_path, name) < (int)out_size;
    }

    if (strncmp(contract, "struct ", 7) == 0) {
        const char* start = contract + 7;
        if (strncmp(start, "pub ", 4) == 0) start += 4;
        const char* end = strchr(start, '(');
        size_t len = end ? (size_t)(end - start) : strlen(start);
        char name[MAX_NAME_LEN];
        if (len == 0 || len >= sizeof(name)) return false;
        memcpy(name, start, len);
        name[len] = '\0';
        return snprintf(out, out_size, "%s:struct:%s::%s", kind, module_path, name) < (int)out_size;
    }

    CompactSymbolRow row;
    if (!parse_compact_symbol_row(kind, contract, &row)) return false;
    bool ok = row.parsed &&
              snprintf(out, out_size, "%s:fn:%s::%s/%d", kind, module_path, row.name, row.arity) < (int)out_size;
    free_compact_symbol_row(&row);
    return ok;
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

static bool parse_test_index_row(const char* line, char* path, size_t path_size,
                                 StringList* covered_symbol_ids) {
    if (!line || !path || path_size == 0 || !covered_symbol_ids) return false;
    while (*line == ' ' || *line == '\t') line++;
    const char* marker = strstr(line, " covers=");
    if (!marker) return false;

    size_t path_len = (size_t)(marker - line);
    if (path_len == 0 || path_len >= path_size) return false;
    memcpy(path, line, path_len);
    path[path_len] = '\0';
    return parse_csv_field(marker + 8, covered_symbol_ids);
}

static bool parse_state_exec_result_row(const char* line, char* path, size_t path_size,
                                        char* status, size_t status_size) {
    if (!line || !path || !status || path_size == 0 || status_size == 0) return false;
    while (*line == ' ' || *line == '\t') line++;
    const char* test_marker = strstr(line, "|test=");
    const char* status_marker = strstr(line, "|status=");
    if (!test_marker || !status_marker || status_marker <= test_marker + 6) return false;

    const char* path_start = test_marker + 6;
    size_t path_len = (size_t)(status_marker - path_start);
    if (path_len == 0 || path_len >= path_size) return false;
    memcpy(path, path_start, path_len);
    path[path_len] = '\0';

    const char* status_start = status_marker + 8;
    const char* status_end = strchr(status_start, '|');
    size_t status_len = status_end ? (size_t)(status_end - status_start) : strlen(status_start);
    if (status_len == 0 || status_len >= status_size) return false;
    memcpy(status, status_start, status_len);
    status[status_len] = '\0';
    return true;
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

static bool collect_stale_symbol_ids(const char* state_path, const StringList* module_refs,
                                     StringList* symbol_ids) {
    if (!state_path || !module_refs || !symbol_ids) return false;
    memset(symbol_ids, 0, sizeof(*symbol_ids));
    if (module_refs->count == 0) return true;

    FILE* file = fopen(state_path, "r");
    if (!file) return false;

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
        if (!string_list_add_unique(symbol_ids, symbol_id)) {
            ok = false;
            break;
        }
    }

    fclose(file);
    if (!ok || !in_index) {
        free_string_list(symbol_ids);
        return false;
    }
    return true;
}

static bool emit_string_list_section(FILE* out, const char* header, const StringList* items) {
    if (!out || !header || !items) return false;
    if (items->count == 0) return true;

    fprintf(out, "%s\n", header);
    for (int i = 0; i < items->count; i++) {
        fprintf(out, "  %s\n", items->items[i]);
    }
    return true;
}

static bool emit_stale_symbol_section(const char* state_path, FILE* out, const StringList* module_refs) {
    if (!state_path || !out || !module_refs) return false;
    if (module_refs->count == 0) return true;

    StringList symbol_ids;
    if (!collect_stale_symbol_ids(state_path, module_refs, &symbol_ids)) return false;
    fprintf(out, "stale_symbols:\n");
    for (int i = 0; i < symbol_ids.count; i++) {
        fprintf(out, "  %s\n", symbol_ids.items[i]);
    }
    free_string_list(&symbol_ids);
    return true;
}

static bool module_ref_selected(const char* module_ref, const StringList* module_refs) {
    if (!module_ref) return false;
    if (!module_refs || module_refs->count == 0) return true;
    return string_list_index_of(module_refs, module_ref) != -1;
}

static int state_symbol_rows_index_of(const StateSymbolRowList* rows, const char* symbol_id) {
    if (!rows || !symbol_id) return -1;
    for (int i = 0; i < rows->count; i++) {
        if (strcmp(rows->items[i].symbol_id, symbol_id) == 0) return i;
    }
    return -1;
}

static bool state_symbol_row_equals(const StateSymbolRow* lhs, const StateSymbolRow* rhs) {
    if (!lhs || !rhs) return false;
    return strcmp(lhs->symbol_id, rhs->symbol_id) == 0 &&
           strcmp(lhs->module_ref, rhs->module_ref) == 0 &&
           strcmp(lhs->kind, rhs->kind) == 0 &&
           strcmp(lhs->contract, rhs->contract) == 0;
}

static bool state_symbol_row_diff_equals(const StateSymbolRow* lhs, const StateSymbolRow* rhs) {
    if (!lhs || !rhs) return false;
    return strcmp(lhs->kind, rhs->kind) == 0 &&
           strcmp(lhs->contract, rhs->contract) == 0;
}

static bool load_state_symbol_rows_filtered(const char* state_path, const StringList* module_refs,
                                            StateSymbolRowList* rows) {
    if (!state_path || !rows) return false;
    memset(rows, 0, sizeof(*rows));

    FILE* file = fopen(state_path, "r");
    if (!file) return false;

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
        if (!state_symbol_rows_add(rows, symbol_id, module_ref, kind, contract)) {
            ok = false;
            break;
        }
    }

    fclose(file);
    if (!ok || !in_index) {
        free_state_symbol_rows(rows);
        return false;
    }
    return true;
}

static bool load_state_semantic_items(const char* state_path, SemanticItemList* items) {
    if (!state_path || !items) return false;
    memset(items, 0, sizeof(*items));

    FILE* file = fopen(state_path, "r");
    if (!file) return false;

    char line[8192];
    bool in_items = false;
    bool ok = true;
    while (fgets(line, sizeof(line), file)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';

        if (!in_items) {
            if (strcmp(line, "summary_items:") == 0) in_items = true;
            continue;
        }
        if (strcmp(line, "tracked_tests:") == 0 || strcmp(line, "context:") == 0) break;
        if (strncmp(line, "  ", 2) != 0 || line[2] == '\0') continue;
        char kind[16];
        char module_path[MAX_PATH_LEN];
        char contract[4608];
        char key[4700];
        if (!parse_semantic_item_text(line + 2, kind, sizeof(kind),
                                      module_path, sizeof(module_path),
                                      contract, sizeof(contract))) {
            ok = false;
            break;
        }
        const char* item_type = strncmp(contract, "var ", 4) == 0 ? "var"
                                : (strncmp(contract, "struct ", 7) == 0 ? "struct" : "fn");
        if (snprintf(key, sizeof(key), "%s:%s:%s", kind, item_type, contract) >= (int)sizeof(key) ||
            !semantic_items_add(items, key, line + 2)) {
            ok = false;
            break;
        }
    }

    fclose(file);
    if (!ok || !in_items) {
        free_semantic_items(items);
        return false;
    }
    return true;
}

static bool load_state_proof_summary(const char* state_path, StateProofSummary* summary) {
    if (!state_path || !summary) return false;
    memset(summary, 0, sizeof(*summary));

    char proof_path[MAX_PATH_LEN];
    if (!build_state_proof_path(state_path, proof_path, sizeof(proof_path))) return false;

    FILE* file = fopen(proof_path, "r");
    if (!file) return true;
    summary->present = true;

    char line[8192];
    if (!fgets(line, sizeof(line), file)) {
        fclose(file);
        return false;
    }
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
    if (strcmp(line, "PSTATEEXECv1") != 0 && strcmp(line, "PSTATEEXECv2") != 0) {
        fclose(file);
        return false;
    }

    bool in_results = false;
    bool ok = true;
    while (fgets(line, sizeof(line), file)) {
        n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';

        if (strcmp(line, "results:") == 0) {
            in_results = true;
            continue;
        }
        if (in_results && strncmp(line, "  ", 2) == 0) {
            char test_path[MAX_PATH_LEN];
            char status[16];
            char entry[MAX_PATH_LEN + 32];
            if (!parse_state_exec_result_row(line, test_path, sizeof(test_path),
                                             status, sizeof(status)) ||
                snprintf(entry, sizeof(entry), "%s status=%s", test_path, status) >= (int)sizeof(entry) ||
                !string_list_append(&summary->executed_tests, entry)) {
                ok = false;
                break;
            }
            continue;
        }
        in_results = false;

        if (strncmp(line, "focus: ", 7) == 0) {
            if (strcmp(line + 7, "-") != 0) copy_text(summary->focus, sizeof(summary->focus), line + 7);
        } else if (strncmp(line, "impact: ", 8) == 0) {
            summary->include_impact = strcmp(line + 8, "yes") == 0;
        } else if (strncmp(line, "semantic_fingerprint: ", 22) == 0) {
            copy_text(summary->semantic_fingerprint, sizeof(summary->semantic_fingerprint), line + 22);
        } else if (strncmp(line, "tests_planned: ", 15) == 0) {
            summary->tests_planned = atoi(line + 15);
        } else if (strncmp(line, "tests_failed: ", 14) == 0) {
            summary->tests_failed = atoi(line + 14);
        }
    }

    fclose(file);
    if (!ok) {
        free_state_proof_summary(summary);
        return false;
    }
    return true;
}

static bool parse_proof_executed_test_path(const char* entry, char* path, size_t path_size) {
    if (!entry || !path || path_size == 0) return false;
    const char* marker = strstr(entry, " status=");
    size_t len = marker ? (size_t)(marker - entry) : strlen(entry);
    if (len == 0 || len >= path_size) return false;
    memcpy(path, entry, len);
    path[len] = '\0';
    return true;
}

static bool collect_state_verification_stale_tests(const char* state_path,
                                                   const ProjectStateManifest* manifest,
                                                   const StateProofSummary* summary,
                                                   const StringList* stale_lines,
                                                   const StringList* stale_symbol_ids,
                                                   StringList* out_tests) {
    if (!state_path || !manifest || !summary || !out_tests) return false;
    memset(out_tests, 0, sizeof(*out_tests));

    StringList executed_paths;
    memset(&executed_paths, 0, sizeof(executed_paths));
    for (int i = 0; i < summary->executed_tests.count; i++) {
        char test_path[MAX_PATH_LEN];
        if (!parse_proof_executed_test_path(summary->executed_tests.items[i],
                                            test_path, sizeof(test_path)) ||
            !string_list_add_unique(&executed_paths, test_path)) {
            free_string_list(&executed_paths);
            free_string_list(out_tests);
            return false;
        }
    }

    if (stale_lines) {
        for (int i = 0; i < stale_lines->count; i++) {
            char stale_path[MAX_PATH_LEN];
            if (!parse_stale_line_path(stale_lines->items[i], stale_path, sizeof(stale_path))) {
                free_string_list(&executed_paths);
                free_string_list(out_tests);
                return false;
            }
            if (string_list_index_of(&manifest->tracked_test_paths, stale_path) == -1 ||
                string_list_index_of(&executed_paths, stale_path) == -1) {
                continue;
            }
            if (!string_list_add_unique(out_tests, stale_path)) {
                free_string_list(&executed_paths);
                free_string_list(out_tests);
                return false;
            }
        }
    }

    if (stale_symbol_ids && stale_symbol_ids->count > 0 && summary->executed_tests.count > 0) {
        FILE* file = fopen(state_path, "r");
        if (!file) {
            free_string_list(&executed_paths);
            free_string_list(out_tests);
            return false;
        }

        char line[8192];
        bool in_tests = false;
        bool ok = true;
        while (fgets(line, sizeof(line), file)) {
            size_t n = strlen(line);
            while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';

            if (!in_tests) {
                if (strcmp(line, "test_index:") == 0) in_tests = true;
                continue;
            }
            if (strcmp(line, "context:") == 0) break;
            if (line[0] == '\0') continue;

            char path[MAX_PATH_LEN];
            StringList covered_ids;
            memset(&covered_ids, 0, sizeof(covered_ids));
            if (!parse_test_index_row(line, path, sizeof(path), &covered_ids)) {
                free_string_list(&covered_ids);
                ok = false;
                break;
            }
            if (string_list_index_of(&executed_paths, path) == -1) {
                free_string_list(&covered_ids);
                continue;
            }

            bool covered_stale = false;
            for (int i = 0; i < covered_ids.count; i++) {
                if (string_list_index_of(stale_symbol_ids, covered_ids.items[i]) != -1) {
                    covered_stale = true;
                    break;
                }
            }
            free_string_list(&covered_ids);
            if (!covered_stale) continue;
            if (!string_list_add_unique(out_tests, path)) {
                ok = false;
                break;
            }
        }
        fclose(file);
        if (!ok || !in_tests) {
            free_string_list(&executed_paths);
            free_string_list(out_tests);
            return false;
        }
    }

    free_string_list(&executed_paths);
    return true;
}

static bool emit_state_verification_stale(const char* state_path,
                                          const ProjectStateManifest* manifest,
                                          const StateProofSummary* summary,
                                          const StringList* stale_lines,
                                          const StringList* stale_module_refs,
                                          FILE* out) {
    if (!state_path || !manifest || !summary || !out) return false;
    if (!summary->present || !summary->matches_state || summary->tests_planned == 0 ||
        !stale_lines || stale_lines->count == 0) {
        return true;
    }

    StringList stale_symbol_ids;
    StringList stale_tests;
    memset(&stale_symbol_ids, 0, sizeof(stale_symbol_ids));
    memset(&stale_tests, 0, sizeof(stale_tests));

    bool ok = true;
    if (stale_module_refs && stale_module_refs->count > 0 &&
        !collect_stale_symbol_ids(state_path, stale_module_refs, &stale_symbol_ids)) {
        ok = false;
    }
    if (ok && !collect_state_verification_stale_tests(state_path, manifest, summary,
                                                      stale_lines, &stale_symbol_ids,
                                                      &stale_tests)) {
        ok = false;
    }
    if (!ok) {
        free_string_list(&stale_symbol_ids);
        free_string_list(&stale_tests);
        return false;
    }
    if (stale_symbol_ids.count == 0 && stale_tests.count == 0) {
        free_string_list(&stale_symbol_ids);
        free_string_list(&stale_tests);
        return true;
    }

    fprintf(out, "verification_stale: yes\n");
    if (!emit_string_list_section(out, "verification_stale_symbols:", &stale_symbol_ids) ||
        !emit_string_list_section(out, "verification_stale_tests:", &stale_tests)) {
        free_string_list(&stale_symbol_ids);
        free_string_list(&stale_tests);
        return false;
    }

    free_string_list(&stale_symbol_ids);
    free_string_list(&stale_tests);
    return true;
}

static bool emit_state_verification_summary(const char* state_path,
                                            const ProjectStateManifest* manifest,
                                            const StringList* stale_lines,
                                            const StringList* stale_module_refs,
                                            FILE* out) {
    if (!state_path || !manifest || !out) return false;
    StateProofSummary summary;
    if (!load_state_proof_summary(state_path, &summary)) return false;
    if (!summary.present) {
        free_state_proof_summary(&summary);
        return true;
    }

    summary.matches_state =
        summary.semantic_fingerprint[0] != '\0' &&
        strcmp(summary.semantic_fingerprint, manifest->semantic_fingerprint) == 0 &&
        strcmp(summary.focus, manifest->focus) == 0 &&
        summary.include_impact == manifest->include_impact;

    fprintf(out, "verified: %s\n", summary.matches_state ? (summary.tests_failed == 0 ? "yes" : "fail") : "mismatch");
    fprintf(out, "verified_tests: %d\n", summary.tests_planned);
    fprintf(out, "verified_failed: %d\n", summary.tests_failed);
    if (summary.matches_state && summary.executed_tests.count > 0) {
        fprintf(out, "verification:\n");
        for (int i = 0; i < summary.executed_tests.count; i++) {
            fprintf(out, "  %d|%s\n", i + 1, summary.executed_tests.items[i]);
        }
    }
    if (!emit_state_verification_stale(state_path, manifest, &summary,
                                       stale_lines, stale_module_refs, out)) {
        free_state_proof_summary(&summary);
        return false;
    }

    free_state_proof_summary(&summary);
    return true;
}

static bool load_state_module_rows_filtered(const char* state_path, const StringList* module_refs,
                                            StringList* out_refs, StringList* out_paths) {
    if (!state_path || !out_refs || !out_paths) return false;
    memset(out_refs, 0, sizeof(*out_refs));
    memset(out_paths, 0, sizeof(*out_paths));

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
        if (!module_ref_selected(module_ref, module_refs)) continue;
        if (!string_list_append(out_refs, module_ref) || !string_list_append(out_paths, module_path)) {
            ok = false;
            break;
        }
    }

    fclose(file);
    if (!ok || !in_modules) {
        free_string_list(out_refs);
        free_string_list(out_paths);
        return false;
    }
    return true;
}

static bool state_symbol_row_name(const StateSymbolRow* row, char* out, size_t out_size) {
    if (!row || !out || out_size == 0) return false;
    out[0] = '\0';

    if (strncmp(row->contract, "var ", 4) == 0) {
        const char* start = row->contract + 4;
        const char* end = strchr(start, ':');
        size_t len = end ? (size_t)(end - start) : strlen(start);
        if (len == 0 || len >= out_size) return false;
        memcpy(out, start, len);
        out[len] = '\0';
        return true;
    }

    if (strncmp(row->contract, "struct ", 7) == 0) {
        const char* start = row->contract + 7;
        if (strncmp(start, "pub ", 4) == 0) start += 4;
        const char* end = strchr(start, '(');
        size_t len = end ? (size_t)(end - start) : strlen(start);
        if (len == 0 || len >= out_size) return false;
        memcpy(out, start, len);
        out[len] = '\0';
        return true;
    }

    CompactSymbolRow parsed;
    if (!parse_compact_symbol_row(row->kind[0] == 'T' ? "target" : "caller", row->contract, &parsed)) {
        return false;
    }
    copy_text(out, out_size, parsed.name);
    free_compact_symbol_row(&parsed);
    return out[0] != '\0';
}

static bool state_symbol_row_calls_selected(const StateSymbolRow* row, const StringList* active_names) {
    if (!row || !active_names || active_names->count == 0) return false;

    CompactSymbolRow parsed;
    if (!parse_compact_symbol_row(row->kind[0] == 'T' ? "target" : "caller", row->contract, &parsed)) {
        return false;
    }

    bool matched = false;
    for (int i = 0; i < parsed.calls.count && !matched; i++) {
        char call_name[MAX_NAME_LEN];
        copy_text(call_name, sizeof(call_name), parsed.calls.items[i]);
        char* slash = strchr(call_name, '/');
        if (slash) *slash = '\0';
        char* dot = strrchr(call_name, '.');
        const char* base = dot ? dot + 1 : call_name;
        for (int j = 0; j < active_names->count; j++) {
            if (strcmp(base, active_names->items[j]) == 0) {
                matched = true;
                break;
            }
        }
    }

    free_compact_symbol_row(&parsed);
    return matched;
}

static bool state_symbol_row_has_effect(const StateSymbolRow* row, const char* effect_name) {
    if (!row || !effect_name || effect_name[0] == '\0') return false;

    CompactSymbolRow parsed;
    if (!parse_compact_symbol_row(row->kind[0] == 'T' ? "target" : "caller", row->contract, &parsed)) {
        return false;
    }

    bool matched = false;
    for (int i = 0; i < parsed.declared_effects.count && !matched; i++) {
        matched = strcmp(parsed.declared_effects.items[i], effect_name) == 0;
    }
    for (int i = 0; i < parsed.inferred_effects.count && !matched; i++) {
        matched = strcmp(parsed.inferred_effects.items[i], effect_name) == 0;
    }

    free_compact_symbol_row(&parsed);
    return matched;
}

static bool state_symbol_row_calls_filter(const StateSymbolRow* row, const char* call_filter) {
    if (!row || !call_filter || call_filter[0] == '\0') return false;

    CompactSymbolRow parsed;
    if (!parse_compact_symbol_row(row->kind[0] == 'T' ? "target" : "caller", row->contract, &parsed)) {
        return false;
    }

    bool matched = false;
    for (int i = 0; i < parsed.calls.count && !matched; i++) {
        char call_name[MAX_NAME_LEN * 2];
        copy_text(call_name, sizeof(call_name), parsed.calls.items[i]);
        char* slash = strchr(call_name, '/');
        if (slash) *slash = '\0';
        if (strcmp(call_name, call_filter) == 0) {
            matched = true;
            break;
        }
        char* dot = strrchr(call_name, '.');
        if (dot && strcmp(dot + 1, call_filter) == 0) {
            matched = true;
        }
    }

    free_compact_symbol_row(&parsed);
    return matched;
}

static bool state_symbol_row_matches_query(const StateSymbolRow* row,
                                           const char* name_filter,
                                           const char* effect_filter,
                                           const char* call_filter) {
    if (!row) return false;

    char row_name[MAX_NAME_LEN];
    bool match = true;
    if (name_filter && name_filter[0] != '\0') {
        row_name[0] = '\0';
        match = strcmp(row->symbol_id, name_filter) == 0;
        if (!match && state_symbol_row_name(row, row_name, sizeof(row_name))) {
            match = strcmp(row_name, name_filter) == 0;
        }
    }
    if (match && effect_filter && effect_filter[0] != '\0') {
        match = state_symbol_row_has_effect(row, effect_filter);
    }
    if (match && call_filter && call_filter[0] != '\0') {
        match = state_symbol_row_calls_filter(row, call_filter);
    }
    return match;
}

static bool state_symbol_row_collect_call_basenames(const StateSymbolRow* row, StringList* out) {
    if (!row || !out) return false;

    CompactSymbolRow parsed;
    if (!parse_compact_symbol_row(row->kind[0] == 'T' ? "target" : "caller", row->contract, &parsed)) {
        return false;
    }

    for (int i = 0; i < parsed.calls.count; i++) {
        char call_name[MAX_NAME_LEN * 2];
        copy_text(call_name, sizeof(call_name), parsed.calls.items[i]);
        char* slash = strchr(call_name, '/');
        if (slash) *slash = '\0';
        char* dot = strrchr(call_name, '.');
        const char* base = dot ? dot + 1 : call_name;
        if (!string_list_add_unique(out, base)) {
            free_compact_symbol_row(&parsed);
            return false;
        }
    }

    free_compact_symbol_row(&parsed);
    return true;
}

static bool select_query_symbol_rows(const StateSymbolRowList* all_rows,
                                     const char* name_filter, const char* effect_filter,
                                     const char* call_filter, bool include_impact,
                                     bool include_dependencies, StateSymbolRowList* selected_rows,
                                     int* seed_count_out) {
    if (!all_rows || !selected_rows) return false;
    memset(selected_rows, 0, sizeof(*selected_rows));
    if (seed_count_out) *seed_count_out = 0;

    unsigned char* selected = NULL;
    if (all_rows->count > 0) {
        selected = (unsigned char*)calloc((size_t)all_rows->count, sizeof(unsigned char));
        if (!selected) return false;
    }

    for (int i = 0; i < all_rows->count; i++) {
        if (!state_symbol_row_matches_query(&all_rows->items[i], name_filter, effect_filter, call_filter)) {
            continue;
        }

        selected[i] = 1;
        if (seed_count_out) (*seed_count_out)++;
    }

    if (include_impact || include_dependencies) {
        bool changed = true;
        while (changed) {
            StringList selected_names;
            StringList dependency_names;
            memset(&selected_names, 0, sizeof(selected_names));
            memset(&dependency_names, 0, sizeof(dependency_names));
            changed = false;

            for (int i = 0; i < all_rows->count; i++) {
                char row_name[MAX_NAME_LEN];
                if (!selected[i]) continue;
                if (state_symbol_row_name(&all_rows->items[i], row_name, sizeof(row_name)) &&
                    !string_list_add_unique(&selected_names, row_name)) {
                    free(selected);
                    free_string_list(&selected_names);
                    free_string_list(&dependency_names);
                    return false;
                }
                if (include_dependencies &&
                    !state_symbol_row_collect_call_basenames(&all_rows->items[i], &dependency_names)) {
                    free(selected);
                    free_string_list(&selected_names);
                    free_string_list(&dependency_names);
                    return false;
                }
            }

            for (int i = 0; i < all_rows->count; i++) {
                char row_name[MAX_NAME_LEN];
                if (selected[i]) continue;
                if (include_impact &&
                    state_symbol_row_calls_selected(&all_rows->items[i], &selected_names)) {
                    selected[i] = 1;
                    changed = true;
                    continue;
                }
                if (include_dependencies &&
                    state_symbol_row_name(&all_rows->items[i], row_name, sizeof(row_name)) &&
                    string_list_index_of(&dependency_names, row_name) != -1) {
                    selected[i] = 1;
                    changed = true;
                }
            }

            free_string_list(&selected_names);
            free_string_list(&dependency_names);
        }
    }

    for (int i = 0; i < all_rows->count; i++) {
        if (!selected[i]) continue;
        if (!state_symbol_rows_add(selected_rows, all_rows->items[i].symbol_id,
                                   all_rows->items[i].module_ref, all_rows->items[i].kind,
                                   all_rows->items[i].contract)) {
            free(selected);
            free_state_symbol_rows(selected_rows);
            return false;
        }
    }

    free(selected);
    return true;
}

static bool build_query_graph_nodes(const StateSymbolRowList* rows, QueryGraphNode** out_nodes) {
    if (!rows || !out_nodes) return false;
    *out_nodes = NULL;
    if (rows->count == 0) return true;

    QueryGraphNode* nodes = (QueryGraphNode*)calloc((size_t)rows->count, sizeof(QueryGraphNode));
    if (!nodes) return false;

    for (int i = 0; i < rows->count; i++) {
        CompactSymbolRow parsed;
        if (!parse_compact_symbol_row(rows->items[i].kind[0] == 'T' ? "target" : "caller",
                                      rows->items[i].contract, &parsed)) {
            free_query_graph_nodes(nodes, rows->count);
            return false;
        }

        if (parsed.parsed && parsed.name[0] != '\0') {
            copy_text(nodes[i].name, sizeof(nodes[i].name), parsed.name);
        } else if (!state_symbol_row_name(&rows->items[i], nodes[i].name, sizeof(nodes[i].name))) {
            copy_text(nodes[i].name, sizeof(nodes[i].name), rows->items[i].symbol_id);
        }
        nodes[i].is_public = parsed.is_public;

        for (int j = 0; j < parsed.declared_effects.count; j++) {
            if (!string_list_add_unique(&nodes[i].self_effects, parsed.declared_effects.items[j])) {
                free_compact_symbol_row(&parsed);
                free_query_graph_nodes(nodes, rows->count);
                return false;
            }
        }
        for (int j = 0; j < parsed.inferred_effects.count; j++) {
            if (!string_list_add_unique(&nodes[i].self_effects, parsed.inferred_effects.items[j])) {
                free_compact_symbol_row(&parsed);
                free_query_graph_nodes(nodes, rows->count);
                return false;
            }
        }
        for (int j = 0; j < parsed.calls.count; j++) {
            char call_name[MAX_NAME_LEN * 2];
            copy_text(call_name, sizeof(call_name), parsed.calls.items[j]);
            char* slash = strchr(call_name, '/');
            if (slash) *slash = '\0';
            char* dot = strrchr(call_name, '.');
            const char* base = dot ? dot + 1 : call_name;
            if (!string_list_add_unique(&nodes[i].call_basenames, base)) {
                free_compact_symbol_row(&parsed);
                free_query_graph_nodes(nodes, rows->count);
                return false;
            }
        }

        free_compact_symbol_row(&parsed);
    }

    *out_nodes = nodes;
    return true;
}

static bool query_graph_node_calls_name(const QueryGraphNode* node, const char* name) {
    return node && name && name[0] != '\0' &&
           string_list_index_of(&node->call_basenames, name) != -1;
}

static bool build_query_trace(const StateSymbolRowList* rows,
                              const char* name_filter, const char* effect_filter,
                              const char* call_filter, bool include_impact,
                              bool include_dependencies, int** parents_out,
                              char** reasons_out, int** queue_out) {
    if (!rows || !parents_out || !reasons_out || !queue_out) return false;
    *parents_out = NULL;
    *reasons_out = NULL;
    *queue_out = NULL;
    if (rows->count < 0) return false;
    if (rows->count == 0) return true;

    QueryGraphNode* nodes = NULL;
    int* parents = NULL;
    int* queue = NULL;
    char* reasons = NULL;
    size_t row_count = (size_t)rows->count;

    if (!build_query_graph_nodes(rows, &nodes)) return false;

    parents = (int*)malloc(row_count * sizeof(int));
    queue = (int*)malloc(row_count * sizeof(int));
    reasons = (char*)malloc(row_count * sizeof(char));
    if (!parents || !queue || !reasons) {
        free(parents);
        free(queue);
        free(reasons);
        free_query_graph_nodes(nodes, rows->count);
        return false;
    }

    for (int i = 0; i < rows->count; i++) {
        parents[i] = -2;
        reasons[i] = '-';
    }

    int head = 0;
    int tail = 0;
    for (int i = 0; i < rows->count; i++) {
        if (!state_symbol_row_matches_query(&rows->items[i], name_filter, effect_filter, call_filter)) {
            continue;
        }
        parents[i] = -1;
        reasons[i] = 's';
        queue[tail++] = i;
    }

    while (head < tail) {
        int current = queue[head++];
        for (int i = 0; i < rows->count; i++) {
            if (parents[i] != -2) continue;
            if (include_dependencies &&
                query_graph_node_calls_name(&nodes[current], nodes[i].name)) {
                parents[i] = current;
                reasons[i] = 'd';
                queue[tail++] = i;
                continue;
            }
            if (include_impact &&
                query_graph_node_calls_name(&nodes[i], nodes[current].name)) {
                parents[i] = current;
                reasons[i] = 'i';
                queue[tail++] = i;
            }
        }
    }

    free_query_graph_nodes(nodes, rows->count);
    *parents_out = parents;
    *reasons_out = reasons;
    *queue_out = queue;
    return true;
}

static bool emit_query_seed_explain(FILE* out, const StateSymbolRow* row,
                                    const char* name_filter, const char* effect_filter,
                                    const char* call_filter) {
    if (!out || !row) return false;

    bool first = true;
    char row_name[MAX_NAME_LEN];
    row_name[0] = '\0';
    state_symbol_row_name(row, row_name, sizeof(row_name));

    if (name_filter && name_filter[0] != '\0' &&
        (strcmp(row->symbol_id, name_filter) == 0 ||
         (row_name[0] != '\0' && strcmp(row_name, name_filter) == 0))) {
        fprintf(out, "%sname=%s", first ? "" : ",", name_filter);
        first = false;
    }
    if (effect_filter && effect_filter[0] != '\0' &&
        state_symbol_row_has_effect(row, effect_filter)) {
        fprintf(out, "%seffect=%s", first ? "" : ",", effect_filter);
        first = false;
    }
    if (call_filter && call_filter[0] != '\0' &&
        state_symbol_row_calls_filter(row, call_filter)) {
        fprintf(out, "%scall=%s", first ? "" : ",", call_filter);
        first = false;
    }
    if (first) fprintf(out, "direct");
    return true;
}

static void emit_query_path_chain(FILE* out, const StateSymbolRowList* rows,
                                  const int* parents, int* scratch, int index) {
    int stack_len = 0;
    int cursor = index;
    while (cursor >= 0 && stack_len < rows->count) {
        scratch[stack_len++] = cursor;
        cursor = parents[cursor];
    }
    for (int j = stack_len - 1; j >= 0; j--) {
        fprintf(out, "%s%s", j == stack_len - 1 ? "" : "->", rows->items[scratch[j]].symbol_id);
    }
}

static bool emit_query_paths_and_explain(FILE* out, const StateSymbolRowList* rows,
                                         const char* name_filter, const char* effect_filter,
                                         const char* call_filter, bool include_impact,
                                         bool include_dependencies) {
    if (!out || !rows) return false;

    int* parents = NULL;
    int* queue = NULL;
    char* reasons = NULL;
    if (!build_query_trace(rows, name_filter, effect_filter, call_filter,
                           include_impact, include_dependencies,
                           &parents, &reasons, &queue)) {
        return false;
    }

    fprintf(out, "paths:\n");
    for (int i = 0; i < rows->count; i++) {
        fprintf(out, "  %s=", rows->items[i].symbol_id);
        if (!parents || parents[i] == -2 || parents[i] == -1) {
            fprintf(out, "seed\n");
            continue;
        }
        emit_query_path_chain(out, rows, parents, queue, i);
        fprintf(out, "\n");
    }

    fprintf(out, "explain:\n");
    for (int i = 0; i < rows->count; i++) {
        fprintf(out, "  %s=", rows->items[i].symbol_id);
        if (!parents || parents[i] == -2 || reasons[i] == 's') {
            fprintf(out, "seed(");
            emit_query_seed_explain(out, &rows->items[i], name_filter, effect_filter, call_filter);
            fprintf(out, ")\n");
            continue;
        }

        int depth = 0;
        int root = i;
        while (parents[root] >= 0) {
            depth++;
            root = parents[root];
        }
        fprintf(out, "%s(from=%s,depth=%d",
                reasons[i] == 'd' ? "deps" : "impact",
                rows->items[root].symbol_id, depth);
        if (effect_filter && effect_filter[0] != '\0') {
            fprintf(out, ",effect=%s", effect_filter);
        }
        fprintf(out, ")\n");
    }

    free(parents);
    free(queue);
    free(reasons);
    return true;
}

static int compute_query_blast_radius(const QueryGraphNode* nodes, int node_count, int start,
                                      unsigned char* visited, int* queue) {
    if (!nodes || node_count <= 0 || start < 0 || start >= node_count || !visited || !queue) return 0;

    memset(visited, 0, (size_t)node_count);
    int head = 0;
    int tail = 0;
    int radius = 0;
    visited[start] = 1;
    queue[tail++] = start;

    while (head < tail) {
        int current = queue[head++];
        for (int i = 0; i < node_count; i++) {
            if (visited[i]) continue;
            if (!query_graph_node_calls_name(&nodes[i], nodes[current].name)) continue;
            visited[i] = 1;
            queue[tail++] = i;
            radius++;
        }
    }

    return radius;
}

static int collect_query_dependency_effects(const QueryGraphNode* nodes, int node_count, int start,
                                            unsigned char* visited, int* queue, StringList* effects_out) {
    if (!nodes || node_count <= 0 || start < 0 || start >= node_count ||
        !visited || !queue || !effects_out) {
        return 0;
    }

    memset(visited, 0, (size_t)node_count);
    int head = 0;
    int tail = 0;
    visited[start] = 1;
    queue[tail++] = start;

    while (head < tail) {
        int current = queue[head++];
        for (int i = 0; i < nodes[current].self_effects.count; i++) {
            if (!string_list_add_unique(effects_out, nodes[current].self_effects.items[i])) {
                return -1;
            }
        }
        for (int i = 0; i < node_count; i++) {
            if (visited[i]) continue;
            if (!query_graph_node_calls_name(&nodes[current], nodes[i].name)) continue;
            visited[i] = 1;
            queue[tail++] = i;
        }
    }

    return effects_out->count;
}

static void emit_string_list_csv(FILE* out, const StringList* list) {
    if (!out || !list || list->count == 0) {
        fprintf(out, "-");
        return;
    }
    for (int i = 0; i < list->count; i++) {
        fprintf(out, "%s%s", i == 0 ? "" : ",", list->items[i]);
    }
}

static int compare_query_risk_order(int lhs_score, int rhs_score,
                                    int lhs_blast, int rhs_blast,
                                    int lhs_effects, int rhs_effects,
                                    bool lhs_public, bool rhs_public,
                                    const char* lhs_symbol, const char* rhs_symbol) {
    if (lhs_score != rhs_score) return lhs_score > rhs_score ? -1 : 1;
    if (lhs_blast != rhs_blast) return lhs_blast > rhs_blast ? -1 : 1;
    if (lhs_effects != rhs_effects) return lhs_effects > rhs_effects ? -1 : 1;
    if (lhs_public != rhs_public) return lhs_public ? -1 : 1;
    return strcmp(lhs_symbol, rhs_symbol);
}

static bool build_query_risk_stats(const StateSymbolRowList* rows, QueryRiskStats* stats) {
    if (!rows || !stats) return false;
    memset(stats, 0, sizeof(*stats));
    stats->count = rows->count;
    if (rows->count <= 0) return true;

    stats->nodes = NULL;
    if (!build_query_graph_nodes(rows, &stats->nodes)) return false;

    unsigned char* visited = (unsigned char*)malloc((size_t)rows->count);
    int* queue = (int*)malloc((size_t)rows->count * sizeof(int));
    stats->scores = (int*)malloc((size_t)rows->count * sizeof(int));
    stats->blast = (int*)malloc((size_t)rows->count * sizeof(int));
    stats->dep_effect_counts = (int*)malloc((size_t)rows->count * sizeof(int));
    stats->dep_effects = (StringList*)calloc((size_t)rows->count, sizeof(StringList));
    if (!visited || !queue || !stats->scores || !stats->blast ||
        !stats->dep_effect_counts || !stats->dep_effects) {
        free(visited);
        free(queue);
        free_query_risk_stats(stats);
        return false;
    }

    bool ok = true;
    for (int i = 0; i < rows->count; i++) {
        stats->blast[i] = compute_query_blast_radius(stats->nodes, rows->count, i, visited, queue);
        stats->dep_effect_counts[i] = collect_query_dependency_effects(stats->nodes, rows->count, i,
                                                                       visited, queue,
                                                                       &stats->dep_effects[i]);
        if (stats->dep_effect_counts[i] < 0) {
            ok = false;
            break;
        }
        stats->scores[i] = stats->blast[i] * 10 + stats->dep_effect_counts[i] * 6 +
                           (stats->nodes[i].is_public ? 2 : 0) +
                           (rows->items[i].kind[0] == 'T' ? 1 : 0);
    }

    free(visited);
    free(queue);
    if (!ok) {
        free_query_risk_stats(stats);
        return false;
    }
    return true;
}

static bool sort_query_risk_order(const StateSymbolRowList* rows, const QueryRiskStats* stats,
                                  int* order) {
    if (!rows || !stats || !order) return false;
    for (int i = 0; i < rows->count; i++) order[i] = i;
    for (int i = 0; i < rows->count - 1; i++) {
        for (int j = i + 1; j < rows->count; j++) {
            int lhs = order[i];
            int rhs = order[j];
            if (compare_query_risk_order(stats->scores[lhs], stats->scores[rhs],
                                         stats->blast[lhs], stats->blast[rhs],
                                         stats->dep_effect_counts[lhs], stats->dep_effect_counts[rhs],
                                         stats->nodes[lhs].is_public, stats->nodes[rhs].is_public,
                                         rows->items[lhs].symbol_id, rows->items[rhs].symbol_id) > 0) {
                int tmp = order[i];
                order[i] = order[j];
                order[j] = tmp;
            }
        }
    }
    return true;
}

static bool emit_query_risk_top(FILE* out, const StateSymbolRowList* rows) {
    if (!out || !rows) return false;
    fprintf(out, "risk_top:\n");
    if (rows->count <= 0) return true;

    QueryRiskStats stats;
    if (!build_query_risk_stats(rows, &stats)) return false;

    int* order = (int*)malloc((size_t)rows->count * sizeof(int));
    if (!order) {
        free_query_risk_stats(&stats);
        return false;
    }

    bool ok = sort_query_risk_order(rows, &stats, order);
    if (ok) {
        int limit = rows->count < 5 ? rows->count : 5;
        for (int rank = 0; rank < limit; rank++) {
            int idx = order[rank];
            fprintf(out, "  %d|%s|name=%s|score=%d|blast=%d|depfx=",
                    rank + 1, rows->items[idx].symbol_id, stats.nodes[idx].name,
                    stats.scores[idx], stats.blast[idx]);
            emit_string_list_csv(out, &stats.dep_effects[idx]);
            fprintf(out, "|pub=%d\n", stats.nodes[idx].is_public ? 1 : 0);
        }
    }

    free(order);
    free_query_risk_stats(&stats);
    return ok;
}

static void emit_change_plan_checks(FILE* out, const char* status, int dep_effect_count, int blast) {
    bool first = true;
    if (!out) return;

    if (status && strcmp(status, "changed") == 0) {
        fprintf(out, "contract");
        first = false;
    } else if (status && strcmp(status, "added") == 0) {
        fprintf(out, "new_surface");
        first = false;
    } else if (status && strcmp(status, "removed") == 0) {
        fprintf(out, "deleted_surface");
        first = false;
    }
    if (dep_effect_count > 0) {
        fprintf(out, "%seffects", first ? "" : ",");
        first = false;
    }
    if (blast > 0) {
        fprintf(out, "%scallers", first ? "" : ",");
        first = false;
    }
    if (first) fprintf(out, "review");
}

static bool emit_semantic_change_plan(FILE* out, const StateSymbolRowList* before_rows,
                                      const StateSymbolRowList* after_rows) {
    if (!out || !before_rows || !after_rows) return false;

    QueryRiskStats before_stats;
    QueryRiskStats after_stats;
    memset(&before_stats, 0, sizeof(before_stats));
    memset(&after_stats, 0, sizeof(after_stats));
    if (!build_query_risk_stats(before_rows, &before_stats) ||
        !build_query_risk_stats(after_rows, &after_stats)) {
        free_query_risk_stats(&before_stats);
        free_query_risk_stats(&after_stats);
        return false;
    }

    StringList identities;
    memset(&identities, 0, sizeof(identities));
    bool ok = true;
    for (int i = 0; ok && i < before_rows->count; i++) {
        char diff_identity[256];
        ok = state_symbol_row_diff_identity(&before_rows->items[i], diff_identity, sizeof(diff_identity)) &&
             string_list_add_unique(&identities, diff_identity);
    }
    for (int i = 0; ok && i < after_rows->count; i++) {
        char diff_identity[256];
        ok = state_symbol_row_diff_identity(&after_rows->items[i], diff_identity, sizeof(diff_identity)) &&
             string_list_add_unique(&identities, diff_identity);
    }
    if (!ok) {
        free_string_list(&identities);
        free_query_risk_stats(&before_stats);
        free_query_risk_stats(&after_stats);
        return false;
    }

    SemanticChangePlanItem* plan_items = NULL;
    int plan_count = 0;
    if (identities.count > 0) {
        plan_items = (SemanticChangePlanItem*)calloc((size_t)identities.count, sizeof(SemanticChangePlanItem));
        if (!plan_items) {
            free_string_list(&identities);
            free_query_risk_stats(&before_stats);
            free_query_risk_stats(&after_stats);
            return false;
        }
    }

    for (int i = 0; i < identities.count; i++) {
        int before_index = state_symbol_rows_index_of_diff_identity(before_rows, identities.items[i]);
        int after_index = state_symbol_rows_index_of_diff_identity(after_rows, identities.items[i]);
        if (before_index != -1 && after_index != -1 &&
            state_symbol_row_diff_equals(&before_rows->items[before_index], &after_rows->items[after_index])) {
            continue;
        }

        SemanticChangePlanItem* item = &plan_items[plan_count++];
        copy_text(item->diff_identity, sizeof(item->diff_identity), identities.items[i]);
        item->before_index = before_index;
        item->after_index = after_index;
        if (before_index == -1) {
            copy_text(item->status, sizeof(item->status), "added");
        } else if (after_index == -1) {
            copy_text(item->status, sizeof(item->status), "removed");
        } else {
            copy_text(item->status, sizeof(item->status), "changed");
        }
    }

    for (int i = 0; i < plan_count - 1; i++) {
        for (int j = i + 1; j < plan_count; j++) {
            const SemanticChangePlanItem* lhs_item = &plan_items[i];
            const SemanticChangePlanItem* rhs_item = &plan_items[j];
            bool lhs_after = lhs_item->after_index != -1;
            bool rhs_after = rhs_item->after_index != -1;
            int lhs_index = lhs_after ? lhs_item->after_index : lhs_item->before_index;
            int rhs_index = rhs_after ? rhs_item->after_index : rhs_item->before_index;
            const QueryRiskStats* lhs_stats = lhs_after ? &after_stats : &before_stats;
            const QueryRiskStats* rhs_stats = rhs_after ? &after_stats : &before_stats;
            const StateSymbolRowList* lhs_rows = lhs_after ? after_rows : before_rows;
            const StateSymbolRowList* rhs_rows = rhs_after ? after_rows : before_rows;

            if (compare_query_risk_order(lhs_stats->scores[lhs_index], rhs_stats->scores[rhs_index],
                                         lhs_stats->blast[lhs_index], rhs_stats->blast[rhs_index],
                                         lhs_stats->dep_effect_counts[lhs_index], rhs_stats->dep_effect_counts[rhs_index],
                                         lhs_stats->nodes[lhs_index].is_public, rhs_stats->nodes[rhs_index].is_public,
                                         lhs_rows->items[lhs_index].symbol_id, rhs_rows->items[rhs_index].symbol_id) > 0) {
                SemanticChangePlanItem tmp = plan_items[i];
                plan_items[i] = plan_items[j];
                plan_items[j] = tmp;
            }
        }
    }

    fprintf(out, "change_plan:\n");
    for (int i = 0; i < plan_count; i++) {
        bool use_after = plan_items[i].after_index != -1;
        int row_index = use_after ? plan_items[i].after_index : plan_items[i].before_index;
        const QueryRiskStats* stats = use_after ? &after_stats : &before_stats;
        const StateSymbolRowList* rows = use_after ? after_rows : before_rows;
        const char* kind = rows->items[row_index].kind[0] == 'T' ? "target" : "caller";

        fprintf(out, "  %d|status=%s|kind=%s|name=%s|score=%d|blast=%d|depfx=",
                i + 1, plan_items[i].status, kind, stats->nodes[row_index].name,
                stats->scores[row_index], stats->blast[row_index]);
        emit_string_list_csv(out, &stats->dep_effects[row_index]);
        fprintf(out, "|checks=");
        emit_change_plan_checks(out, plan_items[i].status,
                                stats->dep_effect_counts[row_index], stats->blast[row_index]);
        fprintf(out, "\n");
    }

    free(plan_items);
    free_string_list(&identities);
    free_query_risk_stats(&before_stats);
    free_query_risk_stats(&after_stats);
    return true;
}

static bool collect_semantic_change_candidates(const SemanticItemList* before_items,
                                               const SemanticItemList* after_items,
                                               const StateSymbolRowList* before_rows,
                                               const StateSymbolRowList* after_rows,
                                               SemanticChangeCandidate** out_items,
                                               int* out_count) {
    if (!before_items || !after_items || !before_rows || !after_rows || !out_items || !out_count) {
        return false;
    }
    *out_items = NULL;
    *out_count = 0;

    QueryRiskStats before_stats;
    QueryRiskStats after_stats;
    memset(&before_stats, 0, sizeof(before_stats));
    memset(&after_stats, 0, sizeof(after_stats));
    if (!build_query_risk_stats(before_rows, &before_stats) ||
        !build_query_risk_stats(after_rows, &after_stats)) {
        free_query_risk_stats(&before_stats);
        free_query_risk_stats(&after_stats);
        return false;
    }

    StringList identities;
    memset(&identities, 0, sizeof(identities));
    bool ok = true;
    for (int i = 0; ok && i < before_rows->count; i++) {
        char diff_identity[256];
        ok = state_symbol_row_diff_identity(&before_rows->items[i], diff_identity, sizeof(diff_identity)) &&
             string_list_add_unique(&identities, diff_identity);
    }
    for (int i = 0; ok && i < after_rows->count; i++) {
        char diff_identity[256];
        ok = state_symbol_row_diff_identity(&after_rows->items[i], diff_identity, sizeof(diff_identity)) &&
             string_list_add_unique(&identities, diff_identity);
    }
    if (!ok) {
        free_string_list(&identities);
        free_query_risk_stats(&before_stats);
        free_query_risk_stats(&after_stats);
        return false;
    }

    SemanticChangeCandidate* items = NULL;
    int count = 0;
    int cap = 0;
    for (int i = 0; i < identities.count; i++) {
        int before_index = state_symbol_rows_index_of_diff_identity(before_rows, identities.items[i]);
        int after_index = state_symbol_rows_index_of_diff_identity(after_rows, identities.items[i]);
        if (before_index != -1 && after_index != -1 &&
            state_symbol_row_diff_equals(&before_rows->items[before_index], &after_rows->items[after_index])) {
            continue;
        }

        const StateSymbolRowList* rows = after_index != -1 ? after_rows : before_rows;
        const QueryRiskStats* stats = after_index != -1 ? &after_stats : &before_stats;
        const SemanticItemList* semantic_items = after_index != -1 ? after_items : before_items;
        int row_index = after_index != -1 ? after_index : before_index;
        const char* status = after_index == -1 ? "removed" : (before_index == -1 ? "added" : "changed");

        char contract[4608];
        char module_path[MAX_PATH_LEN];
        bool found = false;
        for (int j = 0; j < semantic_items->count; j++) {
            char item_kind[16];
            char item_module[MAX_PATH_LEN];
            char item_contract[4608];
            if (!parse_semantic_item_text(semantic_items->items[j].text, item_kind, sizeof(item_kind),
                                          item_module, sizeof(item_module),
                                          item_contract, sizeof(item_contract))) {
                continue;
            }
            if (strcmp(item_contract, rows->items[row_index].contract) != 0) continue;
            copy_text(module_path, sizeof(module_path), item_module);
            copy_text(contract, sizeof(contract), item_contract);
            found = true;
            break;
        }
        if (!found) {
            free(items);
            free_string_list(&identities);
            free_query_risk_stats(&before_stats);
            free_query_risk_stats(&after_stats);
            return false;
        }

        if (!ensure_capacity((void**)&items, &cap, count + 1, sizeof(SemanticChangeCandidate))) {
            free(items);
            free_string_list(&identities);
            free_query_risk_stats(&before_stats);
            free_query_risk_stats(&after_stats);
            return false;
        }

        SemanticChangeCandidate* item = &items[count++];
        memset(item, 0, sizeof(*item));
        copy_text(item->diff_identity, sizeof(item->diff_identity), identities.items[i]);
        copy_text(item->module_path, sizeof(item->module_path), module_path);
        copy_text(item->status, sizeof(item->status), status);
        copy_text(item->name, sizeof(item->name), stats->nodes[row_index].name);
        if (!build_module_symbol_identity(module_path, contract,
                                          item->module_symbol_identity,
                                          sizeof(item->module_symbol_identity))) {
            free(items);
            free_string_list(&identities);
            free_query_risk_stats(&before_stats);
            free_query_risk_stats(&after_stats);
            return false;
        }
        item->score = stats->scores[row_index];
        item->blast = stats->blast[row_index];
        item->dep_effect_count = stats->dep_effect_counts[row_index];
    }

    free_string_list(&identities);
    free_query_risk_stats(&before_stats);
    free_query_risk_stats(&after_stats);
    *out_items = items;
    *out_count = count;
    return true;
}

static void emit_test_plan_checks(FILE* out, bool has_changed, bool has_added, bool has_removed,
                                  bool has_effects, bool has_callers) {
    bool first = true;
    if (!out) return;
    if (has_changed) {
        fprintf(out, "contract");
        first = false;
    }
    if (has_added) {
        fprintf(out, "%snew_surface", first ? "" : ",");
        first = false;
    }
    if (has_removed) {
        fprintf(out, "%sdeleted_surface", first ? "" : ",");
        first = false;
    }
    if (has_effects) {
        fprintf(out, "%seffects", first ? "" : ",");
        first = false;
    }
    if (has_callers) {
        fprintf(out, "%scallers", first ? "" : ",");
        first = false;
    }
    if (first) fprintf(out, "smoke");
}

static bool emit_semantic_test_plan(FILE* out, const SemanticItemList* before_items,
                                    const SemanticItemList* after_items,
                                    const StateSymbolRowList* before_rows,
                                    const StateSymbolRowList* after_rows) {
    if (!out || !before_items || !after_items || !before_rows || !after_rows) return false;

    SemanticChangeCandidate* changes = NULL;
    int change_count = 0;
    if (!collect_semantic_change_candidates(before_items, after_items, before_rows, after_rows,
                                            &changes, &change_count)) {
        return false;
    }

    fprintf(out, "test_plan:\n");
    if (change_count == 0) {
        free(changes);
        return true;
    }

    StringList test_files;
    memset(&test_files, 0, sizeof(test_files));
    if (!collect_candidate_test_files(index_project_root[0] != '\0' ? index_project_root : ".", &test_files)) {
        free(changes);
        free_string_list(&test_files);
        return false;
    }

    typedef struct {
        char path[MAX_PATH_LEN];
        int hits;
        int max_score;
        bool has_changed;
        bool has_added;
        bool has_removed;
        bool has_effects;
        bool has_callers;
        StringList symbols;
    } TestPlanCandidate;

    TestPlanCandidate* candidates = NULL;
    int candidate_count = 0;
    int candidate_cap = 0;
    bool ok = true;

    for (int i = 0; i < test_files.count && ok; i++) {
        char test_entry[MAX_PATH_LEN];
        ProjectIndex test_project;
        StringList identities;
        char* test_source = read_file(test_files.items[i]);
        memset(&test_project, 0, sizeof(test_project));
        memset(&identities, 0, sizeof(identities));
        if (!test_source) continue;

        bool maybe_relevant = false;
        for (int j = 0; j < change_count && !maybe_relevant; j++) {
            const char* slash = strrchr(changes[j].module_path, '/');
            const char* basename = slash ? slash + 1 : changes[j].module_path;
            if (basename[0] != '\0' && strstr(test_source, basename) != NULL) {
                maybe_relevant = true;
            }
        }
        if (!maybe_relevant) {
            free(test_source);
            continue;
        }

        int hits = 0;
        int max_score = 0;
        bool has_changed = false;
        bool has_added = false;
        bool has_removed = false;
        bool has_effects = false;
        bool has_callers = false;
        StringList matched_symbols;
        memset(&matched_symbols, 0, sizeof(matched_symbols));

        bool manifest_link = strstr(test_source, "@covers") != NULL;
        if (manifest_link) {
            for (int j = 0; j < change_count; j++) {
                const char* slash = strrchr(changes[j].module_path, '/');
                const char* basename = slash ? slash + 1 : changes[j].module_path;
                if ((basename[0] == '\0' || strstr(test_source, basename) == NULL) ||
                    (changes[j].name[0] == '\0' || strstr(test_source, changes[j].name) == NULL)) {
                    continue;
                }
                hits++;
                if (changes[j].score > max_score) max_score = changes[j].score;
                if (strcmp(changes[j].status, "changed") == 0) has_changed = true;
                if (strcmp(changes[j].status, "added") == 0) has_added = true;
                if (strcmp(changes[j].status, "removed") == 0) has_removed = true;
                if (changes[j].dep_effect_count > 0) has_effects = true;
                if (changes[j].blast > 0) has_callers = true;
                if (!string_list_add_unique(&matched_symbols, changes[j].name)) {
                    free_string_list(&matched_symbols);
                    ok = false;
                    break;
                }
            }
        }

        if (!manifest_link) {
            free(test_source);
            if (!build_project_index(test_files.items[i], &test_project, test_entry, sizeof(test_entry))) {
                free_string_list(&identities);
                free_string_list(&matched_symbols);
                continue;
            }
            if (!collect_project_symbol_identities(&test_project, &identities)) {
                free_project(&test_project);
                free_string_list(&identities);
                free_string_list(&matched_symbols);
                continue;
            }
            for (int j = 0; j < change_count; j++) {
                if (string_list_index_of(&identities, changes[j].module_symbol_identity) == -1) continue;
                hits++;
                if (changes[j].score > max_score) max_score = changes[j].score;
                if (strcmp(changes[j].status, "changed") == 0) has_changed = true;
                if (strcmp(changes[j].status, "added") == 0) has_added = true;
                if (strcmp(changes[j].status, "removed") == 0) has_removed = true;
                if (changes[j].dep_effect_count > 0) has_effects = true;
                if (changes[j].blast > 0) has_callers = true;
                if (!string_list_add_unique(&matched_symbols, changes[j].name)) {
                    free_string_list(&matched_symbols);
                    ok = false;
                    break;
                }
            }
        } else {
            free(test_source);
        }

        free_project(&test_project);
        free_string_list(&identities);
        if (!ok) {
            free_string_list(&matched_symbols);
            break;
        }
        if (hits == 0) {
            free_string_list(&matched_symbols);
            continue;
        }

        if (!ensure_capacity((void**)&candidates, &candidate_cap, candidate_count + 1,
                             sizeof(TestPlanCandidate))) {
            free_string_list(&matched_symbols);
            ok = false;
            break;
        }

        TestPlanCandidate* candidate = &candidates[candidate_count++];
        memset(candidate, 0, sizeof(*candidate));
        copy_text(candidate->path, sizeof(candidate->path), test_files.items[i]);
        candidate->hits = hits;
        candidate->max_score = max_score;
        candidate->has_changed = has_changed;
        candidate->has_added = has_added;
        candidate->has_removed = has_removed;
        candidate->has_effects = has_effects;
        candidate->has_callers = has_callers;
        candidate->symbols = matched_symbols;
    }

    if (ok) {
        for (int i = 0; i < candidate_count - 1; i++) {
            for (int j = i + 1; j < candidate_count; j++) {
                if (candidates[i].hits < candidates[j].hits ||
                    (candidates[i].hits == candidates[j].hits && candidates[i].max_score < candidates[j].max_score) ||
                    (candidates[i].hits == candidates[j].hits && candidates[i].max_score == candidates[j].max_score &&
                     strcmp(candidates[i].path, candidates[j].path) > 0)) {
                    TestPlanCandidate tmp = candidates[i];
                    candidates[i] = candidates[j];
                    candidates[j] = tmp;
                }
            }
        }

        for (int i = 0; i < candidate_count; i++) {
            char display_path[MAX_PATH_LEN];
            format_pack_path(candidates[i].path, display_path, sizeof(display_path));
            fprintf(out, "  %d|test=%s|hits=%d|max_score=%d|symbols=",
                    i + 1, display_path, candidates[i].hits, candidates[i].max_score);
            emit_string_list_csv(out, &candidates[i].symbols);
            fprintf(out, "|checks=");
            emit_test_plan_checks(out, candidates[i].has_changed, candidates[i].has_added,
                                  candidates[i].has_removed, candidates[i].has_effects,
                                  candidates[i].has_callers);
            fprintf(out, "\n");
        }
    }

    for (int i = 0; i < candidate_count; i++) {
        free_string_list(&candidates[i].symbols);
    }
    free(candidates);
    free(changes);
    free_string_list(&test_files);
    return ok;
}

static bool state_plan_candidate_record_hit(StatePlanTestCandidate* candidate,
                                            const SemanticChangeCandidate* change) {
    if (!candidate || !change) return false;
    candidate->hits++;
    if (change->score > candidate->max_score) candidate->max_score = change->score;
    if (strcmp(change->status, "changed") == 0) candidate->has_changed = true;
    if (strcmp(change->status, "added") == 0) candidate->has_added = true;
    if (strcmp(change->status, "removed") == 0) candidate->has_removed = true;
    if (change->dep_effect_count > 0) candidate->has_effects = true;
    if (change->blast > 0) candidate->has_callers = true;
    return string_list_add_unique(&candidate->symbols, change->name);
}

static bool state_plan_candidate_status_selected(const char* status, bool include_changed,
                                                 bool include_added, bool include_removed) {
    if (!status) return false;
    if (include_changed && strcmp(status, "changed") == 0) return true;
    if (include_added && strcmp(status, "added") == 0) return true;
    if (include_removed && strcmp(status, "removed") == 0) return true;
    return false;
}

static StatePlanTestCandidate* find_state_plan_candidate(StatePlanTestCandidateList* list,
                                                         const char* path) {
    if (!list || !path) return NULL;
    for (int i = 0; i < list->count; i++) {
        if (strcmp(list->items[i].path, path) == 0) return &list->items[i];
    }
    return NULL;
}

static bool collect_state_test_plan_candidates(const char* state_path, const StateSymbolRowList* rows,
                                               const SemanticChangeCandidate* changes, int change_count,
                                               bool include_changed, bool include_added,
                                               bool include_removed, StatePlanTestCandidateList* out) {
    if (!state_path || !rows || !changes || !out) return false;

    FILE* file = fopen(state_path, "r");
    if (!file) return false;

    char line[8192];
    bool in_tests = false;
    bool ok = true;
    while (fgets(line, sizeof(line), file)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';

        if (!in_tests) {
            if (strcmp(line, "test_index:") == 0) in_tests = true;
            continue;
        }
        if (strcmp(line, "context:") == 0) break;
        if (line[0] == '\0') continue;

        char path[MAX_PATH_LEN];
        StringList covered_ids;
        memset(&covered_ids, 0, sizeof(covered_ids));
        if (!parse_test_index_row(line, path, sizeof(path), &covered_ids)) {
            free_string_list(&covered_ids);
            ok = false;
            break;
        }

        StatePlanTestCandidate* candidate = NULL;
        for (int i = 0; i < change_count; i++) {
            if (!state_plan_candidate_status_selected(changes[i].status, include_changed,
                                                      include_added, include_removed)) {
                continue;
            }
            int row_index = state_symbol_rows_index_of_diff_identity(rows, changes[i].diff_identity);
            if (row_index == -1 ||
                string_list_index_of(&covered_ids, rows->items[row_index].symbol_id) == -1) {
                continue;
            }
            if (!candidate) {
                candidate = find_state_plan_candidate(out, path);
                if (!candidate) {
                    if (!ensure_capacity((void**)&out->items, &out->cap, out->count + 1,
                                         sizeof(StatePlanTestCandidate))) {
                        ok = false;
                        break;
                    }
                    candidate = &out->items[out->count++];
                    memset(candidate, 0, sizeof(*candidate));
                    copy_text(candidate->path, sizeof(candidate->path), path);
                }
            }
            if (!state_plan_candidate_record_hit(candidate, &changes[i])) {
                ok = false;
                break;
            }
        }
        free_string_list(&covered_ids);
        if (!ok) break;
    }
    fclose(file);
    if (!ok || !in_tests) {
        return false;
    }
    return true;
}

static bool emit_state_test_plan(FILE* out, const char* before_state_path, const char* after_state_path,
                                 const SemanticItemList* before_items, const SemanticItemList* after_items,
                                 const StateSymbolRowList* before_rows, const StateSymbolRowList* after_rows) {
    if (!out || !before_state_path || !after_state_path ||
        !before_items || !after_items || !before_rows || !after_rows) {
        return false;
    }

    fprintf(out, "test_plan:\n");
    StatePlanTestCandidateList candidates;
    memset(&candidates, 0, sizeof(candidates));
    bool ok = build_state_test_plan_candidates(before_state_path, after_state_path,
                                               before_items, after_items, before_rows, after_rows,
                                               &candidates);
    if (ok) {
        for (int i = 0; i < candidates.count - 1; i++) {
            for (int j = i + 1; j < candidates.count; j++) {
                if (candidates.items[i].hits < candidates.items[j].hits ||
                    (candidates.items[i].hits == candidates.items[j].hits &&
                     candidates.items[i].max_score < candidates.items[j].max_score) ||
                    (candidates.items[i].hits == candidates.items[j].hits &&
                     candidates.items[i].max_score == candidates.items[j].max_score &&
                     strcmp(candidates.items[i].path, candidates.items[j].path) > 0)) {
                    StatePlanTestCandidate tmp = candidates.items[i];
                    candidates.items[i] = candidates.items[j];
                    candidates.items[j] = tmp;
                }
            }
        }
        for (int i = 0; i < candidates.count; i++) {
            fprintf(out, "  %d|test=%s|hits=%d|max_score=%d|symbols=",
                    i + 1, candidates.items[i].path, candidates.items[i].hits,
                    candidates.items[i].max_score);
            emit_string_list_csv(out, &candidates.items[i].symbols);
            fprintf(out, "|checks=");
            emit_test_plan_checks(out, candidates.items[i].has_changed, candidates.items[i].has_added,
                                  candidates.items[i].has_removed, candidates.items[i].has_effects,
                                  candidates.items[i].has_callers);
            fprintf(out, "\n");
        }
    }

    free_state_plan_test_candidates(&candidates);
    return ok;
}

static bool build_state_test_plan_candidates(const char* before_state_path, const char* after_state_path,
                                             const SemanticItemList* before_items, const SemanticItemList* after_items,
                                             const StateSymbolRowList* before_rows, const StateSymbolRowList* after_rows,
                                             StatePlanTestCandidateList* out) {
    if (!before_state_path || !after_state_path || !before_items || !after_items ||
        !before_rows || !after_rows || !out) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    SemanticChangeCandidate* changes = NULL;
    int change_count = 0;
    if (!collect_semantic_change_candidates(before_items, after_items, before_rows, after_rows,
                                            &changes, &change_count)) {
        return false;
    }
    if (change_count == 0) {
        free(changes);
        return true;
    }

    bool ok = collect_state_test_plan_candidates(after_state_path, after_rows, changes, change_count,
                                                 true, true, false, out);
    if (ok) {
        ok = collect_state_test_plan_candidates(before_state_path, before_rows, changes, change_count,
                                                false, false, true, out);
    }
    free(changes);
    if (!ok) {
        free_state_plan_test_candidates(out);
    }
    return ok;
}

static bool read_failure_preview(const char* path, char* out, size_t out_size) {
    if (!path || !out || out_size == 0) return false;
    out[0] = '\0';
    FILE* file = fopen(path, "r");
    if (!file) return false;
    if (!fgets(out, (int)out_size, file)) {
        fclose(file);
        return true;
    }
    fclose(file);
    size_t n = strlen(out);
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = '\0';
    return true;
}

static bool open_temp_capture_file(char* path, size_t path_size, int* out_fd) {
    if (!path || path_size == 0 || !out_fd) return false;
    *out_fd = -1;
    for (int attempt = 0; attempt < 128; attempt++) {
        if (snprintf(path, path_size, "/tmp/viper-state-run-%ld-%d",
                     (long)getpid(), attempt) >= (int)path_size) {
            return false;
        }
        int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
        if (fd >= 0) {
            *out_fd = fd;
            return true;
        }
    }
    path[0] = '\0';
    return false;
}

static bool run_viper_script_capture(const char* exe_path, const char* script_path,
                                     int* exit_code, char* preview, size_t preview_size) {
    if (!exe_path || !script_path || !exit_code) return false;
    *exit_code = -1;
    if (preview && preview_size > 0) preview[0] = '\0';

    char temp_path[MAX_PATH_LEN];
    int fd = -1;
    if (!open_temp_capture_file(temp_path, sizeof(temp_path), &fd)) return false;

    pid_t pid = fork();
    if (pid < 0) {
        close(fd);
        unlink(temp_path);
        return false;
    }
    if (pid == 0) {
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        close(fd);
        execl(exe_path, exe_path, script_path, (char*)NULL);
        _exit(127);
    }

    close(fd);
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        unlink(temp_path);
        return false;
    }

    if (WIFEXITED(status)) {
        *exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        *exit_code = 128 + WTERMSIG(status);
    }

    if (*exit_code != 0 && preview && preview_size > 0) {
        read_failure_preview(temp_path, preview, preview_size);
    }
    unlink(temp_path);
    return true;
}

static bool select_resume_symbol_rows(const StateSymbolRowList* all_rows, const char* focus_symbol,
                                      bool include_impact, StateSymbolRowList* selected_rows) {
    if (!all_rows || !focus_symbol || !selected_rows) return false;
    memset(selected_rows, 0, sizeof(*selected_rows));

    unsigned char* selected = NULL;
    StringList active_names;
    memset(&active_names, 0, sizeof(active_names));
    if (all_rows->count > 0) {
        selected = (unsigned char*)calloc((size_t)all_rows->count, sizeof(unsigned char));
        if (!selected) return false;
    }

    int selected_count = 0;
    for (int i = 0; i < all_rows->count; i++) {
        char row_name[MAX_NAME_LEN];
        bool matches = strcmp(all_rows->items[i].symbol_id, focus_symbol) == 0;
        if (!matches && state_symbol_row_name(&all_rows->items[i], row_name, sizeof(row_name))) {
            matches = strcmp(row_name, focus_symbol) == 0;
        }
        if (!matches) continue;
        selected[i] = 1;
        selected_count++;
        if (row_name[0] != '\0' && !string_list_add_unique(&active_names, row_name)) {
            free(selected);
            free_string_list(&active_names);
            return false;
        }
    }

    if (selected_count == 0) {
        free(selected);
        free_string_list(&active_names);
        return false;
    }

    if (include_impact) {
        bool changed = true;
        while (changed) {
            changed = false;
            for (int i = 0; i < all_rows->count; i++) {
                if (selected[i]) continue;
                if (!state_symbol_row_calls_selected(&all_rows->items[i], &active_names)) continue;
                char row_name[MAX_NAME_LEN];
                if (!state_symbol_row_name(&all_rows->items[i], row_name, sizeof(row_name))) continue;
                selected[i] = 1;
                selected_count++;
                if (!string_list_add_unique(&active_names, row_name)) {
                    free(selected);
                    free_string_list(&active_names);
                    return false;
                }
                changed = true;
            }
        }
    }

    for (int i = 0; i < all_rows->count; i++) {
        if (!selected[i]) continue;
        if (!state_symbol_rows_add(selected_rows, all_rows->items[i].symbol_id,
                                   all_rows->items[i].module_ref, all_rows->items[i].kind,
                                   all_rows->items[i].contract)) {
            free(selected);
            free_string_list(&active_names);
            free_state_symbol_rows(selected_rows);
            return false;
        }
    }

    free(selected);
    free_string_list(&active_names);
    return true;
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

static bool emit_resume_symbol_ledger_from_rows(const char* state_path, const StateSymbolRowList* rows,
                                                FILE* out) {
    if (!state_path || !rows || !out) return false;
    if (rows->count == 0) {
        fprintf(out, "mods:\n");
        fprintf(out, "rets:\n");
        fprintf(out, "effs:\n");
        fprintf(out, "calls:\n");
        fprintf(out, "syms:\n");
        return true;
    }

    StringList selected_module_refs;
    StringList module_refs;
    StringList module_paths;
    StringList returns;
    StringList effects;
    StringList calls;
    memset(&selected_module_refs, 0, sizeof(selected_module_refs));
    memset(&module_refs, 0, sizeof(module_refs));
    memset(&module_paths, 0, sizeof(module_paths));
    memset(&returns, 0, sizeof(returns));
    memset(&effects, 0, sizeof(effects));
    memset(&calls, 0, sizeof(calls));

    for (int i = 0; i < rows->count; i++) {
        if (!string_list_add_unique(&selected_module_refs, rows->items[i].module_ref)) {
            free_string_list(&selected_module_refs);
            free_string_list(&module_refs);
            free_string_list(&module_paths);
            free_string_list(&returns);
            free_string_list(&effects);
            free_string_list(&calls);
            return false;
        }
    }
    if (!load_state_module_rows_filtered(state_path, &selected_module_refs, &module_refs, &module_paths) ||
        !collect_symbol_tables_from_rows(rows, &returns, &effects, &calls, NULL)) {
        free_string_list(&selected_module_refs);
        free_string_list(&module_refs);
        free_string_list(&module_paths);
        free_string_list(&returns);
        free_string_list(&effects);
        free_string_list(&calls);
        return false;
    }

    fprintf(out, "mods:\n");
    for (int i = 0; i < module_refs.count; i++) {
        fprintf(out, "  %s %s\n", module_refs.items[i], module_paths.items[i]);
    }
    if (!emit_compact_symbol_refs(out, "rets", 'r', &returns) ||
        !emit_compact_symbol_refs(out, "effs", 'e', &effects) ||
        !emit_compact_symbol_refs(out, "calls", 'c', &calls)) {
        free_string_list(&selected_module_refs);
        free_string_list(&module_refs);
        free_string_list(&module_paths);
        free_string_list(&returns);
        free_string_list(&effects);
        free_string_list(&calls);
        return false;
    }

    fprintf(out, "syms:\n");
    for (int i = 0; i < rows->count; i++) {
        if (!emit_compact_symbol_row_from_state(out, &rows->items[i], &returns, &effects, &calls)) {
            free_string_list(&selected_module_refs);
            free_string_list(&module_refs);
            free_string_list(&module_paths);
            free_string_list(&returns);
            free_string_list(&effects);
            free_string_list(&calls);
            return false;
        }
    }

    free_string_list(&selected_module_refs);
    free_string_list(&module_refs);
    free_string_list(&module_paths);
    free_string_list(&returns);
    free_string_list(&effects);
    free_string_list(&calls);
    return true;
}

static bool collect_state_linked_test_candidates(const char* state_path, const StateSymbolRowList* selected_rows,
                                                 StatePlanTestCandidateList* out) {
    if (!state_path || !selected_rows || !out) return false;
    memset(out, 0, sizeof(*out));
    if (selected_rows->count == 0) return true;

    FILE* file = fopen(state_path, "r");
    if (!file) return false;

    char line[8192];
    bool in_tests = false;
    bool ok = true;
    while (fgets(line, sizeof(line), file)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';

        if (!in_tests) {
            if (strcmp(line, "test_index:") == 0) in_tests = true;
            continue;
        }
        if (strcmp(line, "context:") == 0) break;
        if (line[0] == '\0') continue;

        char path[MAX_PATH_LEN];
        StringList covered_ids;
        StringList matched_names;
        memset(&covered_ids, 0, sizeof(covered_ids));
        memset(&matched_names, 0, sizeof(matched_names));
        if (!parse_test_index_row(line, path, sizeof(path), &covered_ids)) {
            free_string_list(&covered_ids);
            ok = false;
            break;
        }

        int hits = 0;
        for (int i = 0; i < covered_ids.count; i++) {
            int row_index = state_symbol_rows_index_of(selected_rows, covered_ids.items[i]);
            char row_name[MAX_NAME_LEN];
            if (row_index == -1 ||
                !state_symbol_row_name(&selected_rows->items[row_index], row_name, sizeof(row_name))) {
                continue;
            }
            hits++;
            if (!string_list_add_unique(&matched_names, row_name)) {
                ok = false;
                break;
            }
        }
        free_string_list(&covered_ids);
        if (!ok) {
            free_string_list(&matched_names);
            break;
        }
        if (hits == 0) {
            free_string_list(&matched_names);
            continue;
        }

        if (!ensure_capacity((void**)&out->items, &out->cap, out->count + 1,
                             sizeof(StatePlanTestCandidate))) {
            free_string_list(&matched_names);
            ok = false;
            break;
        }

        StatePlanTestCandidate* candidate = &out->items[out->count++];
        memset(candidate, 0, sizeof(*candidate));
        copy_text(candidate->path, sizeof(candidate->path), path);
        candidate->hits = hits;
        candidate->symbols = matched_names;
    }
    fclose(file);

    if (ok) {
        for (int i = 0; i < out->count - 1; i++) {
            for (int j = i + 1; j < out->count; j++) {
                if (out->items[i].hits < out->items[j].hits ||
                    (out->items[i].hits == out->items[j].hits &&
                     strcmp(out->items[i].path, out->items[j].path) > 0)) {
                    StatePlanTestCandidate tmp = out->items[i];
                    out->items[i] = out->items[j];
                    out->items[j] = tmp;
                }
            }
        }
    }

    if (!ok || !in_tests) {
        free_state_plan_test_candidates(out);
        return false;
    }
    return true;
}

static bool emit_state_linked_tests(const char* state_path, const StateSymbolRowList* selected_rows,
                                    FILE* out) {
    if (!state_path || !selected_rows || !out || selected_rows->count == 0) return true;

    StatePlanTestCandidateList candidates;
    if (!collect_state_linked_test_candidates(state_path, selected_rows, &candidates)) return false;

    if (candidates.count > 0) {
        fprintf(out, "tests:\n");
        for (int i = 0; i < candidates.count; i++) {
            fprintf(out, "  %d|test=%s|hits=%d|symbols=",
                    i + 1, candidates.items[i].path, candidates.items[i].hits);
            emit_string_list_csv(out, &candidates.items[i].symbols);
            fprintf(out, "\n");
        }
    }

    free_state_plan_test_candidates(&candidates);
    return true;
}

static bool emit_state_module_section_from_rows(const char* state_path, const StateSymbolRowList* rows,
                                                FILE* out) {
    if (!state_path || !rows || !out) return false;
    fprintf(out, "mods:\n");
    if (rows->count == 0) return true;

    StringList selected_module_refs;
    StringList module_refs;
    StringList module_paths;
    memset(&selected_module_refs, 0, sizeof(selected_module_refs));
    memset(&module_refs, 0, sizeof(module_refs));
    memset(&module_paths, 0, sizeof(module_paths));

    bool ok = true;
    for (int i = 0; i < rows->count && ok; i++) {
        ok = string_list_add_unique(&selected_module_refs, rows->items[i].module_ref);
    }
    if (ok) {
        ok = load_state_module_rows_filtered(state_path, &selected_module_refs, &module_refs, &module_paths);
    }
    if (ok) {
        for (int i = 0; i < module_refs.count; i++) {
            fprintf(out, "  %s %s\n", module_refs.items[i], module_paths.items[i]);
        }
    }

    free_string_list(&selected_module_refs);
    free_string_list(&module_refs);
    free_string_list(&module_paths);
    return ok;
}

static bool emit_state_brief_rows(FILE* out, const StateSymbolRowList* rows) {
    if (!out || !rows) return false;
    fprintf(out, "brief_syms:\n");
    for (int i = 0; i < rows->count; i++) {
        CompactSymbolRow row;
        if (!parse_compact_symbol_row(rows->items[i].kind[0] == 'T' ? "target" : "caller",
                                      rows->items[i].contract, &row)) {
            return false;
        }

        if (!row.parsed) {
            char name[MAX_NAME_LEN];
            if (!state_symbol_row_name(&rows->items[i], name, sizeof(name))) {
                copy_text(name, sizeof(name), rows->items[i].contract);
            }
            fprintf(out, "  %d|%s|%s|%s|%s|-|-|-|0|raw=%s\n",
                    i + 1, rows->items[i].symbol_id, rows->items[i].kind,
                    rows->items[i].module_ref, name, row.raw_contract);
            free_compact_symbol_row(&row);
            continue;
        }

        StringList effects;
        memset(&effects, 0, sizeof(effects));
        bool ok = true;
        for (int j = 0; j < row.declared_effects.count && ok; j++) {
            ok = string_list_add_unique(&effects, row.declared_effects.items[j]);
        }
        for (int j = 0; j < row.inferred_effects.count && ok; j++) {
            ok = string_list_add_unique(&effects, row.inferred_effects.items[j]);
        }
        if (!ok) {
            free_string_list(&effects);
            free_compact_symbol_row(&row);
            return false;
        }

        fprintf(out, "  %d|%s|%s|%s|%s|%s|%c|",
                i + 1, rows->items[i].symbol_id, rows->items[i].kind, rows->items[i].module_ref,
                row.signature[0] != '\0' ? row.signature : row.name,
                row.return_type[0] != '\0' ? row.return_type : "-",
                row.is_public ? '+' : '-');
        if (effects.count > 0) {
            emit_string_list_csv(out, &effects);
        } else {
            fprintf(out, "-");
        }
        fprintf(out, "|%d\n", row.calls.count);

        free_string_list(&effects);
        free_compact_symbol_row(&row);
    }
    return true;
}

static long measure_state_payload_bytes(const char* state_path, const StateSymbolRowList* rows,
                                        bool brief_output, bool include_tests) {
    if (!state_path) return -1;
    static unsigned long ledger_tmp_counter = 0;
    char tmp_path[MAX_PATH_LEN];
    snprintf(tmp_path, sizeof(tmp_path), "/tmp/viper-ledger-%ld-%lu.tmp",
             (long)getpid(), ++ledger_tmp_counter);
    FILE* tmp = fopen(tmp_path, "w+");
    if (!tmp) return -1;
    unlink(tmp_path);

    bool ok = true;
    if (!brief_output) {
        ok = rows ? emit_resume_symbol_ledger_from_rows(state_path, rows, tmp)
                  : emit_resume_symbol_ledger(state_path, tmp);
    } else {
        if (!rows) {
            fclose(tmp);
            return -1;
        }
        ok = emit_state_module_section_from_rows(state_path, rows, tmp) &&
             emit_state_brief_rows(tmp, rows);
    }
    if (ok && include_tests && rows) {
        ok = emit_state_linked_tests(state_path, rows, tmp);
    }
    if (!ok) {
        fclose(tmp);
        return -1;
    }
    if (fflush(tmp) != 0) {
        fclose(tmp);
        return -1;
    }
    long bytes = ftell(tmp);
    fclose(tmp);
    return bytes;
}

static bool compute_state_payload_metrics(const char* state_path, const StateSymbolRowList* rows,
                                          bool include_tests, StatePayloadMetrics* metrics) {
    if (!state_path || !rows || !metrics) return false;

    memset(metrics, 0, sizeof(*metrics));

    unsigned long long start_us = monotonic_time_us();
    metrics->brief_bytes = measure_state_payload_bytes(state_path, rows, true, include_tests);
    metrics->brief_emit_us = monotonic_time_us() - start_us;

    start_us = monotonic_time_us();
    metrics->full_bytes = measure_state_payload_bytes(state_path, rows, false, include_tests);
    metrics->full_emit_us = monotonic_time_us() - start_us;

    if (metrics->brief_bytes < 0 || metrics->full_bytes < 0) return false;

    metrics->saved_bytes = metrics->full_bytes - metrics->brief_bytes;
    metrics->saved_pct = metrics->full_bytes > 0
        ? (metrics->saved_bytes * 100) / metrics->full_bytes
        : 0;
    metrics->brief_tokens_est = estimate_payload_tokens(metrics->brief_bytes);
    metrics->full_tokens_est = estimate_payload_tokens(metrics->full_bytes);
    metrics->saved_tokens_est = metrics->full_tokens_est - metrics->brief_tokens_est;

    if (metrics->saved_bytes < 0) metrics->saved_bytes = 0;
    if (metrics->saved_pct < 0) metrics->saved_pct = 0;
    if (metrics->saved_tokens_est < 0) metrics->saved_tokens_est = 0;
    return true;
}

static void emit_ledger_metrics(FILE* out, const char* state_path,
                                const StateSymbolRowList* rows, bool include_tests) {
    if (!out || !state_path || !rows) return;

    StatePayloadMetrics metrics;
    if (!compute_state_payload_metrics(state_path, rows, include_tests, &metrics)) return;

    fprintf(out, "ledger_bytes: %ld\n", metrics.brief_bytes);
    fprintf(out, "ledger_full_bytes: %ld\n", metrics.full_bytes);
    fprintf(out, "ledger_saved_bytes: %ld\n", metrics.saved_bytes);
    fprintf(out, "ledger_saved_pct: %ld\n", metrics.saved_pct);
}

static int count_unique_state_modules(const StateSymbolRowList* rows) {
    if (!rows) return 0;

    StringList refs;
    memset(&refs, 0, sizeof(refs));
    bool ok = true;
    for (int i = 0; i < rows->count && ok; i++) {
        ok = string_list_add_unique(&refs, rows->items[i].module_ref);
    }

    int count = ok ? refs.count : 0;
    free_string_list(&refs);
    return count;
}

static bool collect_symbol_tables_from_rows(const StateSymbolRowList* rows,
                                            StringList* returns, StringList* effects,
                                            StringList* calls, int* symbol_count) {
    if (!rows || !returns || !effects || !calls) return false;
    int collected = 0;
    for (int i = 0; i < rows->count; i++) {
        CompactSymbolRow row;
        if (!parse_compact_symbol_row(rows->items[i].kind[0] == 'T' ? "target" : "caller",
                                      rows->items[i].contract, &row)) {
            return false;
        }
        if (row.parsed) {
            collected++;
            if (row.return_type[0] != '\0' && !string_list_add_unique(returns, row.return_type)) {
                free_compact_symbol_row(&row);
                return false;
            }
            for (int j = 0; j < row.declared_effects.count; j++) {
                if (!string_list_add_unique(effects, row.declared_effects.items[j])) {
                    free_compact_symbol_row(&row);
                    return false;
                }
            }
            for (int j = 0; j < row.inferred_effects.count; j++) {
                if (!string_list_add_unique(effects, row.inferred_effects.items[j])) {
                    free_compact_symbol_row(&row);
                    return false;
                }
            }
            for (int j = 0; j < row.calls.count; j++) {
                if (!string_list_add_unique(calls, row.calls.items[j])) {
                    free_compact_symbol_row(&row);
                    return false;
                }
            }
        }
        free_compact_symbol_row(&row);
    }
    if (symbol_count) *symbol_count = collected;
    return true;
}

static bool emit_compact_symbol_row_from_state(FILE* out, const StateSymbolRow* state_row,
                                               const StringList* returns, const StringList* effects,
                                               const StringList* calls) {
    if (!out || !state_row || !returns || !effects || !calls) return false;

    CompactSymbolRow row;
    if (!parse_compact_symbol_row(state_row->kind[0] == 'T' ? "target" : "caller",
                                  state_row->contract, &row)) {
        return false;
    }

    if (!row.parsed) {
        fprintf(out, "  %s|%s|%s|raw|%s\n",
                state_row->symbol_id, state_row->kind, state_row->module_ref, row.raw_contract);
        free_compact_symbol_row(&row);
        return true;
    }

    fprintf(out, "  %s|%s|%s|%c|%s|", state_row->symbol_id, state_row->kind, state_row->module_ref,
            row.is_public ? '+' : '-',
            row.signature[0] != '\0' ? row.signature : row.name);
    if (row.return_type[0] == '\0') {
        fprintf(out, "-|");
    } else {
        int return_index = string_list_index_of(returns, row.return_type);
        if (return_index == -1) {
            free_compact_symbol_row(&row);
            return false;
        }
        fprintf(out, "r%d|", return_index + 1);
    }
    if (!emit_compact_symbol_id_list(out, 'e', &row.declared_effects, effects) ||
        fprintf(out, "|") < 0 ||
        !emit_compact_symbol_id_list(out, 'e', &row.inferred_effects, effects) ||
        fprintf(out, "|") < 0 ||
        !emit_compact_symbol_id_list(out, 'c', &row.calls, calls)) {
        free_compact_symbol_row(&row);
        return false;
    }
    fprintf(out, "\n");
    free_compact_symbol_row(&row);
    return true;
}

static bool emit_patch_symbol_ledger(const char* old_state_path, const char* new_state_path,
                                     const StringList* module_refs, const StringList* module_paths,
                                     FILE* out, int* patched_symbols) {
    if (!old_state_path || !new_state_path || !module_refs || !module_paths || !out) return false;
    if (patched_symbols) *patched_symbols = 0;
    if (module_refs->count == 0) return true;

    StateSymbolRowList before_rows;
    StateSymbolRowList after_rows;
    StateSymbolRowList changed_rows;
    StringList removed_symbol_ids;
    StringList returns;
    StringList effects;
    StringList calls;
    memset(&before_rows, 0, sizeof(before_rows));
    memset(&after_rows, 0, sizeof(after_rows));
    memset(&changed_rows, 0, sizeof(changed_rows));
    memset(&removed_symbol_ids, 0, sizeof(removed_symbol_ids));
    memset(&returns, 0, sizeof(returns));
    memset(&effects, 0, sizeof(effects));
    memset(&calls, 0, sizeof(calls));

    if (!load_state_symbol_rows_filtered(old_state_path, module_refs, &before_rows) ||
        !load_state_symbol_rows_filtered(new_state_path, module_refs, &after_rows)) {
        free_state_symbol_rows(&before_rows);
        free_state_symbol_rows(&after_rows);
        free_string_list(&returns);
        free_string_list(&effects);
        free_string_list(&calls);
        return false;
    }

    for (int i = 0; i < after_rows.count; i++) {
        int old_index = state_symbol_rows_index_of(&before_rows, after_rows.items[i].symbol_id);
        if (old_index != -1 && state_symbol_row_equals(&before_rows.items[old_index], &after_rows.items[i])) {
            continue;
        }
        if (!state_symbol_rows_add(&changed_rows, after_rows.items[i].symbol_id,
                                   after_rows.items[i].module_ref, after_rows.items[i].kind,
                                   after_rows.items[i].contract)) {
            free_state_symbol_rows(&before_rows);
            free_state_symbol_rows(&after_rows);
            free_state_symbol_rows(&changed_rows);
            free_string_list(&removed_symbol_ids);
            free_string_list(&returns);
            free_string_list(&effects);
            free_string_list(&calls);
            return false;
        }
    }
    for (int i = 0; i < before_rows.count; i++) {
        if (state_symbol_rows_index_of(&after_rows, before_rows.items[i].symbol_id) != -1) continue;
        if (!string_list_append(&removed_symbol_ids, before_rows.items[i].symbol_id)) {
            free_state_symbol_rows(&before_rows);
            free_state_symbol_rows(&after_rows);
            free_state_symbol_rows(&changed_rows);
            free_string_list(&removed_symbol_ids);
            free_string_list(&returns);
            free_string_list(&effects);
            free_string_list(&calls);
            return false;
        }
    }

    int symbol_count = 0;
    if (!collect_symbol_tables_from_rows(&changed_rows, &returns, &effects, &calls, &symbol_count)) {
        free_state_symbol_rows(&before_rows);
        free_state_symbol_rows(&after_rows);
        free_state_symbol_rows(&changed_rows);
        free_string_list(&removed_symbol_ids);
        free_string_list(&returns);
        free_string_list(&effects);
        free_string_list(&calls);
        return false;
    }

    fprintf(out, "patch_mods:\n");
    for (int i = 0; i < module_refs->count; i++) {
        fprintf(out, "  %s %s\n", module_refs->items[i], module_paths->items[i]);
    }
    if (removed_symbol_ids.count > 0) {
        fprintf(out, "patch_removed_syms:\n");
        for (int i = 0; i < removed_symbol_ids.count; i++) {
            fprintf(out, "  %s\n", removed_symbol_ids.items[i]);
        }
    }
    if (changed_rows.count > 0) {
        if (!emit_compact_symbol_refs(out, "patch_rets", 'r', &returns) ||
            !emit_compact_symbol_refs(out, "patch_effs", 'e', &effects) ||
            !emit_compact_symbol_refs(out, "patch_calls", 'c', &calls)) {
            free_state_symbol_rows(&before_rows);
            free_state_symbol_rows(&after_rows);
            free_state_symbol_rows(&changed_rows);
            free_string_list(&removed_symbol_ids);
            free_string_list(&returns);
            free_string_list(&effects);
            free_string_list(&calls);
            return false;
        }
        fprintf(out, "patch_syms:\n");
        for (int i = 0; i < changed_rows.count; i++) {
            if (!emit_compact_symbol_row_from_state(out, &changed_rows.items[i],
                                                    &returns, &effects, &calls)) {
                free_state_symbol_rows(&before_rows);
                free_state_symbol_rows(&after_rows);
                free_state_symbol_rows(&changed_rows);
                free_string_list(&removed_symbol_ids);
                free_string_list(&returns);
                free_string_list(&effects);
                free_string_list(&calls);
                return false;
            }
        }
    }

    int total_patch_ops = symbol_count + removed_symbol_ids.count;
    free_state_symbol_rows(&before_rows);
    free_state_symbol_rows(&after_rows);
    free_state_symbol_rows(&changed_rows);
    free_string_list(&removed_symbol_ids);
    free_string_list(&returns);
    free_string_list(&effects);
    free_string_list(&calls);
    if (patched_symbols) *patched_symbols = total_patch_ops;
    return true;
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
        char symbol_identity[4608];
        if (!build_symbol_identity_source(kind, module_path, contract,
                                          symbol_identity, sizeof(symbol_identity))) {
            free_string_list(&modules);
            free_string_list(&module_refs);
            free_string_list(&symbol_refs);
            return false;
        }
        char symbol_ref[32];
        if (!build_stable_ref("s", symbol_identity, &symbol_refs, symbol_ref, sizeof(symbol_ref)) ||
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

static bool collect_symbol_rows_from_semantic_items(const SemanticItemList* items,
                                                    StateSymbolRowList* rows) {
    if (!items || !rows) return false;
    memset(rows, 0, sizeof(*rows));

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
                                      contract, sizeof(contract)) ||
            !string_list_add_unique(&modules, module_path)) {
            free_string_list(&modules);
            free_string_list(&module_refs);
            free_string_list(&symbol_refs);
            free_state_symbol_rows(rows);
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
            free_state_symbol_rows(rows);
            return false;
        }
    }

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
            free_state_symbol_rows(rows);
            return false;
        }
        int module_index = string_list_index_of(&modules, module_path);
        if (module_index == -1) {
            free_string_list(&modules);
            free_string_list(&module_refs);
            free_string_list(&symbol_refs);
            free_state_symbol_rows(rows);
            return false;
        }

        char symbol_identity[4608];
        char symbol_ref[32];
        if (!build_symbol_identity_source(kind, module_path, contract,
                                          symbol_identity, sizeof(symbol_identity)) ||
            !build_stable_ref("s", symbol_identity, &symbol_refs, symbol_ref, sizeof(symbol_ref)) ||
            !string_list_append(&symbol_refs, symbol_ref) ||
            !state_symbol_rows_add(rows, symbol_ref, module_refs.items[module_index],
                                   strcmp(kind, "target") == 0 ? "T" : "C", contract)) {
            free_string_list(&modules);
            free_string_list(&module_refs);
            free_string_list(&symbol_refs);
            free_state_symbol_rows(rows);
            return false;
        }
    }

    free_string_list(&modules);
    free_string_list(&module_refs);
    free_string_list(&symbol_refs);
    return true;
}

static bool collect_module_rows_from_semantic_items(const SemanticItemList* items,
                                                    StringList* module_refs, StringList* module_paths) {
    if (!items || !module_refs || !module_paths) return false;
    memset(module_refs, 0, sizeof(*module_refs));
    memset(module_paths, 0, sizeof(*module_paths));

    for (int i = 0; i < items->count; i++) {
        char kind[16];
        char module_path[MAX_PATH_LEN];
        char contract[4608];
        if (!parse_semantic_item_text(items->items[i].text, kind, sizeof(kind),
                                      module_path, sizeof(module_path),
                                      contract, sizeof(contract)) ||
            !string_list_add_unique(module_paths, module_path)) {
            free_string_list(module_refs);
            free_string_list(module_paths);
            return false;
        }
    }

    for (int i = 0; i < module_paths->count; i++) {
        char module_ref[32];
        if (!build_stable_ref("m", module_paths->items[i], module_refs, module_ref, sizeof(module_ref)) ||
            !string_list_append(module_refs, module_ref)) {
            free_string_list(module_refs);
            free_string_list(module_paths);
            return false;
        }
    }

    return true;
}

static bool module_token_matches(const char* module_path, const char* token) {
    if (!module_path || !token || token[0] == '\0') return false;
    if (strcmp(module_path, token) == 0) return true;
    const char* slash = strrchr(module_path, '/');
    const char* basename = slash ? slash + 1 : module_path;
    return strcmp(basename, token) == 0;
}

static bool state_test_links_add(StateTestLinkList* links, const char* path, const char* hash_hex,
                                 const StringList* covered_symbol_ids) {
    if (!links || !path || !hash_hex || !covered_symbol_ids) return false;
    if (!ensure_capacity((void**)&links->items, &links->cap, links->count + 1, sizeof(StateTestLink))) {
        return false;
    }

    StateTestLink* item = &links->items[links->count++];
    memset(item, 0, sizeof(*item));
    copy_text(item->path, sizeof(item->path), path);
    copy_text(item->hash_hex, sizeof(item->hash_hex), hash_hex);
    for (int i = 0; i < covered_symbol_ids->count; i++) {
        if (!string_list_add_unique(&item->covered_symbol_ids, covered_symbol_ids->items[i])) {
            free_state_test_links(links);
            return false;
        }
    }
    return true;
}

static bool collect_state_test_links(const char* project_root, const SemanticItemList* items,
                                     StateTestLinkList* out) {
    if (!project_root || !items || !out) return false;
    memset(out, 0, sizeof(*out));

    StringList test_files;
    StringList module_refs;
    StringList module_paths;
    StateSymbolRowList rows;
    memset(&test_files, 0, sizeof(test_files));
    memset(&module_refs, 0, sizeof(module_refs));
    memset(&module_paths, 0, sizeof(module_paths));
    memset(&rows, 0, sizeof(rows));
    bool ok = collect_candidate_test_files(project_root, &test_files) &&
              collect_module_rows_from_semantic_items(items, &module_refs, &module_paths) &&
              collect_symbol_rows_from_semantic_items(items, &rows);
    if (!ok) {
        free_string_list(&test_files);
        free_string_list(&module_refs);
        free_string_list(&module_paths);
        free_state_symbol_rows(&rows);
        return false;
    }

    for (int i = 0; i < test_files.count; i++) {
        char* source = read_file(test_files.items[i]);
        StringList covered_symbol_ids;
        memset(&covered_symbol_ids, 0, sizeof(covered_symbol_ids));
        if (!source) continue;
        if (strstr(source, "@covers") == NULL) {
            free(source);
            continue;
        }

        char* cursor = source;
        while (*cursor != '\0') {
            char* line_end = strchr(cursor, '\n');
            size_t len = line_end ? (size_t)(line_end - cursor) : strlen(cursor);
            if (len > 0) {
                char line_buf[2048];
                if (len >= sizeof(line_buf)) len = sizeof(line_buf) - 1;
                memcpy(line_buf, cursor, len);
                line_buf[len] = '\0';
                char* marker = strstr(line_buf, "@covers");
                if (marker) {
                    char* token = strtok(marker + 7, " \t");
                    if (token) {
                        char module_token[MAX_PATH_LEN];
                        copy_text(module_token, sizeof(module_token), token);
                        while ((token = strtok(NULL, " \t")) != NULL) {
                            for (int row_index = 0; row_index < rows.count; row_index++) {
                                int module_index = string_list_index_of(&module_refs, rows.items[row_index].module_ref);
                                char row_name[MAX_NAME_LEN];
                                if (module_index == -1 ||
                                    !module_token_matches(module_paths.items[module_index], module_token) ||
                                    !state_symbol_row_name(&rows.items[row_index], row_name, sizeof(row_name)) ||
                                    strcmp(row_name, token) != 0) {
                                    continue;
                                }
                                if (!string_list_add_unique(&covered_symbol_ids, rows.items[row_index].symbol_id)) {
                                    ok = false;
                                    break;
                                }
                            }
                            if (!ok) break;
                        }
                    }
                }
            }
            if (!ok || !line_end) break;
            cursor = line_end + 1;
        }

        if (ok && covered_symbol_ids.count > 0) {
            char hash_hex[65];
            char display_path[MAX_PATH_LEN];
            if (!compute_module_hash_hex(test_files.items[i], hash_hex) ||
                !format_pack_path(test_files.items[i], display_path, sizeof(display_path)) ||
                !state_test_links_add(out, display_path, hash_hex, &covered_symbol_ids)) {
                ok = false;
            }
        }
        free_string_list(&covered_symbol_ids);
        free(source);
        if (!ok) break;
    }

    free_string_list(&test_files);
    free_string_list(&module_refs);
    free_string_list(&module_paths);
    free_state_symbol_rows(&rows);
    if (!ok) {
        free_state_test_links(out);
        return false;
    }
    return true;
}

static bool emit_state_test_sections(FILE* out, const StateTestLinkList* links) {
    if (!out || !links) return false;
    fprintf(out, "tracked_tests:\n");
    for (int i = 0; i < links->count; i++) {
        fprintf(out, "  %s sha256=%s\n", links->items[i].path, links->items[i].hash_hex);
    }
    fprintf(out, "test_index:\n");
    for (int i = 0; i < links->count; i++) {
        fprintf(out, "  %s covers=", links->items[i].path);
        emit_string_list_csv(out, &links->items[i].covered_symbol_ids);
        fprintf(out, "\n");
    }
    return true;
}

static bool build_diff_symbol_identity(const char* kind, const char* contract,
                                       char* out, size_t out_size) {
    if (!kind || !contract || !out || out_size == 0) return false;

    if (strncmp(contract, "var ", 4) == 0) {
        const char* start = contract + 4;
        const char* end = strchr(start, ':');
        size_t len = end ? (size_t)(end - start) : strlen(start);
        char name[MAX_NAME_LEN];
        if (len == 0 || len >= sizeof(name)) return false;
        memcpy(name, start, len);
        name[len] = '\0';
        return snprintf(out, out_size, "%s:var:%s", kind, name) < (int)out_size;
    }

    if (strncmp(contract, "struct ", 7) == 0) {
        const char* start = contract + 7;
        if (strncmp(start, "pub ", 4) == 0) start += 4;
        const char* end = strchr(start, '(');
        size_t len = end ? (size_t)(end - start) : strlen(start);
        char name[MAX_NAME_LEN];
        if (len == 0 || len >= sizeof(name)) return false;
        memcpy(name, start, len);
        name[len] = '\0';
        return snprintf(out, out_size, "%s:struct:%s", kind, name) < (int)out_size;
    }

    CompactSymbolRow row;
    if (!parse_compact_symbol_row(kind, contract, &row)) return false;
    bool ok = row.parsed &&
              snprintf(out, out_size, "%s:fn:%s/%d", kind, row.name, row.arity) < (int)out_size;
    free_compact_symbol_row(&row);
    return ok;
}

static bool state_symbol_row_diff_identity(const StateSymbolRow* row, char* out, size_t out_size) {
    if (!row || !out || out_size == 0) return false;
    return build_diff_symbol_identity(row->kind[0] == 'T' ? "target" : "caller",
                                      row->contract, out, out_size);
}

static int state_symbol_rows_index_of_diff_identity(const StateSymbolRowList* rows,
                                                    const char* diff_identity) {
    if (!rows || !diff_identity) return -1;
    for (int i = 0; i < rows->count; i++) {
        char current[256];
        if (!state_symbol_row_diff_identity(&rows->items[i], current, sizeof(current))) continue;
        if (strcmp(current, diff_identity) == 0) return i;
    }
    return -1;
}

static bool build_module_symbol_identity(const char* module_path, const char* contract,
                                         char* out, size_t out_size) {
    if (!module_path || !contract || !out || out_size == 0) return false;

    if (strncmp(contract, "var ", 4) == 0) {
        const char* start = contract + 4;
        const char* end = strchr(start, ':');
        size_t len = end ? (size_t)(end - start) : strlen(start);
        char name[MAX_NAME_LEN];
        if (len == 0 || len >= sizeof(name)) return false;
        memcpy(name, start, len);
        name[len] = '\0';
        return snprintf(out, out_size, "%s::var:%s", module_path, name) < (int)out_size;
    }

    if (strncmp(contract, "struct ", 7) == 0) {
        const char* start = contract + 7;
        if (strncmp(start, "pub ", 4) == 0) start += 4;
        const char* end = strchr(start, '(');
        size_t len = end ? (size_t)(end - start) : strlen(start);
        char name[MAX_NAME_LEN];
        if (len == 0 || len >= sizeof(name)) return false;
        memcpy(name, start, len);
        name[len] = '\0';
        return snprintf(out, out_size, "%s::struct:%s", module_path, name) < (int)out_size;
    }

    CompactSymbolRow row;
    if (!parse_compact_symbol_row("target", contract, &row)) return false;
    bool ok = row.parsed &&
              snprintf(out, out_size, "%s::fn:%s/%d", module_path, row.name, row.arity) < (int)out_size;
    free_compact_symbol_row(&row);
    return ok;
}

static bool collect_project_symbol_identities(const ProjectIndex* project, StringList* out) {
    if (!project || !out) return false;
    for (int i = 0; i < project->module_count; i++) {
        char canonical_path[MAX_PATH_LEN];
        char module_path[MAX_PATH_LEN];
        if (!canonicalize_existing_path(project->modules[i].path, canonical_path, sizeof(canonical_path)) ||
            !format_pack_path(canonical_path, module_path, sizeof(module_path))) {
            return false;
        }

        for (int j = 0; j < project->modules[i].variable_count; j++) {
            char contract[512];
            char identity[1536];
            if (!format_var_contract(&project->modules[i].variables[j], contract, sizeof(contract)) ||
                !build_module_symbol_identity(module_path, contract, identity, sizeof(identity)) ||
                !string_list_append_unique(out, identity)) {
                return false;
            }
        }
        for (int j = 0; j < project->modules[i].struct_count; j++) {
            char contract[1024];
            char identity[2048];
            if (!format_struct_contract(&project->modules[i].structs[j], contract, sizeof(contract)) ||
                !build_module_symbol_identity(module_path, contract, identity, sizeof(identity)) ||
                !string_list_append_unique(out, identity)) {
                return false;
            }
        }
        for (int j = 0; j < project->modules[i].function_count; j++) {
            char contract[4096];
            char identity[4608];
            if (!format_fn_contract(&project->modules[i].functions[j], contract, sizeof(contract)) ||
                !build_module_symbol_identity(module_path, contract, identity, sizeof(identity)) ||
                !string_list_append_unique(out, identity)) {
                return false;
            }
        }
    }
    return true;
}

static bool collect_vp_files_in_dir(const char* dir_path, StringList* out) {
    if (!dir_path || !out) return false;

    DIR* dir = opendir(dir_path);
    if (!dir) return true;

    struct dirent* entry;
    bool ok = true;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        if (!has_suffix(entry->d_name, ".vp")) continue;

        char full_path[MAX_PATH_LEN];
        struct stat st;
        if (!join_path(dir_path, entry->d_name, full_path, sizeof(full_path)) ||
            stat(full_path, &st) != 0 || !S_ISREG(st.st_mode) ||
            !string_list_append_unique(out, full_path)) {
            ok = false;
            break;
        }
    }

    closedir(dir);
    return ok;
}

static bool collect_candidate_test_files(const char* project_root, StringList* out) {
    if (!project_root || !out) return false;

    char scripts_dir[MAX_PATH_LEN];
    char tests_dir[MAX_PATH_LEN];
    if (!join_path(project_root, "tests", tests_dir, sizeof(tests_dir))) return false;
    if (!join_path(tests_dir, "scripts", scripts_dir, sizeof(scripts_dir))) return false;
    if (!collect_vp_files_in_dir(scripts_dir, out)) return false;
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

static bool canonicalize_existing_path(const char* path, char* out, size_t out_size) {
    if (!path || !out || out_size == 0) return false;
    char temp[MAX_PATH_LEN];
    char* segments[128];
    int segment_count = 0;
    bool absolute = path[0] == '/';

    copy_text(temp, sizeof(temp), path);
    char* cursor = temp;
    if (absolute && *cursor == '/') cursor++;

    while (*cursor) {
        char* next = strchr(cursor, '/');
        if (next) *next = '\0';

        if (cursor[0] != '\0' && strcmp(cursor, ".") != 0) {
            if (strcmp(cursor, "..") == 0) {
                if (segment_count > 0) segment_count--;
            } else if (segment_count < (int)(sizeof(segments) / sizeof(segments[0]))) {
                segments[segment_count++] = cursor;
            } else {
                return false;
            }
        }

        if (!next) break;
        cursor = next + 1;
    }

    size_t offset = 0;
    if (absolute) {
        if (offset + 1 >= out_size) return false;
        out[offset++] = '/';
    }
    for (int i = 0; i < segment_count; i++) {
        size_t len = strlen(segments[i]);
        if (offset > 0 && out[offset - 1] != '/') {
            if (offset + 1 >= out_size) return false;
            out[offset++] = '/';
        }
        if (offset + len >= out_size) return false;
        memcpy(out + offset, segments[i], len);
        offset += len;
    }
    if (offset == 0) {
        if (out_size < 2) return false;
        out[0] = absolute ? '/' : '.';
        out[1] = '\0';
        return true;
    }
    out[offset] = '\0';
    return true;
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
        indexer_error("VIX021", "Could not resolve entry file path.");
        return false;
    }

    if (!find_project_root_from_entry(entry_path, index_project_root, sizeof(index_project_root))) {
        indexer_error("VIX022", "Could not resolve project root.");
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
        indexer_error("VIX023", "Could not open output file \"%s\".", out_path);
        return NULL;
    }
    return out;
}

bool resume_project_state(const char* state_path, const char* out_path,
                          const char* focus_symbol, bool include_impact,
                          bool brief_output) {
    ProjectStateManifest manifest;
    if (!load_project_state_manifest(state_path, &manifest)) return false;

    StringList stale_lines;
    StringList stale_module_refs;
    StringList stale_module_paths;
    StateSymbolRowList all_rows;
    StateSymbolRowList selected_rows;
    memset(&stale_lines, 0, sizeof(stale_lines));
    memset(&stale_module_refs, 0, sizeof(stale_module_refs));
    memset(&stale_module_paths, 0, sizeof(stale_module_paths));
    memset(&all_rows, 0, sizeof(all_rows));
    memset(&selected_rows, 0, sizeof(selected_rows));
    int stale_count = 0;
    bool ok = check_project_state_manifest(state_path, &manifest, &stale_lines, &stale_count);
    if (!ok) {
        free_string_list(&stale_lines);
        free_string_list(&stale_module_refs);
        free_string_list(&stale_module_paths);
        free_state_symbol_rows(&all_rows);
        free_state_symbol_rows(&selected_rows);
        free_project_state_manifest(&manifest);
        indexer_error("VIX024", "Could not resume from state file \"%s\".", state_path);
        return false;
    }
    if (stale_count > 0 &&
        !collect_stale_module_refs(state_path, &stale_lines, &stale_module_refs, &stale_module_paths)) {
        free_string_list(&stale_lines);
        free_string_list(&stale_module_refs);
        free_string_list(&stale_module_paths);
        free_state_symbol_rows(&all_rows);
        free_state_symbol_rows(&selected_rows);
        free_project_state_manifest(&manifest);
        indexer_error("VIX025", "Could not map stale modules in state file \"%s\".", state_path);
        return false;
    }

    if ((focus_symbol && focus_symbol[0] != '\0') || brief_output) {
        if (!load_state_symbol_rows_filtered(state_path, NULL, &all_rows)) {
            free_string_list(&stale_lines);
            free_string_list(&stale_module_refs);
            free_string_list(&stale_module_paths);
            free_state_symbol_rows(&all_rows);
            free_state_symbol_rows(&selected_rows);
            free_project_state_manifest(&manifest);
            indexer_error("VIX026", "Could not load symbol ledger for \"%s\".", state_path);
            return false;
        }
    }
    if (focus_symbol && focus_symbol[0] != '\0') {
        if (!select_resume_symbol_rows(&all_rows, focus_symbol, include_impact, &selected_rows)) {
            free_string_list(&stale_lines);
            free_string_list(&stale_module_refs);
            free_string_list(&stale_module_paths);
            free_state_symbol_rows(&all_rows);
            free_state_symbol_rows(&selected_rows);
            free_project_state_manifest(&manifest);
            indexer_error("VIX027", "Could not focus resume state on \"%s\".", focus_symbol);
            return false;
        }
    } else if (brief_output) {
        selected_rows = all_rows;
        memset(&all_rows, 0, sizeof(all_rows));
    }

    FILE* out = open_index_output(out_path);
    if (!out) {
        free_string_list(&stale_lines);
        free_string_list(&stale_module_refs);
        free_string_list(&stale_module_paths);
        free_state_symbol_rows(&selected_rows);
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
        free_state_symbol_rows(&selected_rows);
        free_project_state_manifest(&manifest);
        return false;
    }

    char entry_display[MAX_PATH_LEN];
    format_pack_path(entry_path, entry_display, sizeof(entry_display));

    fprintf(out, "%s\n", (focus_symbol && focus_symbol[0] != '\0') ? "PRESUMEv6" : "PRESUMEv4");
    fprintf(out, "state: %s\n", state_path);
    fprintf(out, "entry: %s\n", entry_display);
    fprintf(out, "focus: %s\n", manifest.focus[0] != '\0' ? manifest.focus : "-");
    fprintf(out, "impact: %s\n", manifest.include_impact ? "yes" : "no");
    fprintf(out, "brief: %s\n", brief_output ? "yes" : "no");
    if (focus_symbol && focus_symbol[0] != '\0') {
        fprintf(out, "resume_focus: %s\n", focus_symbol);
        fprintf(out, "resume_impact: %s\n", include_impact ? "yes" : "no");
    }
    fprintf(out, "valid: %s\n", stale_count == 0 ? "yes" : "no");
    fprintf(out, "changed_files: %d\n", stale_count);
    fprintf(out, "semantic_items: %d\n", manifest.semantic_item_count);
    fprintf(out, "semantic_fingerprint: %s\n",
            manifest.semantic_fingerprint[0] != '\0' ? manifest.semantic_fingerprint : "-");
    if (brief_output) {
        emit_ledger_metrics(out, state_path, &selected_rows,
                            focus_symbol && focus_symbol[0] != '\0');
    }
    if (!emit_state_verification_summary(state_path, &manifest,
                                         &stale_lines, &stale_module_refs, out)) {
        if (out != stdout) fclose(out);
        free_string_list(&stale_lines);
        free_string_list(&stale_module_refs);
        free_string_list(&stale_module_paths);
        free_state_symbol_rows(&all_rows);
        free_state_symbol_rows(&selected_rows);
        free_project_state_manifest(&manifest);
        indexer_error("VIX028", "Could not emit state verification summary for \"%s\".", state_path);
        return false;
    }
    if (stale_count > 0) {
        bool stale_ok = true;
        fprintf(out, "stale_files:\n");
        for (int i = 0; i < stale_lines.count; i++) {
            fprintf(out, "  %s\n", stale_lines.items[i]);
        }
        stale_ok = emit_stale_module_section(out, &stale_module_refs, &stale_module_paths);
        if (stale_ok) {
            if (!focus_symbol || focus_symbol[0] == '\0') {
                stale_ok = emit_stale_symbol_section(state_path, out, &stale_module_refs);
            } else {
                fprintf(out, "stale_symbols:\n");
                for (int i = 0; i < selected_rows.count; i++) {
                    if (string_list_index_of(&stale_module_refs, selected_rows.items[i].module_ref) == -1) {
                        continue;
                    }
                    fprintf(out, "  %s\n", selected_rows.items[i].symbol_id);
                }
            }
        }
        if (!stale_ok) {
            if (out != stdout) fclose(out);
            free_string_list(&stale_lines);
            free_string_list(&stale_module_refs);
            free_string_list(&stale_module_paths);
            free_state_symbol_rows(&all_rows);
            free_state_symbol_rows(&selected_rows);
            free_project_state_manifest(&manifest);
            indexer_error("VIX029", "Could not emit stale symbol map for \"%s\".", state_path);
            return false;
        }
    }
    if (!brief_output) {
        if ((focus_symbol && focus_symbol[0] != '\0')
                ? !emit_resume_symbol_ledger_from_rows(state_path, &selected_rows, out)
                : !emit_resume_symbol_ledger(state_path, out)) {
            if (out != stdout) fclose(out);
            free_string_list(&stale_lines);
            free_string_list(&stale_module_refs);
            free_string_list(&stale_module_paths);
            free_state_symbol_rows(&all_rows);
            free_state_symbol_rows(&selected_rows);
            free_project_state_manifest(&manifest);
            indexer_error("VIX030", "Missing embedded symbol ledger in state file \"%s\".", state_path);
            return false;
        }
    } else {
        if (!emit_state_module_section_from_rows(state_path, &selected_rows, out) ||
            !emit_state_brief_rows(out, &selected_rows)) {
            if (out != stdout) fclose(out);
            free_string_list(&stale_lines);
            free_string_list(&stale_module_refs);
            free_string_list(&stale_module_paths);
            free_state_symbol_rows(&all_rows);
            free_state_symbol_rows(&selected_rows);
            free_project_state_manifest(&manifest);
            indexer_error("VIX031", "Could not emit brief state resume for \"%s\".", state_path);
            return false;
        }
    }
    if (focus_symbol && focus_symbol[0] != '\0' &&
        !emit_state_linked_tests(state_path, &selected_rows, out)) {
        if (out != stdout) fclose(out);
        free_string_list(&stale_lines);
        free_string_list(&stale_module_refs);
        free_string_list(&stale_module_paths);
        free_state_symbol_rows(&all_rows);
        free_state_symbol_rows(&selected_rows);
        free_project_state_manifest(&manifest);
        indexer_error("VIX032", "Could not emit linked tests for \"%s\".", state_path);
        return false;
    }

    if (out != stdout) fclose(out);
    free_string_list(&stale_lines);
    free_string_list(&stale_module_refs);
    free_string_list(&stale_module_paths);
    free_state_symbol_rows(&all_rows);
    free_state_symbol_rows(&selected_rows);
    free_project_state_manifest(&manifest);
    return true;
}

bool query_project_state(const char* state_path, const char* out_path,
                         const char* name_filter, const char* effect_filter,
                         const char* call_filter, bool include_impact,
                         bool include_dependencies, bool brief_output) {
    if ((!name_filter || name_filter[0] == '\0') &&
        (!effect_filter || effect_filter[0] == '\0') &&
        (!call_filter || call_filter[0] == '\0')) {
        indexer_error("VIX033", "State query requires at least one filter.");
        return false;
    }

    ProjectStateManifest manifest;
    if (!load_project_state_manifest(state_path, &manifest)) return false;

    StringList stale_lines;
    StringList stale_module_refs;
    StringList stale_module_paths;
    StateSymbolRowList all_rows;
    StateSymbolRowList selected_rows;
    memset(&stale_lines, 0, sizeof(stale_lines));
    memset(&stale_module_refs, 0, sizeof(stale_module_refs));
    memset(&stale_module_paths, 0, sizeof(stale_module_paths));
    memset(&all_rows, 0, sizeof(all_rows));
    memset(&selected_rows, 0, sizeof(selected_rows));
    int seed_matches = 0;

    int stale_count = 0;
    bool ok = check_project_state_manifest(state_path, &manifest, &stale_lines, &stale_count);
    if (!ok) {
        free_string_list(&stale_lines);
        free_string_list(&stale_module_refs);
        free_string_list(&stale_module_paths);
        free_state_symbol_rows(&all_rows);
        free_state_symbol_rows(&selected_rows);
        free_project_state_manifest(&manifest);
        indexer_error("VIX034", "Could not query state file \"%s\".", state_path);
        return false;
    }
    if (stale_count > 0 &&
        !collect_stale_module_refs(state_path, &stale_lines, &stale_module_refs, &stale_module_paths)) {
        free_string_list(&stale_lines);
        free_string_list(&stale_module_refs);
        free_string_list(&stale_module_paths);
        free_state_symbol_rows(&all_rows);
        free_state_symbol_rows(&selected_rows);
        free_project_state_manifest(&manifest);
        indexer_error("VIX035", "Could not map stale modules in state file \"%s\".", state_path);
        return false;
    }
    if (!load_state_symbol_rows_filtered(state_path, NULL, &all_rows) ||
        !select_query_symbol_rows(&all_rows, name_filter, effect_filter, call_filter,
                                  include_impact, include_dependencies,
                                  &selected_rows, &seed_matches)) {
        free_string_list(&stale_lines);
        free_string_list(&stale_module_refs);
        free_string_list(&stale_module_paths);
        free_state_symbol_rows(&all_rows);
        free_state_symbol_rows(&selected_rows);
        free_project_state_manifest(&manifest);
        indexer_error("VIX036", "Could not query symbol ledger in \"%s\".", state_path);
        return false;
    }

    FILE* out = open_index_output(out_path);
    if (!out) {
        free_string_list(&stale_lines);
        free_string_list(&stale_module_refs);
        free_string_list(&stale_module_paths);
        free_state_symbol_rows(&all_rows);
        free_state_symbol_rows(&selected_rows);
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
        free_state_symbol_rows(&all_rows);
        free_state_symbol_rows(&selected_rows);
        free_project_state_manifest(&manifest);
        return false;
    }

    char entry_display[MAX_PATH_LEN];
    format_pack_path(entry_path, entry_display, sizeof(entry_display));

    fprintf(out, "PQUERYv3\n");
    fprintf(out, "state: %s\n", state_path);
    fprintf(out, "entry: %s\n", entry_display);
    fprintf(out, "focus: %s\n", manifest.focus[0] != '\0' ? manifest.focus : "-");
    fprintf(out, "impact: %s\n", manifest.include_impact ? "yes" : "no");
    fprintf(out, "brief: %s\n", brief_output ? "yes" : "no");
    fprintf(out, "query_name: %s\n", (name_filter && name_filter[0] != '\0') ? name_filter : "-");
    fprintf(out, "query_effect: %s\n", (effect_filter && effect_filter[0] != '\0') ? effect_filter : "-");
    fprintf(out, "query_call: %s\n", (call_filter && call_filter[0] != '\0') ? call_filter : "-");
    fprintf(out, "query_impact: %s\n", include_impact ? "yes" : "no");
    fprintf(out, "query_deps: %s\n", include_dependencies ? "yes" : "no");
    fprintf(out, "valid: %s\n", stale_count == 0 ? "yes" : "no");
    fprintf(out, "changed_files: %d\n", stale_count);
    fprintf(out, "seed_matches: %d\n", seed_matches);
    fprintf(out, "matches: %d\n", selected_rows.count);
    if (brief_output) {
        emit_ledger_metrics(out, state_path, &selected_rows, true);
    }
    if (!emit_state_verification_summary(state_path, &manifest,
                                         &stale_lines, &stale_module_refs, out)) {
        if (out != stdout) fclose(out);
        free_string_list(&stale_lines);
        free_string_list(&stale_module_refs);
        free_string_list(&stale_module_paths);
        free_state_symbol_rows(&all_rows);
        free_state_symbol_rows(&selected_rows);
        free_project_state_manifest(&manifest);
        indexer_error("VIX028", "Could not emit state verification summary for \"%s\".", state_path);
        return false;
    }
    if (stale_count > 0) {
        fprintf(out, "stale_files:\n");
        for (int i = 0; i < stale_lines.count; i++) {
            fprintf(out, "  %s\n", stale_lines.items[i]);
        }
        if (!emit_stale_module_section(out, &stale_module_refs, &stale_module_paths)) {
            if (out != stdout) fclose(out);
            free_string_list(&stale_lines);
            free_string_list(&stale_module_refs);
            free_string_list(&stale_module_paths);
            free_state_symbol_rows(&all_rows);
            free_state_symbol_rows(&selected_rows);
            free_project_state_manifest(&manifest);
            indexer_error("VIX037", "Could not emit stale module map for \"%s\".", state_path);
            return false;
        }
        fprintf(out, "stale_symbols:\n");
        for (int i = 0; i < selected_rows.count; i++) {
            if (string_list_index_of(&stale_module_refs, selected_rows.items[i].module_ref) == -1) continue;
            fprintf(out, "  %s\n", selected_rows.items[i].symbol_id);
        }
    }
    if ((include_impact || include_dependencies) &&
        !emit_query_paths_and_explain(out, &selected_rows, name_filter, effect_filter, call_filter,
                                      include_impact, include_dependencies)) {
        if (out != stdout) fclose(out);
        free_string_list(&stale_lines);
        free_string_list(&stale_module_refs);
        free_string_list(&stale_module_paths);
        free_state_symbol_rows(&all_rows);
        free_state_symbol_rows(&selected_rows);
        free_project_state_manifest(&manifest);
        indexer_error("VIX038", "Could not emit query paths for \"%s\".", state_path);
        return false;
    }
    if ((include_impact || include_dependencies) &&
        !emit_query_risk_top(out, &selected_rows)) {
        if (out != stdout) fclose(out);
        free_string_list(&stale_lines);
        free_string_list(&stale_module_refs);
        free_string_list(&stale_module_paths);
        free_state_symbol_rows(&all_rows);
        free_state_symbol_rows(&selected_rows);
        free_project_state_manifest(&manifest);
        indexer_error("VIX039", "Could not emit risk ranking for \"%s\".", state_path);
        return false;
    }
    if (!brief_output) {
        if (!emit_resume_symbol_ledger_from_rows(state_path, &selected_rows, out)) {
            if (out != stdout) fclose(out);
            free_string_list(&stale_lines);
            free_string_list(&stale_module_refs);
            free_string_list(&stale_module_paths);
            free_state_symbol_rows(&all_rows);
            free_state_symbol_rows(&selected_rows);
            free_project_state_manifest(&manifest);
            indexer_error("VIX040", "Could not emit state query for \"%s\".", state_path);
            return false;
        }
    } else {
        if (!emit_state_module_section_from_rows(state_path, &selected_rows, out) ||
            !emit_state_brief_rows(out, &selected_rows)) {
            if (out != stdout) fclose(out);
            free_string_list(&stale_lines);
            free_string_list(&stale_module_refs);
            free_string_list(&stale_module_paths);
            free_state_symbol_rows(&all_rows);
            free_state_symbol_rows(&selected_rows);
            free_project_state_manifest(&manifest);
            indexer_error("VIX041", "Could not emit brief state query for \"%s\".", state_path);
            return false;
        }
    }
    if (!emit_state_linked_tests(state_path, &selected_rows, out)) {
        if (out != stdout) fclose(out);
        free_string_list(&stale_lines);
        free_string_list(&stale_module_refs);
        free_string_list(&stale_module_paths);
        free_state_symbol_rows(&all_rows);
        free_state_symbol_rows(&selected_rows);
        free_project_state_manifest(&manifest);
        indexer_error("VIX032", "Could not emit linked tests for \"%s\".", state_path);
        return false;
    }

    if (out != stdout) fclose(out);
    free_string_list(&stale_lines);
    free_string_list(&stale_module_refs);
    free_string_list(&stale_module_paths);
    free_state_symbol_rows(&all_rows);
    free_state_symbol_rows(&selected_rows);
    free_project_state_manifest(&manifest);
    return true;
}

bool bench_project_state(const char* state_path, const char* out_path,
                         const char* focus_symbol, const char* name_filter,
                         const char* effect_filter, const char* call_filter,
                         bool include_impact, bool include_dependencies) {
    bool query_mode = (name_filter && name_filter[0] != '\0') ||
                      (effect_filter && effect_filter[0] != '\0') ||
                      (call_filter && call_filter[0] != '\0');
    if (!query_mode && include_dependencies) {
        indexer_error("VIX042", "State benchmark dependencies require a query filter.");
        return false;
    }

    ProjectStateManifest manifest;
    if (!load_project_state_manifest(state_path, &manifest)) return false;

    StringList stale_lines;
    StateSymbolRowList all_rows;
    StateSymbolRowList selected_rows;
    memset(&stale_lines, 0, sizeof(stale_lines));
    memset(&all_rows, 0, sizeof(all_rows));
    memset(&selected_rows, 0, sizeof(selected_rows));

    int stale_count = 0;
    int seed_matches = 0;
    bool ok = check_project_state_manifest(state_path, &manifest, &stale_lines, &stale_count);
    if (!ok || !load_state_symbol_rows_filtered(state_path, NULL, &all_rows)) {
        free_string_list(&stale_lines);
        free_state_symbol_rows(&all_rows);
        free_state_symbol_rows(&selected_rows);
        free_project_state_manifest(&manifest);
        indexer_error("VIX043", "Could not load state benchmark inputs for \"%s\".", state_path);
        return false;
    }

    if (query_mode) {
        ok = select_query_symbol_rows(&all_rows, name_filter, effect_filter, call_filter,
                                      include_impact, include_dependencies,
                                      &selected_rows, &seed_matches);
    } else if (focus_symbol && focus_symbol[0] != '\0') {
        ok = select_resume_symbol_rows(&all_rows, focus_symbol, include_impact, &selected_rows);
    } else {
        selected_rows = all_rows;
        memset(&all_rows, 0, sizeof(all_rows));
    }

    if (!ok) {
        free_string_list(&stale_lines);
        free_state_symbol_rows(&all_rows);
        free_state_symbol_rows(&selected_rows);
        free_project_state_manifest(&manifest);
        indexer_error("VIX044", "Could not select state benchmark slice for \"%s\".", state_path);
        return false;
    }

    StatePayloadMetrics metrics;
    bool include_tests = query_mode || (focus_symbol && focus_symbol[0] != '\0');
    if (!compute_state_payload_metrics(state_path, &selected_rows, include_tests, &metrics)) {
        free_string_list(&stale_lines);
        free_state_symbol_rows(&all_rows);
        free_state_symbol_rows(&selected_rows);
        free_project_state_manifest(&manifest);
        indexer_error("VIX045", "Could not compute state benchmark metrics for \"%s\".", state_path);
        return false;
    }

    FILE* out = open_index_output(out_path);
    if (!out) {
        free_string_list(&stale_lines);
        free_state_symbol_rows(&all_rows);
        free_state_symbol_rows(&selected_rows);
        free_project_state_manifest(&manifest);
        return false;
    }

    char root_path[MAX_PATH_LEN];
    char entry_path[MAX_PATH_LEN];
    if (!resolve_manifest_root(state_path, &manifest, root_path, sizeof(root_path)) ||
        !resolve_state_path(root_path, manifest.entry, entry_path, sizeof(entry_path))) {
        if (out != stdout) fclose(out);
        free_string_list(&stale_lines);
        free_state_symbol_rows(&all_rows);
        free_state_symbol_rows(&selected_rows);
        free_project_state_manifest(&manifest);
        return false;
    }

    char entry_display[MAX_PATH_LEN];
    format_pack_path(entry_path, entry_display, sizeof(entry_display));

    fprintf(out, "PBENCHv1\n");
    fprintf(out, "state: %s\n", state_path);
    fprintf(out, "entry: %s\n", entry_display);
    fprintf(out, "mode: %s\n", query_mode ? "query" : "resume");
    fprintf(out, "scope: ledger\n");
    fprintf(out, "focus: %s\n", manifest.focus[0] != '\0' ? manifest.focus : "-");
    fprintf(out, "impact: %s\n", manifest.include_impact ? "yes" : "no");
    if (query_mode) {
        fprintf(out, "query_name: %s\n", (name_filter && name_filter[0] != '\0') ? name_filter : "-");
        fprintf(out, "query_effect: %s\n", (effect_filter && effect_filter[0] != '\0') ? effect_filter : "-");
        fprintf(out, "query_call: %s\n", (call_filter && call_filter[0] != '\0') ? call_filter : "-");
        fprintf(out, "query_impact: %s\n", include_impact ? "yes" : "no");
        fprintf(out, "query_deps: %s\n", include_dependencies ? "yes" : "no");
        fprintf(out, "seed_matches: %d\n", seed_matches);
    } else {
        fprintf(out, "resume_focus: %s\n",
                (focus_symbol && focus_symbol[0] != '\0') ? focus_symbol : "-");
        fprintf(out, "resume_impact: %s\n", include_impact ? "yes" : "no");
    }
    fprintf(out, "valid: %s\n", stale_count == 0 ? "yes" : "no");
    fprintf(out, "changed_files: %d\n", stale_count);
    fprintf(out, "selection_symbols: %d\n", selected_rows.count);
    fprintf(out, "selection_modules: %d\n", count_unique_state_modules(&selected_rows));
    fprintf(out, "include_tests: %s\n", include_tests ? "yes" : "no");
    fprintf(out, "brief_bytes: %ld\n", metrics.brief_bytes);
    fprintf(out, "full_bytes: %ld\n", metrics.full_bytes);
    fprintf(out, "saved_bytes: %ld\n", metrics.saved_bytes);
    fprintf(out, "saved_pct: %ld\n", metrics.saved_pct);
    fprintf(out, "brief_tokens_est: %ld\n", metrics.brief_tokens_est);
    fprintf(out, "full_tokens_est: %ld\n", metrics.full_tokens_est);
    fprintf(out, "saved_tokens_est: %ld\n", metrics.saved_tokens_est);
    fprintf(out, "brief_emit_us: %llu\n", metrics.brief_emit_us);
    fprintf(out, "full_emit_us: %llu\n", metrics.full_emit_us);

    if (out != stdout) fclose(out);
    free_string_list(&stale_lines);
    free_state_symbol_rows(&all_rows);
    free_state_symbol_rows(&selected_rows);
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
        indexer_error("VIX046", "Could not verify state file \"%s\".", state_path);
        return false;
    }
    if (stale_count > 0 &&
        !collect_stale_module_refs(state_path, &stale_lines, &stale_module_refs, &stale_module_paths)) {
        free_string_list(&stale_lines);
        free_string_list(&stale_module_refs);
        free_string_list(&stale_module_paths);
        free_project_state_manifest(&manifest);
        indexer_error("VIX047", "Could not map stale modules in state file \"%s\".", state_path);
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
    if (!emit_state_verification_summary(state_path, &manifest,
                                         &stale_lines, &stale_module_refs, out)) {
        if (out != stdout) fclose(out);
        free_string_list(&stale_lines);
        free_string_list(&stale_module_refs);
        free_string_list(&stale_module_paths);
        free_project_state_manifest(&manifest);
        indexer_error("VIX028", "Could not emit state verification summary for \"%s\".", state_path);
        return false;
    }
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
            indexer_error("VIX029", "Could not emit stale symbol map for \"%s\".", state_path);
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

    StateProofSummary existing_proof;
    StringList stale_lines;
    StringList stale_module_refs;
    StringList stale_module_paths;
    StringList refreshed_module_refs;
    StringList refreshed_module_paths;
    memset(&existing_proof, 0, sizeof(existing_proof));
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
        indexer_error("VIX048", "Could not refresh state file \"%s\".", state_path);
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
        indexer_error("VIX047", "Could not map stale modules in state file \"%s\".", state_path);
        return false;
    }
    if (!load_state_proof_summary(state_path, &existing_proof)) {
        free_string_list(&stale_lines);
        free_string_list(&stale_module_refs);
        free_string_list(&stale_module_paths);
        free_string_list(&refreshed_module_refs);
        free_string_list(&refreshed_module_paths);
        free_project_state_manifest(&manifest);
        indexer_error("VIX049", "Could not load state proof for \"%s\".", state_path);
        return false;
    }

    char root_path[MAX_PATH_LEN];
    char entry_path[MAX_PATH_LEN];
    if (!resolve_manifest_root(state_path, &manifest, root_path, sizeof(root_path)) ||
        !resolve_state_path(root_path, manifest.entry, entry_path, sizeof(entry_path))) {
        free_state_proof_summary(&existing_proof);
        free_string_list(&stale_lines);
        free_string_list(&stale_module_refs);
        free_string_list(&stale_module_paths);
        free_string_list(&refreshed_module_refs);
        free_string_list(&refreshed_module_paths);
        free_project_state_manifest(&manifest);
        return false;
    }

    char temp_state_path[MAX_PATH_LEN];
    char temp_proof_path[MAX_PATH_LEN];
    char final_proof_path[MAX_PATH_LEN];
    temp_state_path[0] = '\0';
    temp_proof_path[0] = '\0';
    final_proof_path[0] = '\0';
    ProjectStateManifest refreshed_manifest;
    ProjectIndex refreshed_project;
    StringList tracked_module_paths;
    memset(&refreshed_manifest, 0, sizeof(refreshed_manifest));
    memset(&refreshed_project, 0, sizeof(refreshed_project));
    memset(&tracked_module_paths, 0, sizeof(tracked_module_paths));
    bool semantic_changed = false;
    bool proof_should_refresh = false;
    bool proof_refreshed = false;
    int proof_tests_planned = existing_proof.present ? existing_proof.tests_planned : 0;
    int proof_tests_failed = existing_proof.present ? existing_proof.tests_failed : 0;

    if (stale_count > 0) {
        if (snprintf(temp_state_path, sizeof(temp_state_path), "%s.tmp-%ld",
                     state_path, (long)getpid()) >= (int)sizeof(temp_state_path)) {
            free_state_proof_summary(&existing_proof);
            free_string_list(&stale_lines);
            free_string_list(&stale_module_refs);
            free_string_list(&stale_module_paths);
            free_string_list(&refreshed_module_refs);
            free_string_list(&refreshed_module_paths);
            free_project_state_manifest(&manifest);
            indexer_error("VIX050", "Could not allocate temp state path for \"%s\".", state_path);
            return false;
        }
        unlink(temp_state_path);

        if (!resolve_manifest_tracked_paths(state_path, &manifest, &tracked_module_paths) ||
            !build_project_index_filtered(root_path, &tracked_module_paths, &refreshed_project) ||
            !emit_project_state_from_project(&refreshed_project, entry_path, temp_state_path,
                                             manifest.focus[0] != '\0' ? manifest.focus : NULL,
                                             manifest.include_impact)) {
            unlink(temp_state_path);
            free_state_proof_summary(&existing_proof);
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
            free_state_proof_summary(&existing_proof);
            free_string_list(&stale_lines);
            free_string_list(&stale_module_refs);
            free_string_list(&stale_module_paths);
            free_string_list(&refreshed_module_refs);
            free_string_list(&refreshed_module_paths);
            free_string_list(&tracked_module_paths);
            free_project(&refreshed_project);
            free_project_state_manifest(&manifest);
            indexer_error("VIX051", "Could not reload refreshed state for \"%s\".", state_path);
            return false;
        }
        semantic_changed = manifest.semantic_item_count != refreshed_manifest.semantic_item_count ||
                           strcmp(manifest.semantic_fingerprint, refreshed_manifest.semantic_fingerprint) != 0;

        if (semantic_changed &&
            !collect_state_module_refs_for_paths(temp_state_path, &stale_module_paths,
                                                 &refreshed_module_refs, &refreshed_module_paths)) {
            unlink(temp_state_path);
            free_state_proof_summary(&existing_proof);
            free_string_list(&stale_lines);
            free_string_list(&stale_module_refs);
            free_string_list(&stale_module_paths);
            free_string_list(&refreshed_module_refs);
            free_string_list(&refreshed_module_paths);
            free_string_list(&tracked_module_paths);
            free_project(&refreshed_project);
            free_project_state_manifest(&refreshed_manifest);
            free_project_state_manifest(&manifest);
            indexer_error("VIX052", "Could not map refreshed modules in state file \"%s\".", state_path);
            return false;
        }

        proof_should_refresh =
            existing_proof.present &&
            (semantic_changed || stale_lines_include_tracked_test_change(&manifest, &stale_lines));
        if (proof_should_refresh) {
            if (!build_state_proof_path(state_path, final_proof_path, sizeof(final_proof_path)) ||
                snprintf(temp_proof_path, sizeof(temp_proof_path), "%s.tmp-%ld",
                         final_proof_path, (long)getpid()) >= (int)sizeof(temp_proof_path)) {
                unlink(temp_state_path);
                free_state_proof_summary(&existing_proof);
                free_string_list(&stale_lines);
                free_string_list(&stale_module_refs);
                free_string_list(&stale_module_paths);
                free_string_list(&refreshed_module_refs);
                free_string_list(&refreshed_module_paths);
                free_string_list(&tracked_module_paths);
                free_project(&refreshed_project);
                free_project_state_manifest(&refreshed_manifest);
                free_project_state_manifest(&manifest);
                indexer_error("VIX053", "Could not allocate state proof path for \"%s\".", state_path);
                return false;
            }
            unlink(temp_proof_path);
            if (!write_state_linked_proof(state_path, temp_state_path, temp_proof_path,
                                          &proof_tests_planned, &proof_tests_failed)) {
                unlink(temp_state_path);
                unlink(temp_proof_path);
                free_state_proof_summary(&existing_proof);
                free_string_list(&stale_lines);
                free_string_list(&stale_module_refs);
                free_string_list(&stale_module_paths);
                free_string_list(&refreshed_module_refs);
                free_string_list(&refreshed_module_paths);
                free_string_list(&tracked_module_paths);
                free_project(&refreshed_project);
                free_project_state_manifest(&refreshed_manifest);
                free_project_state_manifest(&manifest);
                indexer_error("VIX054", "Could not refresh state proof for \"%s\".", state_path);
                return false;
            }
            proof_refreshed = true;
        }
    }

    FILE* out = open_index_output(out_path);
    if (!out) {
        unlink(temp_proof_path);
        unlink(temp_state_path);
        free_state_proof_summary(&existing_proof);
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
    if (!emit_state_verification_summary(state_path, &manifest,
                                         &stale_lines, &stale_module_refs, out)) {
        if (out != stdout) fclose(out);
        unlink(temp_proof_path);
        unlink(temp_state_path);
        free_state_proof_summary(&existing_proof);
        free_string_list(&stale_lines);
        free_string_list(&stale_module_refs);
        free_string_list(&stale_module_paths);
        free_string_list(&refreshed_module_refs);
        free_string_list(&refreshed_module_paths);
        free_string_list(&tracked_module_paths);
        free_project(&refreshed_project);
        free_project_state_manifest(&refreshed_manifest);
        free_project_state_manifest(&manifest);
        indexer_error("VIX028", "Could not emit state verification summary for \"%s\".", state_path);
        return false;
    }
    fprintf(out, "proof: %s\n",
            !existing_proof.present ? "none" :
            (proof_refreshed ? (proof_tests_failed == 0 ? "refreshed" : "refreshed-fail") : "retained"));
    if (existing_proof.present || proof_refreshed) {
        fprintf(out, "proof_tests: %d\n", proof_tests_planned);
        fprintf(out, "proof_failed: %d\n", proof_tests_failed);
    }
    if (stale_count > 0) {
        int patched_symbols = 0;
        fprintf(out, "patched_modules: %d\n", semantic_changed ? refreshed_module_refs.count : 0);
        if (semantic_changed) {
            if (!emit_patch_symbol_ledger(state_path, temp_state_path,
                                          &refreshed_module_refs, &refreshed_module_paths, out,
                                          &patched_symbols)) {
                if (out != stdout) fclose(out);
                unlink(temp_proof_path);
                unlink(temp_state_path);
                free_state_proof_summary(&existing_proof);
                free_string_list(&stale_lines);
                free_string_list(&stale_module_refs);
                free_string_list(&stale_module_paths);
                free_string_list(&refreshed_module_refs);
                free_string_list(&refreshed_module_paths);
                free_project_state_manifest(&refreshed_manifest);
                free_project_state_manifest(&manifest);
                indexer_error("VIX055", "Could not emit refresh patch for \"%s\".", state_path);
                return false;
            }
        }
        fprintf(out, "patched_symbols: %d\n", patched_symbols);
    }

    if (out != stdout) fclose(out);
    if (stale_count > 0) {
        if (rename(temp_state_path, state_path) != 0) {
            unlink(temp_state_path);
            unlink(temp_proof_path);
            free_state_proof_summary(&existing_proof);
            free_string_list(&stale_lines);
            free_string_list(&stale_module_refs);
            free_string_list(&stale_module_paths);
            free_string_list(&refreshed_module_refs);
            free_string_list(&refreshed_module_paths);
            free_string_list(&tracked_module_paths);
            free_project(&refreshed_project);
            free_project_state_manifest(&refreshed_manifest);
            free_project_state_manifest(&manifest);
            indexer_error("VIX056", "Could not replace state file \"%s\".", state_path);
            return false;
        }
        if (proof_refreshed && rename(temp_proof_path, final_proof_path) != 0) {
            unlink(temp_proof_path);
            free_state_proof_summary(&existing_proof);
            free_string_list(&stale_lines);
            free_string_list(&stale_module_refs);
            free_string_list(&stale_module_paths);
            free_string_list(&refreshed_module_refs);
            free_string_list(&refreshed_module_paths);
            free_string_list(&tracked_module_paths);
            free_project(&refreshed_project);
            free_project_state_manifest(&refreshed_manifest);
            free_project_state_manifest(&manifest);
            indexer_error("VIX057", "Could not replace state proof file \"%s\".", final_proof_path);
            return false;
        }
    }
    free_state_proof_summary(&existing_proof);
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
    StateTestLinkList test_links;
    memset(&focus_slice, 0, sizeof(focus_slice));
    memset(&impact_slice, 0, sizeof(impact_slice));
    memset(&semantic_items, 0, sizeof(semantic_items));
    memset(&test_links, 0, sizeof(test_links));

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
    if (!collect_state_test_links(index_project_root[0] != '\0' ? index_project_root : ".",
                                  &semantic_items, &test_links)) {
        if (out != stdout) fclose(out);
        free_semantic_items(&semantic_items);
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
        free_state_test_links(&test_links);
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
            free_semantic_items(&semantic_items);
            free_state_test_links(&test_links);
            free_focus_slice(&impact_slice);
            free_focus_slice(&focus_slice);
            return false;
        }
        fprintf(out, "  %s sha256=%s\n", module_path, hash_hex);
    }
    if (!emit_symbol_index_sections(out, &semantic_items)) {
        if (out != stdout) fclose(out);
        free_semantic_items(&semantic_items);
        free_state_test_links(&test_links);
        free_focus_slice(&impact_slice);
        free_focus_slice(&focus_slice);
        return false;
    }
    fprintf(out, "summary_items:\n");
    for (int i = 0; i < semantic_items.count; i++) {
        fprintf(out, "  %s\n", semantic_items.items[i].text);
    }
    if (!emit_state_test_sections(out, &test_links)) {
        if (out != stdout) fclose(out);
        free_semantic_items(&semantic_items);
        free_state_test_links(&test_links);
        free_focus_slice(&impact_slice);
        free_focus_slice(&focus_slice);
        return false;
    }
    fprintf(out, "context:\n");
    emit_context_pack_text(out, project, entry_path, focus_slice_ptr, impact_slice_ptr,
                           focus_symbol, include_impact);

    if (out != stdout) fclose(out);
    free_semantic_items(&semantic_items);
    free_state_test_links(&test_links);
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

bool emit_state_plan(const char* before_state, const char* after_state, const char* out_path) {
    if (!before_state || !after_state) {
        indexer_error("VIX058", "State plan requires before and after state files.");
        return false;
    }

    ProjectStateManifest before_manifest;
    ProjectStateManifest after_manifest;
    SemanticItemList before_items;
    SemanticItemList after_items;
    StateSymbolRowList before_rows;
    StateSymbolRowList after_rows;
    memset(&before_manifest, 0, sizeof(before_manifest));
    memset(&after_manifest, 0, sizeof(after_manifest));
    memset(&before_items, 0, sizeof(before_items));
    memset(&after_items, 0, sizeof(after_items));
    memset(&before_rows, 0, sizeof(before_rows));
    memset(&after_rows, 0, sizeof(after_rows));

    bool ok = load_project_state_manifest(before_state, &before_manifest) &&
              load_project_state_manifest(after_state, &after_manifest) &&
              load_state_semantic_items(before_state, &before_items) &&
              load_state_semantic_items(after_state, &after_items) &&
              load_state_symbol_rows_filtered(before_state, NULL, &before_rows) &&
              load_state_symbol_rows_filtered(after_state, NULL, &after_rows);
    if (!ok) {
        free_project_state_manifest(&before_manifest);
        free_project_state_manifest(&after_manifest);
        free_semantic_items(&before_items);
        free_semantic_items(&after_items);
        free_state_symbol_rows(&before_rows);
        free_state_symbol_rows(&after_rows);
        indexer_error("VIX059", "Could not load state plan inputs.");
        return false;
    }

    if (strcmp(before_manifest.focus, after_manifest.focus) != 0 ||
        before_manifest.include_impact != after_manifest.include_impact) {
        free_project_state_manifest(&before_manifest);
        free_project_state_manifest(&after_manifest);
        free_semantic_items(&before_items);
        free_semantic_items(&after_items);
        free_state_symbol_rows(&before_rows);
        free_state_symbol_rows(&after_rows);
        indexer_error("VIX060", "State plan requires matching focus and impact settings.");
        return false;
    }

    FILE* out = open_index_output(out_path);
    if (!out) {
        free_project_state_manifest(&before_manifest);
        free_project_state_manifest(&after_manifest);
        free_semantic_items(&before_items);
        free_semantic_items(&after_items);
        free_state_symbol_rows(&before_rows);
        free_state_symbol_rows(&after_rows);
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

    fprintf(out, "PSTATEPLANv1\n");
    fprintf(out, "before_state: %s\n", before_state);
    fprintf(out, "after_state: %s\n", after_state);
    fprintf(out, "before_entry: %s\n", before_manifest.entry);
    fprintf(out, "after_entry: %s\n", after_manifest.entry);
    fprintf(out, "focus: %s\n", before_manifest.focus[0] != '\0' ? before_manifest.focus : "-");
    fprintf(out, "impact: %s\n", before_manifest.include_impact ? "yes" : "no");
    fprintf(out, "summary: before=%d after=%d added=%d removed=%d unchanged=%d\n",
            before_items.count, after_items.count, added_count, removed_count, unchanged_count);

    ok = emit_semantic_change_plan(out, &before_rows, &after_rows) &&
         emit_state_test_plan(out, before_state, after_state,
                              &before_items, &after_items, &before_rows, &after_rows);

    if (out != stdout) fclose(out);
    free_project_state_manifest(&before_manifest);
    free_project_state_manifest(&after_manifest);
    free_semantic_items(&before_items);
    free_semantic_items(&after_items);
    free_state_symbol_rows(&before_rows);
    free_state_symbol_rows(&after_rows);
    return ok;
}

static bool stale_lines_include_tracked_test_change(const ProjectStateManifest* manifest,
                                                    const StringList* stale_lines) {
    if (!manifest || !stale_lines) return false;
    for (int i = 0; i < stale_lines->count; i++) {
        char stale_path[MAX_PATH_LEN];
        if (!parse_stale_line_path(stale_lines->items[i], stale_path, sizeof(stale_path))) continue;
        if (string_list_index_of(&manifest->tracked_test_paths, stale_path) != -1) return true;
    }
    return false;
}

static bool emit_state_exec_report(FILE* out, const char* before_state, const char* after_state,
                                   const ProjectStateManifest* after_manifest, const char* after_root,
                                   const StatePlanTestCandidateList* candidates, int* failed_out) {
    if (!out || !before_state || !after_state || !after_manifest || !after_root || !candidates) return false;

    fprintf(out, "PSTATEEXECv2\n");
    fprintf(out, "before_state: %s\n", before_state);
    fprintf(out, "after_state: %s\n", after_state);
    fprintf(out, "focus: %s\n", after_manifest->focus[0] != '\0' ? after_manifest->focus : "-");
    fprintf(out, "impact: %s\n", after_manifest->include_impact ? "yes" : "no");
    fprintf(out, "semantic_fingerprint: %s\n",
            after_manifest->semantic_fingerprint[0] != '\0' ? after_manifest->semantic_fingerprint : "-");
    fprintf(out, "tests_planned: %d\n", candidates->count);
    fprintf(out, "results:\n");

    int failed = 0;
    for (int i = 0; i < candidates->count; i++) {
        char test_path[MAX_PATH_LEN];
        bool resolved = resolve_state_path(after_root, candidates->items[i].path, test_path, sizeof(test_path));

        int exit_code = 127;
        char preview[256];
        preview[0] = '\0';
        bool ran = resolved && run_viper_script_capture("/proc/self/exe", test_path, &exit_code,
                                                        preview, sizeof(preview));
        bool passed = ran && exit_code == 0;
        if (!passed) failed++;

        fprintf(out, "  %d|test=%s|status=%s|exit=%d|hits=%d|max_score=%d|symbols=",
                i + 1, candidates->items[i].path, passed ? "pass" : "fail",
                ran ? exit_code : 127, candidates->items[i].hits, candidates->items[i].max_score);
        emit_string_list_csv(out, &candidates->items[i].symbols);
        fprintf(out, "|checks=");
        emit_test_plan_checks(out, candidates->items[i].has_changed, candidates->items[i].has_added,
                              candidates->items[i].has_removed, candidates->items[i].has_effects,
                              candidates->items[i].has_callers);
        if (!passed && preview[0] != '\0') {
            fprintf(out, "|detail=%s", preview);
        }
        fprintf(out, "\n");
    }
    fprintf(out, "tests_failed: %d\n", failed);
    if (failed_out) *failed_out = failed;
    return true;
}

static bool write_state_linked_proof(const char* before_state, const char* after_state,
                                     const char* out_path, int* tests_planned_out,
                                     int* tests_failed_out) {
    if (!before_state || !after_state || !out_path) return false;

    ProjectStateManifest after_manifest;
    StateSymbolRowList after_rows;
    StatePlanTestCandidateList candidates;
    memset(&after_manifest, 0, sizeof(after_manifest));
    memset(&after_rows, 0, sizeof(after_rows));
    memset(&candidates, 0, sizeof(candidates));

    bool ok = load_project_state_manifest(after_state, &after_manifest) &&
              load_state_symbol_rows_filtered(after_state, NULL, &after_rows) &&
              collect_state_linked_test_candidates(after_state, &after_rows, &candidates);
    if (!ok) {
        free_project_state_manifest(&after_manifest);
        free_state_symbol_rows(&after_rows);
        free_state_plan_test_candidates(&candidates);
        return false;
    }

    char after_root[MAX_PATH_LEN];
    if (!resolve_manifest_root(after_state, &after_manifest, after_root, sizeof(after_root))) {
        free_project_state_manifest(&after_manifest);
        free_state_symbol_rows(&after_rows);
        free_state_plan_test_candidates(&candidates);
        return false;
    }

    FILE* out = open_index_output(out_path);
    if (!out) {
        free_project_state_manifest(&after_manifest);
        free_state_symbol_rows(&after_rows);
        free_state_plan_test_candidates(&candidates);
        return false;
    }

    int failed = 0;
    ok = emit_state_exec_report(out, before_state, after_state, &after_manifest, after_root,
                                &candidates, &failed);
    if (out != stdout) fclose(out);
    if (tests_planned_out) *tests_planned_out = candidates.count;
    if (tests_failed_out) *tests_failed_out = failed;

    free_project_state_manifest(&after_manifest);
    free_state_symbol_rows(&after_rows);
    free_state_plan_test_candidates(&candidates);
    return ok;
}

bool run_state_plan(const char* before_state, const char* after_state, const char* out_path) {
    if (!before_state || !after_state) {
        indexer_error("VIX061", "State test run requires before and after state files.");
        return false;
    }

    ProjectStateManifest before_manifest;
    ProjectStateManifest after_manifest;
    SemanticItemList before_items;
    SemanticItemList after_items;
    StateSymbolRowList before_rows;
    StateSymbolRowList after_rows;
    StatePlanTestCandidateList candidates;
    memset(&before_manifest, 0, sizeof(before_manifest));
    memset(&after_manifest, 0, sizeof(after_manifest));
    memset(&before_items, 0, sizeof(before_items));
    memset(&after_items, 0, sizeof(after_items));
    memset(&before_rows, 0, sizeof(before_rows));
    memset(&after_rows, 0, sizeof(after_rows));
    memset(&candidates, 0, sizeof(candidates));

    bool ok = load_project_state_manifest(before_state, &before_manifest) &&
              load_project_state_manifest(after_state, &after_manifest) &&
              load_state_semantic_items(before_state, &before_items) &&
              load_state_semantic_items(after_state, &after_items) &&
              load_state_symbol_rows_filtered(before_state, NULL, &before_rows) &&
              load_state_symbol_rows_filtered(after_state, NULL, &after_rows) &&
              build_state_test_plan_candidates(before_state, after_state,
                                               &before_items, &after_items, &before_rows, &after_rows,
                                               &candidates);
    if (!ok) {
        free_project_state_manifest(&before_manifest);
        free_project_state_manifest(&after_manifest);
        free_semantic_items(&before_items);
        free_semantic_items(&after_items);
        free_state_symbol_rows(&before_rows);
        free_state_symbol_rows(&after_rows);
        free_state_plan_test_candidates(&candidates);
        indexer_error("VIX062", "Could not build state test run inputs.");
        return false;
    }

    if (strcmp(before_manifest.focus, after_manifest.focus) != 0 ||
        before_manifest.include_impact != after_manifest.include_impact) {
        free_project_state_manifest(&before_manifest);
        free_project_state_manifest(&after_manifest);
        free_semantic_items(&before_items);
        free_semantic_items(&after_items);
        free_state_symbol_rows(&before_rows);
        free_state_symbol_rows(&after_rows);
        free_state_plan_test_candidates(&candidates);
        indexer_error("VIX063", "State test run requires matching focus and impact settings.");
        return false;
    }

    char after_root[MAX_PATH_LEN];
    char before_root[MAX_PATH_LEN];
    if (!resolve_manifest_root(after_state, &after_manifest, after_root, sizeof(after_root)) ||
        !resolve_manifest_root(before_state, &before_manifest, before_root, sizeof(before_root))) {
        free_project_state_manifest(&before_manifest);
        free_project_state_manifest(&after_manifest);
        free_semantic_items(&before_items);
        free_semantic_items(&after_items);
        free_state_symbol_rows(&before_rows);
        free_state_symbol_rows(&after_rows);
        free_state_plan_test_candidates(&candidates);
        indexer_error("VIX064", "Could not prepare state test runner.");
        return false;
    }

    FILE* out = open_index_output(out_path);
    if (!out) {
        free_project_state_manifest(&before_manifest);
        free_project_state_manifest(&after_manifest);
        free_semantic_items(&before_items);
        free_semantic_items(&after_items);
        free_state_symbol_rows(&before_rows);
        free_state_symbol_rows(&after_rows);
        free_state_plan_test_candidates(&candidates);
        return false;
    }

    int failed = 0;
    ok = emit_state_exec_report(out, before_state, after_state, &after_manifest, after_root,
                                &candidates, &failed);

    if (out != stdout) fclose(out);
    free_project_state_manifest(&before_manifest);
    free_project_state_manifest(&after_manifest);
    free_semantic_items(&before_items);
    free_semantic_items(&after_items);
    free_state_symbol_rows(&before_rows);
    free_state_symbol_rows(&after_rows);
    free_state_plan_test_candidates(&candidates);
    return failed == 0;
}

bool emit_semantic_diff(const char* before_entry, const char* after_entry, const char* out_path,
                        const char* focus_symbol, bool include_impact) {
    if (!before_entry || !after_entry || !focus_symbol || focus_symbol[0] == '\0') {
        indexer_error("VIX065", "Semantic diff requires before, after, and focus symbol.");
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
        indexer_error("VIX066", "Focus symbol \"%s\" not found in either diff input.", focus_symbol);
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
    StateSymbolRowList before_rows;
    StateSymbolRowList after_rows;
    memset(&before_items, 0, sizeof(before_items));
    memset(&after_items, 0, sizeof(after_items));
    memset(&before_rows, 0, sizeof(before_rows));
    memset(&after_rows, 0, sizeof(after_rows));

    bool ok = collect_focus_semantic_items(&before_project, &before_focus, &before_items) &&
              collect_focus_semantic_items(&after_project, &after_focus, &after_items);
    if (ok && include_impact) {
        ok = collect_impact_semantic_items(&before_project, &before_impact, &before_items) &&
             collect_impact_semantic_items(&after_project, &after_impact, &after_items);
    }
    if (!ok) {
        indexer_error("VIX067", "Could not build semantic diff items.");
        free_semantic_items(&before_items);
        free_semantic_items(&after_items);
        free_state_symbol_rows(&before_rows);
        free_state_symbol_rows(&after_rows);
        free_focus_slice(&before_impact);
        free_focus_slice(&after_impact);
        free_focus_slice(&before_focus);
        free_focus_slice(&after_focus);
        free_project(&before_project);
        free_project(&after_project);
        return false;
    }
    if (!collect_symbol_rows_from_semantic_items(&before_items, &before_rows) ||
        !collect_symbol_rows_from_semantic_items(&after_items, &after_rows)) {
        indexer_error("VIX068", "Could not build semantic diff rows.");
        free_semantic_items(&before_items);
        free_semantic_items(&after_items);
        free_state_symbol_rows(&before_rows);
        free_state_symbol_rows(&after_rows);
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
        free_state_symbol_rows(&before_rows);
        free_state_symbol_rows(&after_rows);
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

    fprintf(out, "SDIFFv2\n");
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
        if (!emit_semantic_change_plan(out, &before_rows, &after_rows)) {
            if (out != stdout) fclose(out);
            free_semantic_items(&before_items);
            free_semantic_items(&after_items);
            free_state_symbol_rows(&before_rows);
            free_state_symbol_rows(&after_rows);
            free_focus_slice(&before_impact);
            free_focus_slice(&after_impact);
            free_focus_slice(&before_focus);
            free_focus_slice(&after_focus);
            free_project(&before_project);
            free_project(&after_project);
            indexer_error("VIX069", "Could not emit semantic change plan.");
            return false;
        }
        if (!emit_semantic_test_plan(out, &before_items, &after_items, &before_rows, &after_rows)) {
            if (out != stdout) fclose(out);
            free_semantic_items(&before_items);
            free_semantic_items(&after_items);
            free_state_symbol_rows(&before_rows);
            free_state_symbol_rows(&after_rows);
            free_focus_slice(&before_impact);
            free_focus_slice(&after_impact);
            free_focus_slice(&before_focus);
            free_focus_slice(&after_focus);
            free_project(&before_project);
            free_project(&after_project);
            indexer_error("VIX070", "Could not emit semantic test plan.");
            return false;
        }
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
    free_state_symbol_rows(&before_rows);
    free_state_symbol_rows(&after_rows);
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
