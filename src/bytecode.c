#include "bytecode.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONTAINER_MAGIC "VPK1"
#define CONTAINER_VERSION 1

#define CONTAINER_KIND_SOURCE   1
#define CONTAINER_KIND_BYTECODE 2

typedef struct {
    Obj* obj;
    uint32_t id;
} ObjRef;

typedef struct {
    ObjRef* items;
    int count;
    int cap;
} ObjTable;

typedef struct {
    ObjFunction* owner;
    int constant_index;
    uint32_t target_id;
} ObjFixup;

typedef struct {
    ObjFixup* items;
    int count;
    int cap;
} FixupTable;

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

static bool write_bytes(FILE* out, const void* bytes, size_t n) {
    return fwrite(bytes, 1, n, out) == n;
}

static bool read_bytes(FILE* in, void* bytes, size_t n) {
    return fread(bytes, 1, n, in) == n;
}

static bool write_u8(FILE* out, uint8_t v) {
    return write_bytes(out, &v, sizeof(v));
}

static bool write_u32(FILE* out, uint32_t v) {
    return write_bytes(out, &v, sizeof(v));
}

static bool write_i32(FILE* out, int32_t v) {
    return write_bytes(out, &v, sizeof(v));
}

static bool write_f64(FILE* out, double v) {
    return write_bytes(out, &v, sizeof(v));
}

static bool read_u8(FILE* in, uint8_t* out_v) {
    return read_bytes(in, out_v, sizeof(*out_v));
}

static bool read_u32(FILE* in, uint32_t* out_v) {
    return read_bytes(in, out_v, sizeof(*out_v));
}

static bool read_i32(FILE* in, int32_t* out_v) {
    return read_bytes(in, out_v, sizeof(*out_v));
}

static bool read_f64(FILE* in, double* out_v) {
    return read_bytes(in, out_v, sizeof(*out_v));
}

static bool write_container_header(FILE* out, uint8_t kind) {
    return write_bytes(out, CONTAINER_MAGIC, 4) &&
           write_u8(out, kind) &&
           write_u32(out, CONTAINER_VERSION);
}

static bool read_container_header(FILE* in, uint8_t expected_kind) {
    char magic[4];
    uint8_t kind = 0;
    uint32_t version = 0;
    if (!read_bytes(in, magic, sizeof(magic)) ||
        !read_u8(in, &kind) ||
        !read_u32(in, &version)) {
        return false;
    }
    return memcmp(magic, CONTAINER_MAGIC, 4) == 0 &&
           kind == expected_kind &&
           version == CONTAINER_VERSION;
}

static char* read_len_text(FILE* in, uint32_t len) {
    char* out = (char*)malloc((size_t)len + 1);
    if (!out) return NULL;
    if (!read_bytes(in, out, len)) {
        free(out);
        return NULL;
    }
    out[len] = '\0';
    return out;
}

bool write_source_cache_file(const char* path, const char* source) {
    if (!path || !source) return false;

    FILE* out = fopen(path, "wb");
    if (!out) return false;

    uint32_t len = (uint32_t)strlen(source);
    bool ok = write_container_header(out, CONTAINER_KIND_SOURCE) &&
              write_u32(out, len) &&
              write_bytes(out, source, len);

    fclose(out);
    return ok;
}

char* read_source_cache_file(const char* path) {
    if (!path) return NULL;

    FILE* in = fopen(path, "rb");
    if (!in) return NULL;

    uint32_t len = 0;
    bool ok = read_container_header(in, CONTAINER_KIND_SOURCE) &&
              read_u32(in, &len);
    if (!ok) {
        fclose(in);
        return NULL;
    }

    char* out = read_len_text(in, len);
    fclose(in);
    return out;
}

static int table_find_id(const ObjTable* table, const Obj* obj) {
    for (int i = 0; i < table->count; i++) {
        if (table->items[i].obj == obj) return (int)table->items[i].id;
    }
    return -1;
}

static bool is_supported_obj(const Obj* obj) {
    if (!obj) return false;
    return obj->type == OBJ_STRING ||
           obj->type == OBJ_FUNCTION ||
           obj->type == OBJ_STRUCT;
}

static bool table_get_or_add(ObjTable* table, Obj* obj, uint32_t* out_id) {
    int found = table_find_id(table, obj);
    if (found >= 0) {
        *out_id = (uint32_t)found;
        return true;
    }

    if (!ensure_capacity((void**)&table->items, &table->cap, table->count + 1, sizeof(ObjRef))) {
        return false;
    }

    uint32_t id = (uint32_t)(table->count + 1);
    table->items[table->count].obj = obj;
    table->items[table->count].id = id;
    table->count++;
    *out_id = id;
    return true;
}

static bool collect_referenced_objects(ObjTable* table) {
    for (int i = 0; i < table->count; i++) {
        Obj* obj = table->items[i].obj;
        if (!obj) return false;
        if (obj->type != OBJ_FUNCTION) continue;

        ObjFunction* fn = (ObjFunction*)obj;
        for (int c = 0; c < fn->chunk.constant_count; c++) {
            Value v = fn->chunk.constants[c];
            if (v.type != VAL_OBJ || v.as.obj == NULL) continue;
            if (!is_supported_obj(v.as.obj)) {
                return false;
            }
            uint32_t ignored = 0;
            if (!table_get_or_add(table, v.as.obj, &ignored)) return false;
        }
    }
    return true;
}

static bool write_text_block(FILE* out, const char* text, uint32_t len) {
    if (!write_u32(out, len)) return false;
    return write_bytes(out, text, len);
}

static bool write_value(FILE* out, Value value, const ObjTable* table) {
    if (!write_u8(out, (uint8_t)value.type)) return false;
    switch (value.type) {
        case VAL_BOOL:
            return write_u8(out, value.as.boolean ? 1 : 0);
        case VAL_NIL:
            return true;
        case VAL_NUMBER:
            return write_f64(out, value.as.number);
        case VAL_OBJ: {
            if (!value.as.obj || !is_supported_obj(value.as.obj)) return false;
            int id = table_find_id(table, value.as.obj);
            if (id <= 0) return false;
            return write_u32(out, (uint32_t)id);
        }
        default:
            return false;
    }
}

bool write_bytecode_file(const char* path, ObjFunction* main_fn) {
    if (!path || !main_fn) return false;

    ObjTable table;
    memset(&table, 0, sizeof(table));

    uint32_t root_id = 0;
    if (!table_get_or_add(&table, (Obj*)main_fn, &root_id) ||
        !collect_referenced_objects(&table)) {
        free(table.items);
        return false;
    }

    FILE* out = fopen(path, "wb");
    if (!out) {
        free(table.items);
        return false;
    }

    bool ok = true;
    ok = ok && write_container_header(out, CONTAINER_KIND_BYTECODE);
    ok = ok && write_u32(out, (uint32_t)table.count);
    ok = ok && write_u32(out, root_id);

    for (int i = 0; ok && i < table.count; i++) {
        Obj* obj = table.items[i].obj;
        uint32_t id = table.items[i].id;
        ok = ok && write_u32(out, id);
        ok = ok && write_u8(out, (uint8_t)obj->type);

        if (!ok) break;

        if (obj->type == OBJ_STRING) {
            ObjString* s = (ObjString*)obj;
            ok = ok && write_text_block(out, s->chars, (uint32_t)s->length);
        } else if (obj->type == OBJ_STRUCT) {
            ObjStruct* st = (ObjStruct*)obj;
            ok = ok && write_text_block(out, st->name, (uint32_t)st->name_len);
            ok = ok && write_u32(out, (uint32_t)st->field_count);
            for (int f = 0; ok && f < st->field_count; f++) {
                ok = ok && write_text_block(out, st->field_names[f], (uint32_t)st->field_name_lens[f]);
            }
        } else if (obj->type == OBJ_FUNCTION) {
            ObjFunction* fn = (ObjFunction*)obj;
            ok = ok && write_text_block(out, fn->name, (uint32_t)fn->name_len);
            ok = ok && write_i32(out, fn->arity);
            ok = ok && write_i32(out, fn->base_reg);
            ok = ok && write_u32(out, (uint32_t)fn->chunk.count);
            ok = ok && write_u32(out, (uint32_t)fn->chunk.constant_count);

            for (int c = 0; ok && c < fn->chunk.count; c++) {
                ok = ok && write_u32(out, fn->chunk.code[c]);
            }
            for (int c = 0; ok && c < fn->chunk.constant_count; c++) {
                ok = ok && write_value(out, fn->chunk.constants[c], &table);
            }
        } else {
            ok = false;
        }
    }

    fclose(out);
    free(table.items);
    return ok;
}

static bool add_fixup(FixupTable* table, ObjFunction* owner, int constant_index, uint32_t target_id) {
    if (!ensure_capacity((void**)&table->items, &table->cap, table->count + 1, sizeof(ObjFixup))) {
        return false;
    }
    table->items[table->count].owner = owner;
    table->items[table->count].constant_index = constant_index;
    table->items[table->count].target_id = target_id;
    table->count++;
    return true;
}

static bool read_value(FILE* in, Value* out_value, FixupTable* fixups,
                       ObjFunction* owner, int constant_index) {
    uint8_t raw_type = 0;
    if (!read_u8(in, &raw_type)) return false;

    out_value->type = (ValueType)raw_type;
    switch (out_value->type) {
        case VAL_BOOL: {
            uint8_t b = 0;
            if (!read_u8(in, &b)) return false;
            out_value->as.boolean = b != 0;
            return true;
        }
        case VAL_NIL:
            out_value->as.number = 0;
            return true;
        case VAL_NUMBER:
            return read_f64(in, &out_value->as.number);
        case VAL_OBJ: {
            uint32_t target_id = 0;
            if (!read_u32(in, &target_id)) return false;
            out_value->as.obj = NULL;
            return add_fixup(fixups, owner, constant_index, target_id);
        }
        default:
            return false;
    }
}

ObjFunction* read_bytecode_file(const char* path) {
    if (!path) return NULL;

    FILE* in = fopen(path, "rb");
    if (!in) return NULL;

    uint32_t object_count = 0;
    uint32_t root_id = 0;

    bool ok = read_container_header(in, CONTAINER_KIND_BYTECODE) &&
              read_u32(in, &object_count) &&
              read_u32(in, &root_id);

    if (!ok || object_count == 0 ||
        root_id == 0 || root_id > object_count) {
        fclose(in);
        return NULL;
    }

    Obj** id_to_obj = (Obj**)calloc((size_t)object_count + 1, sizeof(Obj*));
    if (!id_to_obj) {
        fclose(in);
        return NULL;
    }

    FixupTable fixups;
    memset(&fixups, 0, sizeof(fixups));

    for (uint32_t i = 0; ok && i < object_count; i++) {
        uint32_t id = 0;
        uint8_t obj_type = 0;
        ok = read_u32(in, &id) && read_u8(in, &obj_type);
        if (!ok) break;
        if (id == 0 || id > object_count || id_to_obj[id] != NULL) {
            ok = false;
            break;
        }

        if (obj_type == OBJ_STRING) {
            uint32_t len = 0;
            ok = read_u32(in, &len);
            if (!ok) break;
            char* text = read_len_text(in, len);
            if (!text) {
                ok = false;
                break;
            }
            ObjString* s = copy_string(text, (int)len);
            free(text);
            if (!s) {
                ok = false;
                break;
            }
            id_to_obj[id] = (Obj*)s;
        } else if (obj_type == OBJ_STRUCT) {
            uint32_t name_len = 0;
            uint32_t field_count = 0;
            ok = read_u32(in, &name_len);
            if (!ok) break;
            char* name = read_len_text(in, name_len);
            if (!name) {
                ok = false;
                break;
            }
            ok = read_u32(in, &field_count);
            if (!ok) {
                free(name);
                break;
            }

            const char** fields = NULL;
            int* field_lens = NULL;
            if (field_count > 0) {
                fields = (const char**)malloc(sizeof(char*) * field_count);
                field_lens = (int*)malloc(sizeof(int) * field_count);
                if (!fields || !field_lens) {
                    free(name);
                    free((void*)fields);
                    free(field_lens);
                    ok = false;
                    break;
                }
                memset((void*)fields, 0, sizeof(char*) * field_count);
            }

            for (uint32_t f = 0; ok && f < field_count; f++) {
                uint32_t field_len = 0;
                ok = read_u32(in, &field_len);
                if (!ok) break;
                char* field_text = read_len_text(in, field_len);
                if (!field_text) {
                    ok = false;
                    break;
                }
                fields[f] = field_text;
                field_lens[f] = (int)field_len;
            }

            if (!ok) {
                free(name);
                if (fields) {
                    for (uint32_t f = 0; f < field_count; f++) free((void*)fields[f]);
                }
                free((void*)fields);
                free(field_lens);
                break;
            }

            ObjStruct* st = new_struct(name, (int)name_len, (int)field_count, fields, field_lens);
            if (!st) {
                ok = false;
                break;
            }
            id_to_obj[id] = (Obj*)st;
        } else if (obj_type == OBJ_FUNCTION) {
            uint32_t name_len = 0;
            int32_t arity = 0;
            int32_t base_reg = 0;
            uint32_t code_count = 0;
            uint32_t const_count = 0;

            ok = read_u32(in, &name_len);
            if (!ok) break;
            char* name = read_len_text(in, name_len);
            if (!name) {
                ok = false;
                break;
            }

            ok = read_i32(in, &arity) &&
                 read_i32(in, &base_reg) &&
                 read_u32(in, &code_count) &&
                 read_u32(in, &const_count);
            if (!ok) break;

            ObjFunction* fn = new_function(name, (int)name_len, (int)arity);
            if (!fn) {
                ok = false;
                break;
            }
            fn->base_reg = (int)base_reg;

            for (uint32_t c = 0; ok && c < code_count; c++) {
                uint32_t inst = 0;
                ok = read_u32(in, &inst);
                if (!ok) break;
                write_chunk(&fn->chunk, inst);
            }

            for (uint32_t c = 0; ok && c < const_count; c++) {
                Value v;
                memset(&v, 0, sizeof(v));
                int idx = add_constant(&fn->chunk, v);
                if (idx < 0 || idx >= fn->chunk.constant_count) {
                    ok = false;
                    break;
                }
                Value* slot = &fn->chunk.constants[idx];
                ok = read_value(in, slot, &fixups, fn, idx);
            }

            if (!ok) break;
            id_to_obj[id] = (Obj*)fn;
        } else {
            ok = false;
            break;
        }
    }

    if (ok) {
        for (int i = 0; i < fixups.count; i++) {
            uint32_t target_id = fixups.items[i].target_id;
            if (target_id == 0 || target_id > object_count || !id_to_obj[target_id]) {
                ok = false;
                break;
            }
            ObjFunction* owner = fixups.items[i].owner;
            int ci = fixups.items[i].constant_index;
            if (!owner || ci < 0 || ci >= owner->chunk.constant_count) {
                ok = false;
                break;
            }
            owner->chunk.constants[ci].as.obj = id_to_obj[target_id];
        }
    }

    ObjFunction* root = NULL;
    if (ok && id_to_obj[root_id] && id_to_obj[root_id]->type == OBJ_FUNCTION) {
        root = (ObjFunction*)id_to_obj[root_id];
    }

    free(id_to_obj);
    free(fixups.items);
    fclose(in);
    return root;
}
