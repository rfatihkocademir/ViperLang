#define _GNU_SOURCE
#include "native.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdarg.h>
#include <dlfcn.h>
#include <ffi.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <math.h>
#include <limits.h>
#include <stdint.h>

Obj* objects = NULL;

#define MAX_NATIVE_TEXT 4096
#define MAX_DB_PATH_LEN 1024
#define MAX_CACHE_ENTRIES 1024
#define MAX_WEB_ROUTES 128
#define MAX_WEB_MIDDLEWARES 32
#define MAX_WEB_STATICS 16
#define MAX_WS_CLIENTS 64
#define MAX_AI_TOOLS 64

static char g_db_path[MAX_DB_PATH_LEN] = "data.db";
static bool g_os_features_notice_printed = false;
static bool g_ai_features_notice_printed = false;
static char g_ai_provider[64] = "";
static char g_ai_key[256] = "";

typedef struct {
    char* key;
    Value value;
    int64_t expire_at_ms;
    bool used;
} CacheEntry;

static CacheEntry g_cache[MAX_CACHE_ENTRIES];

typedef struct {
    char method[16];
    char path[256];
    Value handler;
    bool used;
} WebRoute;

typedef struct {
    char prefix[128];
    char dir[PATH_MAX];
    bool used;
} WebStaticMount;

static WebRoute g_web_routes[MAX_WEB_ROUTES];
static Value g_web_middlewares[MAX_WEB_MIDDLEWARES];
static int g_web_middleware_count = 0;
static WebStaticMount g_web_static_mounts[MAX_WEB_STATICS];
static char g_web_cors_origin[256] = "";

typedef struct {
    int id;
    int fd;
    bool used;
} WsClient;

static WsClient g_ws_clients[MAX_WS_CLIENTS];
static int g_ws_next_client_id = 1;
static Value g_ai_tools[MAX_AI_TOOLS];
static int g_ai_tool_count = 0;

// Allocate a generic object and add to memory chain
static Obj* allocate_obj(size_t size, int type) {
    Obj* object = (Obj*)malloc(size);
    object->type = type;
    object->ref_count = 1; // start with 1 reference
    object->next = objects;
    objects = object;
    return object;
}

// FNV-1a hash algorithm for fast string hashing
static uint32_t hash_string(const char* key, int length) {
    uint32_t hash = 2166136261u;
    for (int i = 0; i < length; i++) {
        hash ^= (uint8_t)key[i];
        hash *= 16777619;
    }
    return hash;
}

static int64_t now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000LL + (int64_t)(ts.tv_nsec / 1000000LL);
}

static bool is_string_value(Value v) {
    return IS_OBJ(v) && AS_OBJ(v)->type == OBJ_STRING;
}

static char* dup_cstr(const char* text) {
    size_t len = strlen(text);
    char* out = (char*)malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, text, len);
    out[len] = '\0';
    return out;
}

static Value string_value(const char* text) {
    if (!text) return (Value){VAL_NIL, {.number = 0}};
    return (Value){VAL_OBJ, {.obj = (Obj*)copy_string(text, (int)strlen(text))}};
}

static bool parse_double_text(const char* text, double* out) {
    if (!text || !out) return false;
    while (*text && isspace((unsigned char)*text)) text++;
    errno = 0;
    char* end = NULL;
    double n = strtod(text, &end);
    if (errno != 0 || end == text) return false;
    while (*end && isspace((unsigned char)*end)) end++;
    if (*end != '\0') return false;
    *out = n;
    return true;
}

static void sleep_ms_interval(int64_t ms) {
    if (ms <= 0) return;
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000);
    ts.tv_nsec = (long)((ms % 1000) * 1000000LL);
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

static bool parse_positive_int(const char* text, int* out) {
    if (!text || !*text || !out) return false;
    for (const char* p = text; *p; p++) {
        if (!isdigit((unsigned char)*p)) return false;
    }
    long n = strtol(text, NULL, 10);
    if (n <= 0 || n > 1000000) return false;
    *out = (int)n;
    return true;
}

static bool parse_duration_millis(const char* text, int64_t* out_ms) {
    if (!text || !*text || !out_ms) return false;

    size_t len = strlen(text);
    int unit_mul = 0;
    if (len >= 2 && text[len - 2] == 'm' && text[len - 1] == 's') unit_mul = 1;
    else if (text[len - 1] == 's') unit_mul = 1000;
    else if (text[len - 1] == 'm') unit_mul = 60 * 1000;
    else if (text[len - 1] == 'h') unit_mul = 60 * 60 * 1000;
    else return false;

    char num_buf[32];
    size_t nlen = (unit_mul == 1) ? (len - 2) : (len - 1);
    if (nlen == 0 || nlen >= sizeof(num_buf)) return false;
    memcpy(num_buf, text, nlen);
    num_buf[nlen] = '\0';

    int n = 0;
    if (!parse_positive_int(num_buf, &n)) return false;
    *out_ms = (int64_t)n * (int64_t)unit_mul;
    return true;
}

static bool parse_cron_interval_millis(const char* spec, int64_t* out_ms) {
    if (!spec || !out_ms) return false;

    char* copy = dup_cstr(spec);
    if (!copy) return false;

    char* parts[8];
    int count = 0;
    char* tok = strtok(copy, " \t\r\n");
    while (tok && count < (int)(sizeof(parts) / sizeof(parts[0]))) {
        parts[count++] = tok;
        tok = strtok(NULL, " \t\r\n");
    }

    bool ok = false;
    int64_t out = 0;
    if (count == 2 && strcmp(parts[0], "@every") == 0) {
        ok = parse_duration_millis(parts[1], &out);
    } else if (count == 5 || count == 6) {
        const char* first = parts[0];
        int n = 0;
        int64_t unit_ms = (count == 6) ? 1000LL : 60000LL;
        if (strcmp(first, "*") == 0) {
            n = 1;
            ok = true;
        } else if (strncmp(first, "*/", 2) == 0) {
            ok = parse_positive_int(first + 2, &n);
        }
        if (ok) out = (int64_t)n * unit_ms;
    }

    free(copy);
    if (!ok || out <= 0) return false;
    *out_ms = out;
    return true;
}

static char* escape_json_alloc(const char* in) {
    if (!in) return NULL;
    size_t in_len = strlen(in);
    size_t cap = in_len * 2 + 32;
    char* out = (char*)malloc(cap);
    if (!out) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < in_len; i++) {
        unsigned char c = (unsigned char)in[i];
        if (j + 8 >= cap) {
            cap = cap * 2;
            char* grown = (char*)realloc(out, cap);
            if (!grown) {
                free(out);
                return NULL;
            }
            out = grown;
        }
        switch (c) {
            case '"': out[j++] = '\\'; out[j++] = '"'; break;
            case '\\': out[j++] = '\\'; out[j++] = '\\'; break;
            case '\b': out[j++] = '\\'; out[j++] = 'b'; break;
            case '\f': out[j++] = '\\'; out[j++] = 'f'; break;
            case '\n': out[j++] = '\\'; out[j++] = 'n'; break;
            case '\r': out[j++] = '\\'; out[j++] = 'r'; break;
            case '\t': out[j++] = '\\'; out[j++] = 't'; break;
            default:
                if (c < 0x20) {
                    j += (size_t)snprintf(out + j, cap - j, "\\u%04x", c);
                } else {
                    out[j++] = (char)c;
                }
                break;
        }
    }
    out[j] = '\0';
    return out;
}

static uint32_t rotl32(uint32_t x, int n) {
    return (x << n) | (x >> (32 - n));
}

static void sha1_digest(const uint8_t* data, size_t len, uint8_t out[20]) {
    uint64_t bit_len = (uint64_t)len * 8ULL;
    size_t padded_len = len + 1;
    while ((padded_len % 64) != 56) padded_len++;
    padded_len += 8;

    uint8_t* msg = (uint8_t*)calloc(1, padded_len);
    if (!msg) {
        memset(out, 0, 20);
        return;
    }
    memcpy(msg, data, len);
    msg[len] = 0x80;
    for (int i = 0; i < 8; i++) {
        msg[padded_len - 1 - i] = (uint8_t)((bit_len >> (8 * i)) & 0xff);
    }

    uint32_t h0 = 0x67452301;
    uint32_t h1 = 0xEFCDAB89;
    uint32_t h2 = 0x98BADCFE;
    uint32_t h3 = 0x10325476;
    uint32_t h4 = 0xC3D2E1F0;

    for (size_t off = 0; off < padded_len; off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) {
            size_t p = off + (size_t)i * 4;
            w[i] = ((uint32_t)msg[p] << 24) | ((uint32_t)msg[p + 1] << 16) |
                   ((uint32_t)msg[p + 2] << 8) | (uint32_t)msg[p + 3];
        }
        for (int i = 16; i < 80; i++) {
            w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }

        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; i++) {
            uint32_t f = 0, k = 0;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            uint32_t temp = rotl32(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotl32(b, 30);
            b = a;
            a = temp;
        }

        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }

    free(msg);

    uint32_t hs[5] = {h0, h1, h2, h3, h4};
    for (int i = 0; i < 5; i++) {
        out[i * 4 + 0] = (uint8_t)((hs[i] >> 24) & 0xff);
        out[i * 4 + 1] = (uint8_t)((hs[i] >> 16) & 0xff);
        out[i * 4 + 2] = (uint8_t)((hs[i] >> 8) & 0xff);
        out[i * 4 + 3] = (uint8_t)(hs[i] & 0xff);
    }
}

static size_t base64_encode_buf(const uint8_t* in, size_t in_len, char* out, size_t out_size) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (!in || !out || out_size == 0) return 0;
    size_t j = 0;
    for (size_t i = 0; i < in_len; i += 3) {
        uint32_t a = in[i];
        uint32_t b = (i + 1 < in_len) ? in[i + 1] : 0;
        uint32_t c = (i + 2 < in_len) ? in[i + 2] : 0;
        uint32_t n = (a << 16) | (b << 8) | c;
        char o0 = tbl[(n >> 18) & 63];
        char o1 = tbl[(n >> 12) & 63];
        char o2 = (i + 1 < in_len) ? tbl[(n >> 6) & 63] : '=';
        char o3 = (i + 2 < in_len) ? tbl[n & 63] : '=';
        if (j + 4 >= out_size) return 0;
        out[j++] = o0;
        out[j++] = o1;
        out[j++] = o2;
        out[j++] = o3;
    }
    if (j >= out_size) return 0;
    out[j] = '\0';
    return j;
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

static Value run_shell_capture(const char* cmd) {
    if (!cmd) return (Value){VAL_NIL, {.number = 0}};

    FILE* fp = popen(cmd, "r");
    if (!fp) return (Value){VAL_NIL, {.number = 0}};

    size_t cap = 2048;
    size_t len = 0;
    char* out = (char*)malloc(cap);
    if (!out) {
        pclose(fp);
        return (Value){VAL_NIL, {.number = 0}};
    }
    out[0] = '\0';

    char buf[512];
    while (fgets(buf, sizeof(buf), fp) != NULL) {
        size_t n = strlen(buf);
        if (len + n + 1 > cap) {
            cap = cap * 2 + n + 1;
            char* grown = (char*)realloc(out, cap);
            if (!grown) {
                free(out);
                pclose(fp);
                return (Value){VAL_NIL, {.number = 0}};
            }
            out = grown;
        }
        memcpy(out + len, buf, n);
        len += n;
        out[len] = '\0';
    }
    pclose(fp);

    ObjString* text = copy_string(out, (int)len);
    free(out);
    return (Value){VAL_OBJ, {.obj = (Obj*)text}};
}

static int run_shell_status(const char* cmd) {
    int status = system(cmd);
    if (status == -1) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

static Value run_sql_capture(const char* sql) {
    if (!sql) return (Value){VAL_NIL, {.number = 0}};

    char tmp_sql[] = "/tmp/viper-sql-XXXXXX";
    int fd = mkstemp(tmp_sql);
    if (fd < 0) return (Value){VAL_NIL, {.number = 0}};

    size_t n = strlen(sql);
    ssize_t wrote = write(fd, sql, n);
    close(fd);
    if (wrote < 0 || (size_t)wrote != n) {
        unlink(tmp_sql);
        return (Value){VAL_NIL, {.number = 0}};
    }

    char q_db[MAX_DB_PATH_LEN * 2];
    if (!shell_quote_double(g_db_path, q_db, sizeof(q_db))) {
        unlink(tmp_sql);
        return (Value){VAL_NIL, {.number = 0}};
    }

    char cmd[8192];
    snprintf(cmd, sizeof(cmd), "sqlite3 -batch %s < %s 2>&1", q_db, tmp_sql);
    Value out = run_shell_capture(cmd);
    unlink(tmp_sql);
    return out;
}

static void web_clear_registry(void) {
    for (int i = 0; i < MAX_WEB_ROUTES; i++) {
        if (g_web_routes[i].used && IS_OBJ(g_web_routes[i].handler)) {
            release_obj(AS_OBJ(g_web_routes[i].handler));
        }
        g_web_routes[i].used = false;
        g_web_routes[i].method[0] = '\0';
        g_web_routes[i].path[0] = '\0';
        g_web_routes[i].handler = (Value){VAL_NIL, {.number = 0}};
    }
    for (int i = 0; i < g_web_middleware_count; i++) {
        if (IS_OBJ(g_web_middlewares[i])) release_obj(AS_OBJ(g_web_middlewares[i]));
    }
    g_web_middleware_count = 0;
    for (int i = 0; i < MAX_WEB_STATICS; i++) {
        g_web_static_mounts[i].used = false;
        g_web_static_mounts[i].prefix[0] = '\0';
        g_web_static_mounts[i].dir[0] = '\0';
    }
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (g_ws_clients[i].used) {
            close(g_ws_clients[i].fd);
            g_ws_clients[i].used = false;
        }
        g_ws_clients[i].id = 0;
        g_ws_clients[i].fd = -1;
    }
    g_ws_next_client_id = 1;
    g_web_cors_origin[0] = '\0';
}

static void ai_clear_tool_registry(void) {
    for (int i = 0; i < g_ai_tool_count; i++) {
        if (IS_OBJ(g_ai_tools[i])) release_obj(AS_OBJ(g_ai_tools[i]));
        g_ai_tools[i] = (Value){VAL_NIL, {.number = 0}};
    }
    g_ai_tool_count = 0;
}

static int ws_find_free_slot(void) {
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (!g_ws_clients[i].used) return i;
    }
    return -1;
}

static int ws_find_slot_by_id(int client_id) {
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (g_ws_clients[i].used && g_ws_clients[i].id == client_id) return i;
    }
    return -1;
}

static int ws_register_client(int fd) {
    int slot = ws_find_free_slot();
    if (slot < 0) return -1;
    g_ws_clients[slot].used = true;
    g_ws_clients[slot].fd = fd;
    g_ws_clients[slot].id = g_ws_next_client_id++;
    if (g_ws_next_client_id < 1) g_ws_next_client_id = 1;
    return g_ws_clients[slot].id;
}

static void ws_unregister_client_by_id(int client_id) {
    int slot = ws_find_slot_by_id(client_id);
    if (slot < 0) return;
    close(g_ws_clients[slot].fd);
    g_ws_clients[slot].used = false;
    g_ws_clients[slot].id = 0;
    g_ws_clients[slot].fd = -1;
}

static bool ws_http_get_header(const char* req, const char* key, char* out, size_t out_size) {
    if (!req || !key || !out || out_size == 0) return false;
    out[0] = '\0';
    size_t key_len = strlen(key);
    const char* p = req;
    while (*p) {
        const char* line_end = strstr(p, "\r\n");
        if (!line_end) line_end = p + strlen(p);
        if (line_end == p) break;
        if ((size_t)(line_end - p) > key_len + 1 &&
            strncasecmp(p, key, key_len) == 0 && p[key_len] == ':') {
            const char* val = p + key_len + 1;
            while (*val == ' ' || *val == '\t') val++;
            size_t n = (size_t)(line_end - val);
            if (n >= out_size) n = out_size - 1;
            memcpy(out, val, n);
            out[n] = '\0';
            return true;
        }
        if (*line_end == '\0') break;
        p = line_end + 2;
    }
    return false;
}

static bool ws_make_accept_key(const char* client_key, char* out, size_t out_size) {
    if (!client_key || !out || out_size < 32) return false;
    char src[256];
    if (snprintf(src, sizeof(src), "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", client_key) >= (int)sizeof(src)) {
        return false;
    }
    uint8_t digest[20];
    sha1_digest((const uint8_t*)src, strlen(src), digest);
    return base64_encode_buf(digest, sizeof(digest), out, out_size) > 0;
}

static bool send_all(int fd, const void* buf, size_t len) {
    const uint8_t* p = (const uint8_t*)buf;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, p + sent, len - sent, 0);
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

static bool ws_send_frame(int fd, uint8_t opcode, const uint8_t* payload, size_t len) {
    if (fd < 0) return false;
    uint8_t header[10];
    size_t hlen = 0;
    header[hlen++] = (uint8_t)(0x80 | (opcode & 0x0f)); // FIN + opcode
    if (len < 126) {
        header[hlen++] = (uint8_t)len;
    } else if (len <= 0xffff) {
        header[hlen++] = 126;
        header[hlen++] = (uint8_t)((len >> 8) & 0xff);
        header[hlen++] = (uint8_t)(len & 0xff);
    } else {
        return false;
    }

    if (!send_all(fd, header, hlen)) return false;
    if (len > 0 && payload && !send_all(fd, payload, len)) return false;
    return true;
}

static bool ws_send_text_fd(int fd, const char* text) {
    if (!text) text = "";
    return ws_send_frame(fd, 0x1, (const uint8_t*)text, strlen(text));
}

static bool ws_read_exact(int fd, uint8_t* buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(fd, buf + got, len - got, 0);
        if (n <= 0) return false;
        got += (size_t)n;
    }
    return true;
}

static int ws_read_text_frame(int fd, char* out, size_t out_size, bool* out_closed) {
    if (out && out_size > 0) out[0] = '\0';
    if (out_closed) *out_closed = false;

    uint8_t h[2];
    if (!ws_read_exact(fd, h, 2)) return -1;
    uint8_t opcode = h[0] & 0x0f;
    bool masked = (h[1] & 0x80) != 0;
    uint64_t len = (uint64_t)(h[1] & 0x7f);

    if (len == 126) {
        uint8_t ext[2];
        if (!ws_read_exact(fd, ext, 2)) return -1;
        len = ((uint64_t)ext[0] << 8) | (uint64_t)ext[1];
    } else if (len == 127) {
        uint8_t ext[8];
        if (!ws_read_exact(fd, ext, 8)) return -1;
        len = 0;
        for (int i = 0; i < 8; i++) len = (len << 8) | ext[i];
    }

    if (len > 65535) return -1;

    uint8_t mask[4] = {0, 0, 0, 0};
    if (masked && !ws_read_exact(fd, mask, 4)) return -1;

    uint8_t* payload = NULL;
    if (len > 0) {
        payload = (uint8_t*)malloc((size_t)len + 1);
        if (!payload) return -1;
        if (!ws_read_exact(fd, payload, (size_t)len)) {
            free(payload);
            return -1;
        }
        if (masked) {
            for (size_t i = 0; i < (size_t)len; i++) payload[i] ^= mask[i % 4];
        }
        payload[len] = '\0';
    }

    if (opcode == 0x8) {
        if (out_closed) *out_closed = true;
        free(payload);
        return 0;
    }
    if (opcode == 0x9) { // ping -> pong
        (void)ws_send_frame(fd, 0xA, payload, (size_t)len);
        free(payload);
        return 0;
    }
    if (opcode != 0x1) { // only text supported
        free(payload);
        return 0;
    }

    if (out && out_size > 0 && payload) {
        size_t n = (size_t)len;
        if (n >= out_size) n = out_size - 1;
        memcpy(out, payload, n);
        out[n] = '\0';
        free(payload);
        return (int)n;
    }

    free(payload);
    return 0;
}

static int web_find_route(const char* method, const char* path) {
    for (int i = 0; i < MAX_WEB_ROUTES; i++) {
        if (!g_web_routes[i].used) continue;
        if (strcmp(g_web_routes[i].method, method) != 0) continue;
        if (strcmp(g_web_routes[i].path, path) != 0) continue;
        return i;
    }
    return -1;
}

static int web_find_route_slot(void) {
    for (int i = 0; i < MAX_WEB_ROUTES; i++) {
        if (!g_web_routes[i].used) return i;
    }
    return -1;
}

static const char* web_guess_content_type(const char* path) {
    const char* dot = strrchr(path, '.');
    if (!dot) return "text/plain; charset=utf-8";
    if (strcmp(dot, ".html") == 0 || strcmp(dot, ".htm") == 0) return "text/html; charset=utf-8";
    if (strcmp(dot, ".css") == 0) return "text/css; charset=utf-8";
    if (strcmp(dot, ".js") == 0) return "application/javascript; charset=utf-8";
    if (strcmp(dot, ".json") == 0) return "application/json; charset=utf-8";
    if (strcmp(dot, ".png") == 0) return "image/png";
    if (strcmp(dot, ".jpg") == 0 || strcmp(dot, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(dot, ".gif") == 0) return "image/gif";
    if (strcmp(dot, ".svg") == 0) return "image/svg+xml";
    return "text/plain; charset=utf-8";
}

static bool read_file_alloc(const char* path, char** out, size_t* out_len) {
    *out = NULL;
    *out_len = 0;
    FILE* fp = fopen(path, "rb");
    if (!fp) return false;
    if (fseek(fp, 0L, SEEK_END) != 0) {
        fclose(fp);
        return false;
    }
    long n = ftell(fp);
    if (n < 0) {
        fclose(fp);
        return false;
    }
    rewind(fp);
    char* buf = (char*)malloc((size_t)n + 1);
    if (!buf) {
        fclose(fp);
        return false;
    }
    size_t read_n = fread(buf, 1, (size_t)n, fp);
    fclose(fp);
    buf[read_n] = '\0';
    *out = buf;
    *out_len = read_n;
    return true;
}

static void parse_http_request_line(const char* req, char* method, size_t method_size,
                                    char* path, size_t path_size) {
    if (method_size > 0) method[0] = '\0';
    if (path_size > 0) path[0] = '\0';
    if (!req) return;
    sscanf(req, "%15s %255s", method, path);
    if (path[0] == '\0') snprintf(path, path_size, "/");
}

static bool web_try_static_file(const char* req_path, char** out_body, const char** out_type) {
    for (int i = 0; i < MAX_WEB_STATICS; i++) {
        if (!g_web_static_mounts[i].used) continue;
        const char* prefix = g_web_static_mounts[i].prefix;
        size_t pfx_len = strlen(prefix);
        if (strncmp(req_path, prefix, pfx_len) != 0) continue;
        const char* rel = req_path + pfx_len;
        if (*rel == '\0') rel = "/index.html";

        char file_path[PATH_MAX];
        if (snprintf(file_path, sizeof(file_path), "%s%s", g_web_static_mounts[i].dir, rel) >= (int)sizeof(file_path)) {
            continue;
        }
        size_t body_len = 0;
        if (read_file_alloc(file_path, out_body, &body_len)) {
            *out_type = web_guess_content_type(file_path);
            return true;
        }
    }
    return false;
}

static void web_send_response(int client_fd, int code, const char* content_type, const char* body) {
    const char* reason = "OK";
    if (code == 404) reason = "Not Found";
    else if (code >= 500) reason = "Internal Server Error";
    if (!content_type) content_type = "text/plain; charset=utf-8";
    if (!body) body = "";

    char header[2048];
    int n = snprintf(header, sizeof(header),
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n"
                     "%s%s%s"
                     "\r\n",
                     code, reason, content_type, strlen(body),
                     g_web_cors_origin[0] ? "Access-Control-Allow-Origin: " : "",
                     g_web_cors_origin[0] ? g_web_cors_origin : "",
                     g_web_cors_origin[0] ? "\r\n" : "");
    send(client_fd, header, (size_t)n, 0);
    if (body[0] != '\0') {
        send(client_fd, body, strlen(body), 0);
    }
}

ObjString* copy_string(const char* chars, int length) {
    uint32_t hash = hash_string(chars, length);
    
    char* heapChars = malloc(length + 1);
    memcpy(heapChars, chars, length);
    heapChars[length] = '\0';
    
    ObjString* string = (ObjString*)allocate_obj(sizeof(ObjString), OBJ_STRING);
    string->length = length;
    string->chars = heapChars;
    string->hash = hash;
    return string;
}

ObjFunction* new_function(const char* name, int name_len, int arity) {
    ObjFunction* fn = (ObjFunction*)allocate_obj(sizeof(ObjFunction), OBJ_FUNCTION);
    fn->arity = arity;
    fn->name = name;
    fn->name_len = name_len;
    fn->base_reg = 0;
    init_chunk(&fn->chunk);
    return fn;
}

ObjStruct* new_struct(const char* name, int name_len, int field_count, const char** fields, int* field_lens) {
    ObjStruct* st = (ObjStruct*)allocate_obj(sizeof(ObjStruct), OBJ_STRUCT);
    st->name = name;
    st->name_len = name_len;
    st->field_count = field_count;
    st->field_names = fields;
    st->field_name_lens = field_lens;
    return st;
}

ObjInstance* new_instance(ObjStruct* klass) {
    ObjInstance* instance = (ObjInstance*)allocate_obj(sizeof(ObjInstance), OBJ_INSTANCE);
    instance->klass = klass;
    instance->fields = malloc(sizeof(Value) * klass->field_count);
    for (int i = 0; i < klass->field_count; i++) {
        instance->fields[i] = (Value){VAL_NIL, {.number = 0}};
    }
    return instance;
}

ObjDlHandle* new_dl_handle(void* handle) {
    ObjDlHandle* dl = (ObjDlHandle*)allocate_obj(sizeof(ObjDlHandle), OBJ_DL_HANDLE);
    dl->handle = handle;
    return dl;
}

ObjDynamicFunction* new_dynamic_function(void* fn_ptr, const char* signature, int sig_len, void* cif, void** arg_types, void* return_type) {
    ObjDynamicFunction* dyn = (ObjDynamicFunction*)allocate_obj(sizeof(ObjDynamicFunction), OBJ_DYNAMIC_FUNC);
    dyn->fn_ptr = fn_ptr;
    dyn->signature = signature;
    dyn->sig_len = sig_len;
    dyn->cif = cif;
    dyn->arg_types = arg_types;
    dyn->return_type = return_type;
    dyn->return_type = return_type;
    return dyn;
}

ObjPointer* new_pointer(void* ptr) {
    ObjPointer* p = (ObjPointer*)allocate_obj(sizeof(ObjPointer), OBJ_POINTER);
    p->ptr = ptr;
    return p;
}

ObjThread* new_thread(pthread_t thread) {
    ObjThread* t = (ObjThread*)allocate_obj(sizeof(ObjThread), OBJ_THREAD);
    t->thread = thread;
    t->result = (Value){VAL_NIL, {.number = 0}};
    t->finished = false;
    t->joined = false;
    return t;
}

ObjArray* new_array() {
    ObjArray* arr = (ObjArray*)allocate_obj(sizeof(ObjArray), OBJ_ARRAY);
    arr->elements = NULL;
    arr->count = 0;
    arr->capacity = 0;
    return arr;
}

void array_append(ObjArray* array, Value value) {
    if (array->capacity < array->count + 1) {
        int oldCapacity = array->capacity;
        array->capacity = oldCapacity < 8 ? 8 : oldCapacity * 2;
        array->elements = realloc(array->elements, sizeof(Value) * array->capacity);
    }
    array->elements[array->count] = value;
    array->count++;
    
    // If the value is an object, we should retain it since the array now holds a reference
    if (IS_OBJ(value)) {
        retain_obj(AS_OBJ(value));
    }
}

void retain_obj(Obj* obj) {
    if (obj == NULL) return;
    obj->ref_count++;
}

void release_obj(Obj* obj) {
    if (obj == NULL) return;
    obj->ref_count--;
    
    if (obj->ref_count <= 0) {
        if (obj->type == OBJ_STRING) {
            ObjString* string = (ObjString*)obj;
            free(string->chars);
        } else if (obj->type == OBJ_FUNCTION) {
            ObjFunction* fn = (ObjFunction*)obj;
            free(fn->chunk.code);
            free(fn->chunk.constants);
        } else if (obj->type == OBJ_STRUCT) {
            ObjStruct* st = (ObjStruct*)obj;
            free((void*)st->field_names);
            free(st->field_name_lens);
        } else if (obj->type == OBJ_INSTANCE) {
            ObjInstance* instance = (ObjInstance*)obj;
            for(int i=0; i<instance->klass->field_count; i++) {
                if(IS_OBJ(instance->fields[i])) release_obj(AS_OBJ(instance->fields[i]));
            }
            free(instance->fields);
        } else if (obj->type == OBJ_ARRAY) {
            ObjArray* arr = (ObjArray*)obj;
            for(int i=0; i < arr->count; i++) {
                if(IS_OBJ(arr->elements[i])) release_obj(AS_OBJ(arr->elements[i]));
            }
            free(arr->elements);
        } else if (obj->type == OBJ_DL_HANDLE) {
            ObjDlHandle* dl = (ObjDlHandle*)obj;
            if (dl->handle) dlclose(dl->handle);
        } else if (obj->type == OBJ_DYNAMIC_FUNC) {
            ObjDynamicFunction* dyn = (ObjDynamicFunction*)obj;
            free((void*)dyn->signature);
            free(dyn->cif);
            free(dyn->arg_types);
            // return_type points to static ffi_type, so we don't free it
        } else if (obj->type == OBJ_THREAD) {
            ObjThread* t = (ObjThread*)obj;
            // Native thread handle management
            if (!t->joined) {
                pthread_detach(t->thread); // Detach if abandoned by GC before await
            }
        }
        free(obj);
    }
}

void print_value(Value value) {
    switch (value.type) {
        case VAL_NUMBER:
            printf("%.2f", value.as.number);
            break;
        case VAL_BOOL:
            printf("%s", value.as.boolean ? "true" : "false");
            break;
        case VAL_NIL:
            printf("nil");
            break;
        case VAL_OBJ:
            switch (AS_OBJ(value)->type) {
            case OBJ_STRING:
                printf("%s", AS_STRING(value)->chars);
                break;
            case OBJ_ARRAY: {
                ObjArray* arr = (ObjArray*)AS_OBJ(value);
                printf("[");
                for (int i=0; i<arr->count; i++) {
                    print_value(arr->elements[i]);
                    if (i < arr->count - 1) printf(", ");
                }
                printf("]");
                break;
            }
            case OBJ_FUNCTION:
                printf("<fn %.*s>", AS_FUNCTION(value)->name_len, AS_FUNCTION(value)->name);
                break;
            case OBJ_STRUCT:
                printf("<st %.*s>", ((ObjStruct*)AS_OBJ(value))->name_len, ((ObjStruct*)AS_OBJ(value))->name);
                break;
            case OBJ_INSTANCE:
                printf("<%.*s instance>", ((ObjInstance*)AS_OBJ(value))->klass->name_len, ((ObjInstance*)AS_OBJ(value))->klass->name);
                break;
            case OBJ_DL_HANDLE:
                printf("<dl_handle %p>", ((ObjDlHandle*)AS_OBJ(value))->handle);
                break;
            case OBJ_DYNAMIC_FUNC:
                printf("<dyn_fn %.*s %p>", ((ObjDynamicFunction*)AS_OBJ(value))->sig_len, ((ObjDynamicFunction*)AS_OBJ(value))->signature, ((ObjDynamicFunction*)AS_OBJ(value))->fn_ptr);
                break;
            case OBJ_POINTER:
                printf("<ptr %p>", ((ObjPointer*)AS_OBJ(value))->ptr);
                break;
            case OBJ_THREAD:
                printf("<thread %p>", (void*)((ObjThread*)AS_OBJ(value))->thread);
                break;
            default: printf("<obj %d>", AS_OBJ(value)->type); break;
            }
            break;
    }
}

static Value native_print(int argCount, Value* args) {
    for (int i=0; i<argCount; i++) {
        print_value(args[i]);
        if (i < argCount - 1) printf(" ");
    }
    printf("\n");
    return (Value){VAL_NIL, {.number = 0}};
}

// ---- Native Registry ----
typedef struct {
    const char* name;
    NativeFn function;
} NativeEntry;

#define MAX_NATIVES 256
static NativeEntry native_registry[MAX_NATIVES];
static int _native_count = 0;

void push_native(const char* name, NativeFn function) {
    if (_native_count >= MAX_NATIVES) return;
    native_registry[_native_count].name = name;
    native_registry[_native_count].function = function;
    _native_count++;
}

int find_native_index(const char* name, int length) {
    for (int i = 0; i < _native_count; i++) {
        if (strlen(native_registry[i].name) == (size_t)length &&
            memcmp(native_registry[i].name, name, length) == 0) {
            return i;
        }
    }
    
    return -1;
}

NativeFn get_native_by_index(int index) {
    if (index < 0 || index >= _native_count) return NULL;
    return native_registry[index].function;
}

const char* get_native_name(int index) {
    if (index < 0 || index >= _native_count) return "unknown";
    return native_registry[index].name;
}

int native_count() {
    return _native_count;
}

// Global thread-safe structure for passing panics up to 'recover'
__thread const char* last_panic_msg = NULL;

static Value native_panic(int argCount, Value* args) {
    if (argCount != 1 || !IS_OBJ(args[0]) || AS_OBJ(args[0])->type != OBJ_STRING) {
        last_panic_msg = "Unknown fatal error (no message provided to panic).";
        return (Value){VAL_NIL, {.number = 0}};
    }
    const char* msg = AS_STRING(args[0])->chars;
    
    // Set the TLS variable for `recover` and the VM interceptor
    last_panic_msg = msg;
    
    return (Value){VAL_NIL, {.number = 0}};
}

static Value native_recover(int argCount, Value* args) {
    (void)argCount; (void)args;
    if (last_panic_msg) {
        ObjString* msg_obj = copy_string(last_panic_msg, strlen(last_panic_msg));
        last_panic_msg = NULL; // Consume
        return (Value){VAL_OBJ, {.obj = (Obj*)msg_obj}};
    }
    return (Value){VAL_NIL, {.number = 0}};
}

static Value native_sh(int argCount, Value* args) {
    if (argCount != 1 || !is_string_value(args[0])) {
        return (Value){VAL_NIL, {.number = 0}};
    }
    const char* cmd = AS_STRING(args[0])->chars;

    char tmp_out[] = "/tmp/viper-sh-out-XXXXXX";
    char tmp_err[] = "/tmp/viper-sh-err-XXXXXX";
    int out_fd = mkstemp(tmp_out);
    int err_fd = mkstemp(tmp_err);
    if (out_fd < 0 || err_fd < 0) {
        if (out_fd >= 0) { close(out_fd); unlink(tmp_out); }
        if (err_fd >= 0) { close(err_fd); unlink(tmp_err); }
        return (Value){VAL_NIL, {.number = 0}};
    }
    close(out_fd);
    close(err_fd);

    char q_cmd[MAX_NATIVE_TEXT * 2];
    char q_out[PATH_MAX * 2];
    char q_err[PATH_MAX * 2];
    if (!shell_quote_double(cmd, q_cmd, sizeof(q_cmd)) ||
        !shell_quote_double(tmp_out, q_out, sizeof(q_out)) ||
        !shell_quote_double(tmp_err, q_err, sizeof(q_err))) {
        unlink(tmp_out);
        unlink(tmp_err);
        return (Value){VAL_NIL, {.number = 0}};
    }

    char run_cmd[MAX_NATIVE_TEXT * 4];
    snprintf(run_cmd, sizeof(run_cmd), "/bin/sh -c %s > %s 2> %s", q_cmd, q_out, q_err);
    int code = run_shell_status(run_cmd);

    char* out_text = NULL;
    char* err_text = NULL;
    size_t out_len = 0;
    size_t err_len = 0;
    if (!read_file_alloc(tmp_out, &out_text, &out_len)) out_text = dup_cstr("");
    if (!read_file_alloc(tmp_err, &err_text, &err_len)) err_text = dup_cstr("");
    unlink(tmp_out);
    unlink(tmp_err);

    if (!out_text || !err_text) {
        free(out_text);
        free(err_text);
        return (Value){VAL_NIL, {.number = 0}};
    }

    char* out_json = escape_json_alloc(out_text);
    char* err_json = escape_json_alloc(err_text);
    free(out_text);
    free(err_text);
    if (!out_json || !err_json) {
        free(out_json);
        free(err_json);
        return (Value){VAL_NIL, {.number = 0}};
    }

    size_t cap = strlen(out_json) + strlen(err_json) + 128;
    char* json = (char*)malloc(cap);
    if (!json) {
        free(out_json);
        free(err_json);
        return (Value){VAL_NIL, {.number = 0}};
    }
    snprintf(json, cap, "{\"stdout\":\"%s\",\"stderr\":\"%s\",\"code\":%d}", out_json, err_json, code);
    Value res = string_value(json);
    free(json);
    free(out_json);
    free(err_json);
    return res;
}

static Value native_read(int argCount, Value* args) {
    if (argCount != 1 || !IS_OBJ(args[0]) || AS_OBJ(args[0])->type != OBJ_STRING) {
        return (Value){VAL_NIL, {.number = 0}};
    }
    const char* path = AS_STRING(args[0])->chars;
    FILE* fp = fopen(path, "r");
    if (!fp) return (Value){VAL_NIL, {.number = 0}};

    fseek(fp, 0L, SEEK_END);
    size_t fileSize = ftell(fp);
    rewind(fp);

    char* buffer = (char*)malloc(fileSize + 1);
    if (buffer == NULL) return (Value){VAL_NIL, {.number = 0}};
    size_t bytesRead = fread(buffer, sizeof(char), fileSize, fp);
    buffer[bytesRead] = '\0';
    fclose(fp);

    ObjString* res = copy_string(buffer, (int)bytesRead);
    free(buffer);
    return (Value){VAL_OBJ, {.obj = (Obj*)res}};
}

static Value native_ls(int argCount, Value* args) {
    const char* path = ".";
    if (argCount == 1 && IS_OBJ(args[0]) && AS_OBJ(args[0])->type == OBJ_STRING) {
        path = AS_STRING(args[0])->chars;
    }

    DIR* d = opendir(path);
    if (!d) return (Value){VAL_NIL, {.number = 0}};

    ObjArray* arr = new_array();
    struct dirent* dir;
    while ((dir = readdir(d)) != NULL) {
        if (strcmp(dir->d_name, ".") == 0 || strcmp(dir->d_name, "..") == 0) continue;
        array_append(arr, (Value){VAL_OBJ, {.obj = (Obj*)copy_string(dir->d_name, strlen(dir->d_name))}});
    }
    closedir(d);
    return (Value){VAL_OBJ, {.obj = (Obj*)arr}};
}

static Value native_clock(int argCount, Value* args) {
    (void)argCount; (void)args;
    return (Value){VAL_NUMBER, {.number = (double)clock() / CLOCKS_PER_SEC}};
}

static Value native_load_dl(int argCount, Value* args) {
    if (argCount != 1 || !IS_OBJ(args[0]) || AS_OBJ(args[0])->type != OBJ_STRING) {
        printf("Runtime Error: load_dl expects a string argument.\n");
        exit(1);
    }
    const char* path = AS_STRING(args[0])->chars;
    void* handle = dlopen(path, RTLD_LAZY);

    // Fallback: if path starts with "lib/std/", try system install path
    if (!handle && strncmp(path, "lib/std/", 8) == 0) {
        char sys_path[1024];
        snprintf(sys_path, sizeof(sys_path), "/usr/local/lib/viper/std/%s", path + 8);
        handle = dlopen(sys_path, RTLD_LAZY);
    }

    if (!handle) {
        printf("Runtime Error: Failed to load library '%s': %s\n", path, dlerror());
        exit(1);
    }
    return (Value){VAL_OBJ, {.obj = (Obj*)new_dl_handle(handle)}};
}

static ffi_type* parse_ffi_type(const char** p) {
    char c = **p;
    if (!c) return NULL;
    (*p)++;
    switch (c) {
        case 'v': return &ffi_type_void;
        case 'i': return &ffi_type_sint32;
        case 'I': return &ffi_type_sint64;
        case 'f': return &ffi_type_float;
        case 'd': return &ffi_type_double;
        case 'p': return &ffi_type_pointer;
        case 's': return &ffi_type_pointer; // String is passed as (char*) pointer
        case '{': {
             int cap = 4;
             int count = 0;
             ffi_type** elements = malloc((cap + 1) * sizeof(ffi_type*));
             while (**p && **p != '}') {
                 if (**p == ',') { (*p)++; continue; }
                 ffi_type* elem = parse_ffi_type(p);
                 if (!elem) { free(elements); return NULL; }
                 if (count == cap) {
                     cap *= 2;
                     elements = realloc(elements, (cap + 1) * sizeof(ffi_type*));
                 }
                 elements[count++] = elem;
             }
             if (**p == '}') (*p)++;
             elements[count] = NULL;
             
             ffi_type* st = malloc(sizeof(ffi_type));
             st->size = 0;
             st->alignment = 0;
             st->type = FFI_TYPE_STRUCT;
             st->elements = elements;
             return st;
        }
        default:  return NULL;
    }
}

static Value native_get_fn(int argCount, Value* args) {
    if (argCount != 3 || !IS_OBJ(args[0]) || AS_OBJ(args[0])->type != OBJ_DL_HANDLE ||
        !IS_OBJ(args[1]) || AS_OBJ(args[1])->type != OBJ_STRING || 
        !IS_OBJ(args[2]) || AS_OBJ(args[2])->type != OBJ_STRING) {
        printf("Runtime Error: get_fn expects (dl_handle, fn_name, signature).\n");
        exit(1);
    }
    
    ObjDlHandle* dl = (ObjDlHandle*)AS_OBJ(args[0]);
    const char* fn_name = AS_STRING(args[1])->chars;
    const char* signature = AS_STRING(args[2])->chars;
    int sig_len = AS_STRING(args[2])->length;
    
    void* fn_ptr = dlsym(dl->handle, fn_name);
    if (!fn_ptr) {
        printf("Runtime Error: Symbol '%s' not found.\n", fn_name);
        exit(1);
    }

    // Parse simple signature like "i,d->d"
    // Find separator "->"
    const char* sep = strstr(signature, "->");
    if (!sep) {
        printf("Runtime Error: Invalid signature format '%s'. Expected '...->type'.\n", signature);
        exit(1);
    }
    
    const char* r_ptr = sep + 2;
    ffi_type* rtype = parse_ffi_type(&r_ptr);
    if (!rtype) {
        printf("Runtime Error: Unknown return type in signature.\n");
        exit(1);
    }
    
    int arg_cap = 4;
    int num_args = 0;
    ffi_type** arg_types = malloc(arg_cap * sizeof(ffi_type*));
    
    const char* p = signature;
    while (p < sep) {
        if (*p == ',') { p++; continue; }
        ffi_type* t = parse_ffi_type(&p);
        if (!t) {
            printf("Runtime Error: Unknown argument type in signature.\n");
            exit(1);
        }
        if (num_args == arg_cap) {
            arg_cap *= 2;
            arg_types = realloc(arg_types, arg_cap * sizeof(ffi_type*));
        }
        arg_types[num_args++] = t;
    }
    
    ffi_cif* cif = malloc(sizeof(ffi_cif));
    if (ffi_prep_cif(cif, FFI_DEFAULT_ABI, num_args, rtype, arg_types) != FFI_OK) {
        printf("Runtime Error: ffi_prep_cif failed.\n");
        exit(1);
    }
    
    // Copy signature string for GC to track correctly
    char* sig_copy = malloc(sig_len + 1);
    memcpy(sig_copy, signature, sig_len);
    sig_copy[sig_len] = '\0';
    
    ObjDynamicFunction* dyn = new_dynamic_function(fn_ptr, sig_copy, sig_len, cif, (void**)arg_types, rtype);
    return (Value){VAL_OBJ, {.obj = (Obj*)dyn}};
}

static Value native_serve(int argCount, Value* args) {
    if (argCount != 2 || !IS_NUMBER(args[0]) || !IS_OBJ(args[1])) {
        printf("Runtime Error: serve expects (port: number, response: string|function).\n");
        exit(1);
    }
    int port = (int)args[0].as.number;
    const char* response_body = (AS_OBJ(args[1])->type == OBJ_STRING) ? AS_STRING(args[1])->chars : "";

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket failed"); exit(1); }
    
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("bind failed"); exit(1);
    }
    if (listen(server_fd, 3) < 0) { perror("listen failed"); exit(1); }

    printf("ViperLang serving on port %d...\n", port);
    
    char http_response[4096];
    int response_len = snprintf(http_response, sizeof(http_response),
        "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
        strlen(response_body), response_body);

    int is_callable = (args[1].type == VAL_OBJ && 
                       (AS_OBJ(args[1])->type == OBJ_FUNCTION || AS_OBJ(args[1])->type == OBJ_NATIVE));

    while (true) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd >= 0) {
            if (is_callable) {
                // Call the handler
                // For now, pass a dummy request string as first arg
                Value req_val = (Value){VAL_OBJ, {.obj = (Obj*)copy_string("{}", 2)}};
                Value res_val = call_viper_function(AS_FUNCTION(args[1]), 1, &req_val);
                
                const char* dynamic_res = "<h1>Service Unavailable</h1>";
                if (IS_OBJ(res_val) && AS_OBJ(res_val)->type == OBJ_STRING) {
                    dynamic_res = AS_STRING(res_val)->chars;
                }

                char dyn_http[8192];
                int dyn_len = snprintf(dyn_http, sizeof(dyn_http),
                    "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
                    strlen(dynamic_res), dynamic_res);
                send(client_fd, dyn_http, dyn_len, 0);
            } else {
                send(client_fd, http_response, response_len, 0);
            }
            close(client_fd);
        }
    }
    return (Value){VAL_NIL, {.number = 0}};
}

static Value native_fetch(int argCount, Value* args) {
    if (argCount != 1 || !is_string_value(args[0])) {
        printf("Runtime Error: fetch expects a URL string.\n");
        exit(1);
    }
    const char* url = AS_STRING(args[0])->chars;
    char q_url[MAX_NATIVE_TEXT];
    if (!shell_quote_double(url, q_url, sizeof(q_url))) {
        return (Value){VAL_NIL, {.number = 0}};
    }
    char cmd[MAX_NATIVE_TEXT];
    snprintf(cmd, sizeof(cmd), "curl -sL %s", q_url);
    return run_shell_capture(cmd);
}

static Value native_write(int argCount, Value* args) {
    if (argCount != 2 || !IS_OBJ(args[0]) || AS_OBJ(args[0])->type != OBJ_STRING ||
        !IS_OBJ(args[1]) || AS_OBJ(args[1])->type != OBJ_STRING) {
        printf("Runtime Error: write expects (path, content).\n");
        exit(1);
    }
    const char* path = AS_STRING(args[0])->chars;
    const char* content = AS_STRING(args[1])->chars;
    
    FILE* fp = fopen(path, "w");
    if (fp) {
        fputs(content, fp);
        fclose(fp);
    }
    return (Value){VAL_NIL, {.number = 0}};
}

static Value native_query(int argCount, Value* args) {
    if (argCount != 1 || !is_string_value(args[0])) {
        printf("Runtime Error: query expects an SQL string.\n");
        exit(1);
    }
    return run_sql_capture(AS_STRING(args[0])->chars);
}

static Value native_env(int argCount, Value* args) {
    if (argCount != 1 || !IS_OBJ(args[0]) || AS_OBJ(args[0])->type != OBJ_STRING) {
        printf("Runtime Error: env expects a key string.\n");
        exit(1);
    }
    const char* key = AS_STRING(args[0])->chars;
    const char* val = getenv(key);
    if (!val) return (Value){VAL_NIL, {.number = 0}};
    return (Value){VAL_OBJ, {.obj = (Obj*)copy_string(val, strlen(val))}};
}

static Value native_setenv(int argCount, Value* args) {
    if (argCount != 2 || !is_string_value(args[0]) || !is_string_value(args[1])) {
        return (Value){VAL_BOOL, {.boolean = false}};
    }
    const char* key = AS_STRING(args[0])->chars;
    const char* val = AS_STRING(args[1])->chars;
    return (Value){VAL_BOOL, {.boolean = setenv(key, val, 1) == 0}};
}

static Value native_args(int argCount, Value* args) {
    (void)argCount;
    (void)args;

    ObjArray* out = new_array();
    FILE* fp = fopen("/proc/self/cmdline", "rb");
    if (!fp) return (Value){VAL_OBJ, {.obj = (Obj*)out}};

    char* buf = NULL;
    size_t cap = 0;
    size_t len = 0;
    char tmp[256];
    size_t n = 0;
    while ((n = fread(tmp, 1, sizeof(tmp), fp)) > 0) {
        if (len + n + 1 > cap) {
            cap = cap == 0 ? 512 : cap * 2 + n;
            char* grown = (char*)realloc(buf, cap);
            if (!grown) {
                free(buf);
                fclose(fp);
                return (Value){VAL_OBJ, {.obj = (Obj*)out}};
            }
            buf = grown;
        }
        memcpy(buf + len, tmp, n);
        len += n;
    }
    fclose(fp);
    if (!buf || len == 0) {
        free(buf);
        return (Value){VAL_OBJ, {.obj = (Obj*)out}};
    }
    buf[len] = '\0';

    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (buf[i] == '\0') {
            if (i > start) {
                array_append(out, string_value(buf + start));
            }
            start = i + 1;
        }
    }
    free(buf);
    return (Value){VAL_OBJ, {.obj = (Obj*)out}};
}

static Value native_exit_now(int argCount, Value* args) {
    int code = 0;
    if (argCount >= 1 && IS_NUMBER(args[0])) code = (int)args[0].as.number;
    exit(code);
    return (Value){VAL_NIL, {.number = 0}};
}

static Value native_cwd(int argCount, Value* args) {
    (void)argCount;
    (void)args;
    char buf[PATH_MAX];
    if (!getcwd(buf, sizeof(buf))) return (Value){VAL_NIL, {.number = 0}};
    return string_value(buf);
}

static Value native_pid(int argCount, Value* args) {
    (void)argCount;
    (void)args;
    return (Value){VAL_NUMBER, {.number = (double)getpid()}};
}

static Value native_kill_proc(int argCount, Value* args) {
    if (argCount != 2 || !IS_NUMBER(args[0]) || !IS_NUMBER(args[1])) {
        return (Value){VAL_BOOL, {.boolean = false}};
    }
    int pid = (int)args[0].as.number;
    int sig = (int)args[1].as.number;
    return (Value){VAL_BOOL, {.boolean = kill(pid, sig) == 0}};
}

static Value native_sleep_ms(int argCount, Value* args) {
    if (argCount != 1 || !IS_NUMBER(args[0])) return (Value){VAL_NIL, {.number = 0}};
    double ms = args[0].as.number;
    if (ms < 0) ms = 0;
    sleep_ms_interval((int64_t)ms);
    return (Value){VAL_NIL, {.number = 0}};
}

static Value native_os_info(int argCount, Value* args) {
    (void)argCount;
    (void)args;
    struct utsname info;
    if (uname(&info) != 0) return (Value){VAL_NIL, {.number = 0}};

    long mem_total_kb = 0;
    long mem_available_kb = 0;
    FILE* mem = fopen("/proc/meminfo", "r");
    if (mem) {
        char line[256];
        while (fgets(line, sizeof(line), mem)) {
            if (sscanf(line, "MemTotal: %ld kB", &mem_total_kb) == 1) continue;
            if (sscanf(line, "MemAvailable: %ld kB", &mem_available_kb) == 1) continue;
        }
        fclose(mem);
    }

    char json[1200];
    snprintf(json, sizeof(json),
             "{\"os\":\"%s\",\"release\":\"%s\",\"arch\":\"%s\",\"pid\":%d,\"mem_total_kb\":%ld,\"mem_available_kb\":%ld}",
             info.sysname, info.release, info.machine, (int)getpid(), mem_total_kb, mem_available_kb);
    return string_value(json);
}

static Value native_os_cron(int argCount, Value* args) {
    if (argCount < 2 || !is_string_value(args[0]) ||
        !IS_OBJ(args[1]) || AS_OBJ(args[1])->type != OBJ_FUNCTION) {
        return (Value){VAL_BOOL, {.boolean = false}};
    }

    int64_t interval_ms = 0;
    if (!parse_cron_interval_millis(AS_STRING(args[0])->chars, &interval_ms)) {
        if (!g_os_features_notice_printed) {
            fprintf(stderr, "viper: os.cron supports cron first field ('*' or '*/N') and '@every 10s|5m|1h'.\n");
            g_os_features_notice_printed = true;
        }
        return (Value){VAL_BOOL, {.boolean = false}};
    }

    int max_runs = -1;
    if (argCount >= 3 && IS_NUMBER(args[2])) {
        max_runs = (int)args[2].as.number;
        if (max_runs < 0) max_runs = -1;
    }

    ObjFunction* callback = (ObjFunction*)AS_OBJ(args[1]);
    int runs = 0;
    while (max_runs < 0 || runs < max_runs) {
        sleep_ms_interval(interval_ms);
        (void)call_viper_function(callback, 0, NULL);
        runs++;
    }
    return (Value){VAL_NUMBER, {.number = (double)runs}};
}

static bool mkdir_recursive(const char* path) {
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

static Value native_read_bytes(int argCount, Value* args) {
    return native_read(argCount, args);
}

static Value native_append(int argCount, Value* args) {
    if (argCount != 2 || !is_string_value(args[0]) || !is_string_value(args[1])) {
        return (Value){VAL_BOOL, {.boolean = false}};
    }
    FILE* fp = fopen(AS_STRING(args[0])->chars, "a");
    if (!fp) return (Value){VAL_BOOL, {.boolean = false}};
    fputs(AS_STRING(args[1])->chars, fp);
    fclose(fp);
    return (Value){VAL_BOOL, {.boolean = true}};
}

static Value native_delete_file(int argCount, Value* args) {
    if (argCount != 1 || !is_string_value(args[0])) return (Value){VAL_BOOL, {.boolean = false}};
    return (Value){VAL_BOOL, {.boolean = unlink(AS_STRING(args[0])->chars) == 0}};
}

static Value native_copy_file(int argCount, Value* args) {
    if (argCount != 2 || !is_string_value(args[0]) || !is_string_value(args[1])) {
        return (Value){VAL_BOOL, {.boolean = false}};
    }
    const char* src = AS_STRING(args[0])->chars;
    const char* dst = AS_STRING(args[1])->chars;
    FILE* in = fopen(src, "rb");
    if (!in) return (Value){VAL_BOOL, {.boolean = false}};
    FILE* out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return (Value){VAL_BOOL, {.boolean = false}};
    }

    char buf[4096];
    size_t n = 0;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            ok = false;
            break;
        }
    }
    fclose(in);
    fclose(out);
    return (Value){VAL_BOOL, {.boolean = ok}};
}

static Value native_move_file(int argCount, Value* args) {
    if (argCount != 2 || !is_string_value(args[0]) || !is_string_value(args[1])) {
        return (Value){VAL_BOOL, {.boolean = false}};
    }
    return (Value){VAL_BOOL, {.boolean = rename(AS_STRING(args[0])->chars, AS_STRING(args[1])->chars) == 0}};
}

static Value native_exists(int argCount, Value* args) {
    if (argCount != 1 || !is_string_value(args[0])) return (Value){VAL_BOOL, {.boolean = false}};
    return (Value){VAL_BOOL, {.boolean = access(AS_STRING(args[0])->chars, F_OK) == 0}};
}

static Value native_is_dir(int argCount, Value* args) {
    if (argCount != 1 || !is_string_value(args[0])) return (Value){VAL_BOOL, {.boolean = false}};
    struct stat st;
    if (stat(AS_STRING(args[0])->chars, &st) != 0) return (Value){VAL_BOOL, {.boolean = false}};
    return (Value){VAL_BOOL, {.boolean = S_ISDIR(st.st_mode)}};
}

static Value native_mkdir(int argCount, Value* args) {
    if (argCount != 1 || !is_string_value(args[0])) return (Value){VAL_BOOL, {.boolean = false}};
    return (Value){VAL_BOOL, {.boolean = mkdir_recursive(AS_STRING(args[0])->chars)}};
}

static Value native_watch(int argCount, Value* args) {
    if (argCount < 2 || !is_string_value(args[0]) || !IS_OBJ(args[1]) || AS_OBJ(args[1])->type != OBJ_FUNCTION) {
        return (Value){VAL_BOOL, {.boolean = false}};
    }
    const char* path = AS_STRING(args[0])->chars;
    ObjFunction* cb = (ObjFunction*)AS_OBJ(args[1]);

    struct stat st;
    time_t last_mtime = 0;
    if (stat(path, &st) == 0) last_mtime = st.st_mtime;

    // Poll-based watcher fallback: wait up to ~60s and trigger callback on first change.
    for (int i = 0; i < 120; i++) {
        usleep(500000);
        struct stat cur;
        if (stat(path, &cur) != 0) continue;
        if (last_mtime == 0) {
            last_mtime = cur.st_mtime;
            continue;
        }
        if (cur.st_mtime != last_mtime) {
            last_mtime = cur.st_mtime;
            Value arg = string_value(path);
            (void)call_viper_function(cb, 1, &arg);
            return (Value){VAL_BOOL, {.boolean = true}};
        }
    }
    return (Value){VAL_BOOL, {.boolean = false}};
}

static Value native_stream_read(int argCount, Value* args) {
    if (argCount < 1 || !is_string_value(args[0])) return (Value){VAL_NIL, {.number = 0}};
    const char* path = AS_STRING(args[0])->chars;
    FILE* fp = fopen(path, "rb");
    if (!fp) return (Value){VAL_NIL, {.number = 0}};

    bool has_cb = (argCount >= 2 && IS_OBJ(args[1]) && AS_OBJ(args[1])->type == OBJ_FUNCTION);
    ObjFunction* cb = has_cb ? (ObjFunction*)AS_OBJ(args[1]) : NULL;
    double total = 0.0;
    char chunk[1024];
    size_t n = 0;

    while ((n = fread(chunk, 1, sizeof(chunk), fp)) > 0) {
        total += (double)n;
        if (has_cb) {
            ObjString* s = copy_string(chunk, (int)n);
            Value one = (Value){VAL_OBJ, {.obj = (Obj*)s}};
            (void)call_viper_function(cb, 1, &one);
        }
    }
    fclose(fp);
    return (Value){VAL_NUMBER, {.number = total}};
}

static void translate_time_format(const char* in, char* out, size_t out_size) {
    size_t j = 0;
    for (size_t i = 0; in[i] != '\0' && j + 1 < out_size; ) {
        if (strncmp(in + i, "YYYY", 4) == 0) {
            if (j + 2 >= out_size) break;
            out[j++] = '%'; out[j++] = 'Y'; i += 4;
        } else if (strncmp(in + i, "MM", 2) == 0) {
            if (j + 2 >= out_size) break;
            out[j++] = '%'; out[j++] = 'm'; i += 2;
        } else if (strncmp(in + i, "DD", 2) == 0) {
            if (j + 2 >= out_size) break;
            out[j++] = '%'; out[j++] = 'd'; i += 2;
        } else if (strncmp(in + i, "HH", 2) == 0) {
            if (j + 2 >= out_size) break;
            out[j++] = '%'; out[j++] = 'H'; i += 2;
        } else if (strncmp(in + i, "mm", 2) == 0) {
            if (j + 2 >= out_size) break;
            out[j++] = '%'; out[j++] = 'M'; i += 2;
        } else if (strncmp(in + i, "ss", 2) == 0) {
            if (j + 2 >= out_size) break;
            out[j++] = '%'; out[j++] = 'S'; i += 2;
        } else {
            out[j++] = in[i++];
        }
    }
    out[j] = '\0';
}

static Value native_time_now(int argCount, Value* args) {
    (void)argCount;
    (void)args;
    return (Value){VAL_NUMBER, {.number = (double)now_ms()}};
}

static Value native_time_format(int argCount, Value* args) {
    if (argCount != 2 || !IS_NUMBER(args[0]) || !is_string_value(args[1])) {
        return (Value){VAL_NIL, {.number = 0}};
    }

    time_t ts = (time_t)(args[0].as.number / 1000.0);
    struct tm tm;
    localtime_r(&ts, &tm);

    char fmt[128];
    char out[256];
    translate_time_format(AS_STRING(args[1])->chars, fmt, sizeof(fmt));
    if (strftime(out, sizeof(out), fmt, &tm) == 0) return (Value){VAL_NIL, {.number = 0}};
    return string_value(out);
}

static Value native_time_parse(int argCount, Value* args) {
    if (argCount != 2 || !is_string_value(args[0]) || !is_string_value(args[1])) {
        return (Value){VAL_NIL, {.number = 0}};
    }

    char fmt[128];
    translate_time_format(AS_STRING(args[1])->chars, fmt, sizeof(fmt));
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    if (!strptime(AS_STRING(args[0])->chars, fmt, &tm)) return (Value){VAL_NIL, {.number = 0}};
    time_t t = mktime(&tm);
    return (Value){VAL_NUMBER, {.number = (double)t * 1000.0}};
}

static Value native_time_add(int argCount, Value* args) {
    if (argCount != 2 || !IS_NUMBER(args[0])) return (Value){VAL_NIL, {.number = 0}};
    double ts_ms = args[0].as.number;
    double delta_ms = 0.0;

    if (IS_NUMBER(args[1])) {
        delta_ms = args[1].as.number;
    } else if (is_string_value(args[1])) {
        char* spec = dup_cstr(AS_STRING(args[1])->chars);
        if (!spec) return (Value){VAL_NIL, {.number = 0}};

        char* tok = strtok(spec, ",");
        while (tok) {
            while (*tok == ' ') tok++;
            char* colon = strchr(tok, ':');
            if (colon) {
                *colon = '\0';
                const char* key = tok;
                const char* val = colon + 1;
                double n = 0.0;
                if (parse_double_text(val, &n)) {
                    if (strcmp(key, "days") == 0) delta_ms += n * 86400000.0;
                    else if (strcmp(key, "hours") == 0) delta_ms += n * 3600000.0;
                    else if (strcmp(key, "minutes") == 0) delta_ms += n * 60000.0;
                    else if (strcmp(key, "seconds") == 0) delta_ms += n * 1000.0;
                    else if (strcmp(key, "ms") == 0) delta_ms += n;
                }
            }
            tok = strtok(NULL, ",");
        }
        free(spec);
    }

    return (Value){VAL_NUMBER, {.number = ts_ms + delta_ms}};
}

static Value native_math_rand(int argCount, Value* args) {
    if (argCount != 2 || !IS_NUMBER(args[0]) || !IS_NUMBER(args[1])) return (Value){VAL_NIL, {.number = 0}};
    int min = (int)args[0].as.number;
    int max = (int)args[1].as.number;
    if (max < min) {
        int tmp = min;
        min = max;
        max = tmp;
    }
    if (max == min) return (Value){VAL_NUMBER, {.number = (double)min}};
    int r = rand() % (max - min + 1);
    return (Value){VAL_NUMBER, {.number = (double)(min + r)}};
}

static Value native_math_uuid(int argCount, Value* args) {
    (void)argCount;
    (void)args;
    unsigned char b[16];
    for (int i = 0; i < 16; i++) b[i] = (unsigned char)(rand() & 0xFF);
    b[6] = (unsigned char)((b[6] & 0x0F) | 0x40); // version 4
    b[8] = (unsigned char)((b[8] & 0x3F) | 0x80); // variant

    char out[37];
    snprintf(out, sizeof(out),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
             b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return string_value(out);
}

static Value native_math_round(int argCount, Value* args) {
    if (argCount != 2 || !IS_NUMBER(args[0]) || !IS_NUMBER(args[1])) return (Value){VAL_NIL, {.number = 0}};
    double value = args[0].as.number;
    int decimals = (int)args[1].as.number;
    if (decimals < 0) decimals = 0;
    if (decimals > 12) decimals = 12;
    double p = pow(10.0, (double)decimals);
    return (Value){VAL_NUMBER, {.number = round(value * p) / p}};
}

static Value native_math_hash(int argCount, Value* args) {
    if (argCount != 1) return (Value){VAL_NIL, {.number = 0}};
    if (is_string_value(args[0])) {
        ObjString* s = AS_STRING(args[0]);
        return (Value){VAL_NUMBER, {.number = (double)hash_string(s->chars, s->length)}};
    }
    if (IS_NUMBER(args[0])) {
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "%.15g", args[0].as.number);
        return (Value){VAL_NUMBER, {.number = (double)hash_string(tmp, (int)strlen(tmp))}};
    }
    return (Value){VAL_NIL, {.number = 0}};
}

static Value native_keys_method(int argCount, Value* args) {
    if (argCount != 1 || !IS_OBJ(args[0]) || AS_OBJ(args[0])->type != OBJ_INSTANCE) {
        return (Value){VAL_OBJ, {.obj = (Obj*)new_array()}};
    }
    ObjInstance* instance = (ObjInstance*)AS_OBJ(args[0]);
    ObjArray* keys = new_array();
    for (int i = 0; i < instance->klass->field_count; i++) {
        ObjString* s = copy_string(instance->klass->field_names[i], instance->klass->field_name_lens[i]);
        array_append(keys, (Value){VAL_OBJ, {.obj = (Obj*)s}});
    }
    return (Value){VAL_OBJ, {.obj = (Obj*)keys}};
}

static Value native_has_method(int argCount, Value* args) {
    if (argCount != 2 || !IS_OBJ(args[0]) || AS_OBJ(args[0])->type != OBJ_INSTANCE || !is_string_value(args[1])) {
        return (Value){VAL_BOOL, {.boolean = false}};
    }
    ObjInstance* instance = (ObjInstance*)AS_OBJ(args[0]);
    ObjString* prop = AS_STRING(args[1]);
    for (int i = 0; i < instance->klass->field_count; i++) {
        if (instance->klass->field_name_lens[i] == prop->length &&
            memcmp(instance->klass->field_names[i], prop->chars, prop->length) == 0) {
            return (Value){VAL_BOOL, {.boolean = true}};
        }
    }
    return (Value){VAL_BOOL, {.boolean = false}};
}

static int cache_find_index(const char* key) {
    for (int i = 0; i < MAX_CACHE_ENTRIES; i++) {
        if (!g_cache[i].used || !g_cache[i].key) continue;
        if (strcmp(g_cache[i].key, key) == 0) return i;
    }
    return -1;
}

static void cache_release_slot(int idx) {
    if (idx < 0 || idx >= MAX_CACHE_ENTRIES || !g_cache[idx].used) return;
    if (g_cache[idx].key) free(g_cache[idx].key);
    if (IS_OBJ(g_cache[idx].value)) release_obj(AS_OBJ(g_cache[idx].value));
    g_cache[idx].key = NULL;
    g_cache[idx].value = (Value){VAL_NIL, {.number = 0}};
    g_cache[idx].expire_at_ms = 0;
    g_cache[idx].used = false;
}

static void cache_prune_expired(void) {
    int64_t ts = now_ms();
    for (int i = 0; i < MAX_CACHE_ENTRIES; i++) {
        if (!g_cache[i].used) continue;
        if (g_cache[i].expire_at_ms > 0 && ts >= g_cache[i].expire_at_ms) {
            cache_release_slot(i);
        }
    }
}

static int cache_first_free_slot(void) {
    for (int i = 0; i < MAX_CACHE_ENTRIES; i++) {
        if (!g_cache[i].used) return i;
    }
    return -1;
}

static bool cache_pattern_match(const char* key, const char* pattern) {
    if (!pattern || pattern[0] == '\0' || strcmp(pattern, "*") == 0) return true;
    const char* star = strchr(pattern, '*');
    if (!star) return strcmp(key, pattern) == 0;

    size_t prefix_len = (size_t)(star - pattern);
    size_t suffix_len = strlen(star + 1);
    size_t key_len = strlen(key);
    if (key_len < prefix_len + suffix_len) return false;
    if (prefix_len > 0 && memcmp(key, pattern, prefix_len) != 0) return false;
    if (suffix_len > 0 && memcmp(key + key_len - suffix_len, star + 1, suffix_len) != 0) return false;
    return true;
}

static Value native_cache_set(int argCount, Value* args) {
    if (argCount < 2 || !is_string_value(args[0])) return (Value){VAL_BOOL, {.boolean = false}};
    cache_prune_expired();

    const char* key = AS_STRING(args[0])->chars;
    int idx = cache_find_index(key);
    if (idx < 0) idx = cache_first_free_slot();
    if (idx < 0) return (Value){VAL_BOOL, {.boolean = false}};

    if (g_cache[idx].used) cache_release_slot(idx);
    g_cache[idx].used = true;
    g_cache[idx].key = dup_cstr(key);
    g_cache[idx].value = args[1];
    if (IS_OBJ(args[1])) retain_obj(AS_OBJ(args[1]));

    g_cache[idx].expire_at_ms = 0;
    if (argCount >= 3 && IS_NUMBER(args[2]) && args[2].as.number > 0) {
        g_cache[idx].expire_at_ms = now_ms() + (int64_t)args[2].as.number;
    }
    return (Value){VAL_BOOL, {.boolean = true}};
}

static Value native_cache_get(int argCount, Value* args) {
    if (argCount != 1 || !is_string_value(args[0])) return (Value){VAL_NIL, {.number = 0}};
    cache_prune_expired();
    int idx = cache_find_index(AS_STRING(args[0])->chars);
    if (idx < 0) return (Value){VAL_NIL, {.number = 0}};
    return g_cache[idx].value;
}

static Value native_cache_delete(int argCount, Value* args) {
    if (argCount != 1 || !is_string_value(args[0])) return (Value){VAL_BOOL, {.boolean = false}};
    cache_prune_expired();
    int idx = cache_find_index(AS_STRING(args[0])->chars);
    if (idx < 0) return (Value){VAL_BOOL, {.boolean = false}};
    cache_release_slot(idx);
    return (Value){VAL_BOOL, {.boolean = true}};
}

static Value native_cache_has(int argCount, Value* args) {
    if (argCount != 1 || !is_string_value(args[0])) return (Value){VAL_BOOL, {.boolean = false}};
    cache_prune_expired();
    return (Value){VAL_BOOL, {.boolean = cache_find_index(AS_STRING(args[0])->chars) >= 0}};
}

static Value native_cache_increment(int argCount, Value* args) {
    if (argCount < 1 || !is_string_value(args[0])) return (Value){VAL_NIL, {.number = 0}};
    double amount = 1.0;
    if (argCount >= 2 && IS_NUMBER(args[1])) amount = args[1].as.number;

    cache_prune_expired();
    const char* key = AS_STRING(args[0])->chars;
    int idx = cache_find_index(key);
    if (idx < 0) {
        int slot = cache_first_free_slot();
        if (slot < 0) return (Value){VAL_NIL, {.number = 0}};
        g_cache[slot].used = true;
        g_cache[slot].key = dup_cstr(key);
        g_cache[slot].value = (Value){VAL_NUMBER, {.number = amount}};
        g_cache[slot].expire_at_ms = 0;
        return g_cache[slot].value;
    }

    if (!IS_NUMBER(g_cache[idx].value)) {
        g_cache[idx].value = (Value){VAL_NUMBER, {.number = 0}};
    }
    g_cache[idx].value.as.number += amount;
    return g_cache[idx].value;
}

static Value native_cache_clear(int argCount, Value* args) {
    (void)argCount;
    (void)args;
    for (int i = 0; i < MAX_CACHE_ENTRIES; i++) {
        cache_release_slot(i);
    }
    return (Value){VAL_NIL, {.number = 0}};
}

static Value native_cache_keys(int argCount, Value* args) {
    const char* pattern = "*";
    if (argCount >= 1 && is_string_value(args[0])) pattern = AS_STRING(args[0])->chars;
    cache_prune_expired();

    ObjArray* out = new_array();
    for (int i = 0; i < MAX_CACHE_ENTRIES; i++) {
        if (!g_cache[i].used || !g_cache[i].key) continue;
        if (!cache_pattern_match(g_cache[i].key, pattern)) continue;
        array_append(out, string_value(g_cache[i].key));
    }
    return (Value){VAL_OBJ, {.obj = (Obj*)out}};
}

// Helper to create an anonymous struct from a few key-value pairs
static ObjInstance* create_dynamic_instance(int field_count, const char** names, Value* values) {
    ObjStruct* klass = (ObjStruct*)allocate_obj(sizeof(ObjStruct), OBJ_STRUCT);
    klass->name = "JSONObj";
    klass->field_count = field_count;
    klass->field_names = malloc(sizeof(char*) * field_count);
    klass->field_name_lens = malloc(sizeof(int) * field_count);
    for (int i=0; i<field_count; i++) {
        klass->field_names[i] = strdup(names[i]);
        klass->field_name_lens[i] = strlen(names[i]);
    }

    ObjInstance* inst = (ObjInstance*)allocate_obj(sizeof(ObjInstance), OBJ_INSTANCE);
    inst->klass = klass;
    inst->fields = malloc(sizeof(Value) * field_count);
    for(int i=0; i<field_count; i++) inst->fields[i] = values[i];
    
    return inst;
}

static void skip_whitespace(const char** src) {
    while (**src == ' ' || **src == '\n' || **src == '\r' || **src == '\t') (*src)++;
}

static Value parse_json_value(const char** src);

static Value parse_json_object(const char** src) {
    (*src)++; // skip '{'
    const char* names[64];
    Value values[64];
    int count = 0;

    skip_whitespace(src);
    if (**src == '}') {
        (*src)++;
        return (Value){VAL_OBJ, {.obj = (Obj*)create_dynamic_instance(0, NULL, NULL)}};
    }

    while (**src != '\0') {
        skip_whitespace(src);
        if (**src != '"') break;
        (*src)++; // skip '"'
        const char* start = *src;
        while (**src != '"' && **src != '\0') (*src)++;
        int len = (int)(*src - start);
        char* name = malloc(len + 1);
        memcpy(name, start, len);
        name[len] = '\0';
        names[count] = name;
        (*src)++; // skip '"'

        skip_whitespace(src);
        if (**src == ':') (*src)++;
        skip_whitespace(src);

        values[count] = parse_json_value(src);
        count++;

        skip_whitespace(src);
        if (**src == ',') (*src)++;
        else if (**src == '}') {
            (*src)++;
            break;
        }
    }

    ObjInstance* inst = create_dynamic_instance(count, names, values);
    for (int i = 0; i < count; i++) free((void*)names[i]);
    return (Value){VAL_OBJ, {.obj = (Obj*)inst}};
}

static Value parse_json_array(const char** src) {
    (*src)++; // skip '['
    ObjArray* array = new_array();
    skip_whitespace(src);
    if (**src == ']') {
        (*src)++;
        return (Value){VAL_OBJ, {.obj = (Obj*)array}};
    }

    while (**src != '\0') {
        array_append(array, parse_json_value(src));
        skip_whitespace(src);
        if (**src == ',') (*src)++;
        else if (**src == ']') {
            (*src)++;
            break;
        }
    }
    return (Value){VAL_OBJ, {.obj = (Obj*)array}};
}

static Value parse_json_value(const char** src) {
    skip_whitespace(src);
    if (**src == '{') return parse_json_object(src);
    if (**src == '[') return parse_json_array(src);
    if (**src == '"') {
        (*src)++;
        const char* start = *src;
        while (**src != '"' && **src != '\0') (*src)++;
        int len = (int)(*src - start);
        ObjString* s = copy_string(start, len);
        (*src)++;
        return (Value){VAL_OBJ, {.obj = (Obj*)s}};
    }
    if ((**src >= '0' && **src <= '9') || **src == '-') {
        char* end;
        double val = strtod(*src, &end);
        *src = end;
        return (Value){VAL_NUMBER, {.number = val}};
    }
    if (strncmp(*src, "true", 4) == 0) { *src += 4; return (Value){VAL_BOOL, {.boolean = true}}; }
    if (strncmp(*src, "false", 5) == 0) { *src += 5; return (Value){VAL_BOOL, {.boolean = false}}; }
    if (strncmp(*src, "null", 4) == 0) { *src += 4; return (Value){VAL_NIL, {.number = 0}}; }
    
    return (Value){VAL_NIL, {.number = 0}};
}

static void stringify_value(Value val, char** buf, int* cap, int* len) {
    char temp[128];
    int needed = 0;

    if (IS_NUMBER(val)) {
        needed = snprintf(temp, 128, "%g", val.as.number);
    } else if (IS_BOOL(val)) {
        needed = snprintf(temp, 128, "%s", val.as.boolean ? "true" : "false");
    } else if (val.type == VAL_NIL) {
        needed = snprintf(temp, 128, "null");
    } else if (IS_OBJ(val)) {
        Obj* obj = AS_OBJ(val);
        if (obj->type == OBJ_STRING) {
            ObjString* s = (ObjString*)obj;
            needed = s->length + 2;
        } else if (obj->type == OBJ_INSTANCE) {
            ObjInstance* inst = (ObjInstance*)obj;
            // Rough estimation
            needed = 2 + (inst->klass->field_count * 20); 
        } else if (obj->type == OBJ_ARRAY) {
            ObjArray* arr = (ObjArray*)obj;
            needed = 2 + (arr->count * 10);
        }
    }

    if (*len + needed + 1024 >= *cap) {
        *cap = *cap * 2 + needed + 1024;
        *buf = realloc(*buf, *cap);
    }

    if (IS_NUMBER(val)) {
        *len += sprintf(*buf + *len, "%g", val.as.number);
    } else if (IS_BOOL(val)) {
        *len += sprintf(*buf + *len, "%s", val.as.boolean ? "true" : "false");
    } else if (val.type == VAL_NIL) {
        *len += sprintf(*buf + *len, "null");
    } else if (IS_OBJ(val)) {
        Obj* obj = AS_OBJ(val);
        if (obj->type == OBJ_STRING) {
            ObjString* s = (ObjString*)obj;
            *len += sprintf(*buf + *len, "\"%s\"", s->chars);
        } else if (obj->type == OBJ_INSTANCE) {
            ObjInstance* inst = (ObjInstance*)obj;
            (*buf)[(*len)++] = '{';
            for (int i = 0; i < inst->klass->field_count; i++) {
                if (i > 0) (*buf)[(*len)++] = ',';
                *len += sprintf(*buf + *len, "\"%s\":", inst->klass->field_names[i]);
                stringify_value(inst->fields[i], buf, cap, len);
            }
            (*buf)[(*len)++] = '}';
        } else if (obj->type == OBJ_ARRAY) {
            ObjArray* arr = (ObjArray*)obj;
            (*buf)[(*len)++] = '[';
            for (int i = 0; i < arr->count; i++) {
                if (i > 0) (*buf)[(*len)++] = ',';
                stringify_value(arr->elements[i], buf, cap, len);
            }
            (*buf)[(*len)++] = ']';
        }
    }
}

static Value native_json(int argCount, Value* args) {
    if (argCount != 1) return (Value){VAL_NIL, {.number = 0}};
    
    if (IS_OBJ(args[0]) && AS_OBJ(args[0])->type == OBJ_STRING) {
        // PARSE
        const char* src = AS_STRING(args[0])->chars;
        return parse_json_value(&src);
    } else {
        // STRINGIFY
        int cap = 1024;
        int len = 0;
        char* buf = malloc(cap);
        stringify_value(args[0], &buf, &cap, &len);
        buf[len] = '\0';
        ObjString* res = copy_string(buf, len);
        free(buf);
        return (Value){VAL_OBJ, {.obj = (Obj*)res}};
    }
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

static char* trim_inplace(char* s) {
    if (!s) return s;
    while (*s && isspace((unsigned char)*s)) s++;
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) {
        s[n - 1] = '\0';
        n--;
    }
    return s;
}

static bool is_safe_ident(const char* text) {
    if (!text || text[0] == '\0') return false;
    if (!(isalpha((unsigned char)text[0]) || text[0] == '_')) return false;
    for (size_t i = 1; text[i] != '\0'; i++) {
        unsigned char c = (unsigned char)text[i];
        if (!(isalnum(c) || c == '_')) return false;
    }
    return true;
}

static bool escape_sql_string(const char* in, char* out, size_t out_size) {
    if (!in || !out || out_size == 0) return false;
    size_t j = 0;
    for (size_t i = 0; in[i] != '\0'; i++) {
        char c = in[i];
        if (c == '\'') {
            if (j + 2 >= out_size) return false;
            out[j++] = '\'';
            out[j++] = '\'';
        } else {
            if (j + 1 >= out_size) return false;
            out[j++] = c;
        }
    }
    out[j] = '\0';
    return true;
}

static bool sql_value_literal(const char* raw, char* out, size_t out_size) {
    double n = 0.0;
    if (parse_double_text(raw, &n)) {
        return snprintf(out, out_size, "%.15g", n) > 0;
    }
    char escaped[2048];
    if (!escape_sql_string(raw, escaped, sizeof(escaped))) return false;
    return snprintf(out, out_size, "'%s'", escaped) > 0;
}

static bool output_has_sql_error(Value out) {
    if (!is_string_value(out)) return true;
    const char* text = AS_STRING(out)->chars;
    if (!text) return true;
    return strstr(text, "Error:") != NULL || strstr(text, "Parse error") != NULL;
}

static bool build_schema_sql(const char* table, const char* schema, char* sql, size_t sql_size) {
    if (!table || !schema || !is_safe_ident(table)) return false;
    size_t len = 0;
    if (!append_fmt(sql, sql_size, &len, "CREATE TABLE IF NOT EXISTS %s (id INTEGER PRIMARY KEY AUTOINCREMENT", table)) {
        return false;
    }

    char* copy = dup_cstr(schema);
    if (!copy) return false;
    char* tok = strtok(copy, ",");
    while (tok) {
        char* part = trim_inplace(tok);
        char* colon = strchr(part, ':');
        if (colon) {
            *colon = '\0';
            const char* key = trim_inplace(part);
            const char* typ = trim_inplace(colon + 1);
            if (is_safe_ident(key) && strcmp(key, "id") != 0) {
                const char* sql_type = "TEXT";
                if (strcmp(typ, "i") == 0) sql_type = "INTEGER";
                else if (strcmp(typ, "f") == 0) sql_type = "REAL";
                else if (strcmp(typ, "b") == 0) sql_type = "INTEGER";
                else if (strcmp(typ, "s") == 0) sql_type = "TEXT";
                if (!append_fmt(sql, sql_size, &len, ", %s %s", key, sql_type)) {
                    free(copy);
                    return false;
                }
            }
        }
        tok = strtok(NULL, ",");
    }
    free(copy);
    return append_fmt(sql, sql_size, &len, ");");
}

static bool build_data_sql_parts(const char* spec,
                                 char* cols, size_t cols_size,
                                 char* vals, size_t vals_size,
                                 char* sets, size_t sets_size,
                                 bool* out_has_id, int* out_id) {
    if (!spec || !cols || !vals || !sets || !out_has_id || !out_id) return false;
    cols[0] = '\0';
    vals[0] = '\0';
    sets[0] = '\0';
    *out_has_id = false;
    *out_id = 0;

    char* copy = dup_cstr(spec);
    if (!copy) return false;

    size_t cols_len = 0, vals_len = 0, sets_len = 0;
    bool first = true;
    char* tok = strtok(copy, ",");
    while (tok) {
        char* part = trim_inplace(tok);
        char* colon = strchr(part, ':');
        if (!colon) {
            tok = strtok(NULL, ",");
            continue;
        }
        *colon = '\0';
        char* key = trim_inplace(part);
        char* val = trim_inplace(colon + 1);
        if (!is_safe_ident(key)) {
            tok = strtok(NULL, ",");
            continue;
        }

        if (strcmp(key, "id") == 0) {
            double id_num = 0.0;
            if (parse_double_text(val, &id_num)) {
                *out_has_id = true;
                *out_id = (int)id_num;
            }
            tok = strtok(NULL, ",");
            continue;
        }

        char lit[2048];
        if (!sql_value_literal(val, lit, sizeof(lit))) {
            free(copy);
            return false;
        }

        if (!first) {
            if (!append_fmt(cols, cols_size, &cols_len, ", ") ||
                !append_fmt(vals, vals_size, &vals_len, ", ") ||
                !append_fmt(sets, sets_size, &sets_len, ", ")) {
                free(copy);
                return false;
            }
        }

        if (!append_fmt(cols, cols_size, &cols_len, "%s", key) ||
            !append_fmt(vals, vals_size, &vals_len, "%s", lit) ||
            !append_fmt(sets, sets_size, &sets_len, "%s=%s", key, lit)) {
            free(copy);
            return false;
        }
        first = false;
        tok = strtok(NULL, ",");
    }
    free(copy);
    return cols_len > 0;
}

static Value native_web_route(int argCount, Value* args) {
    if (argCount != 3 || !is_string_value(args[0]) || !is_string_value(args[1]) || !IS_OBJ(args[2])) {
        return (Value){VAL_BOOL, {.boolean = false}};
    }
    int slot = web_find_route_slot();
    if (slot < 0) return (Value){VAL_BOOL, {.boolean = false}};

    const char* path = AS_STRING(args[0])->chars;
    const char* method = AS_STRING(args[1])->chars;
    snprintf(g_web_routes[slot].path, sizeof(g_web_routes[slot].path), "%s", path);
    snprintf(g_web_routes[slot].method, sizeof(g_web_routes[slot].method), "%s", method);
    for (size_t i = 0; g_web_routes[slot].method[i] != '\0'; i++) {
        g_web_routes[slot].method[i] = (char)toupper((unsigned char)g_web_routes[slot].method[i]);
    }
    g_web_routes[slot].handler = args[2];
    g_web_routes[slot].used = true;
    retain_obj(AS_OBJ(args[2]));
    return (Value){VAL_BOOL, {.boolean = true}};
}

static Value native_web_middleware(int argCount, Value* args) {
    if (argCount != 1 || !IS_OBJ(args[0]) || AS_OBJ(args[0])->type != OBJ_FUNCTION) {
        return (Value){VAL_BOOL, {.boolean = false}};
    }
    if (g_web_middleware_count >= MAX_WEB_MIDDLEWARES) return (Value){VAL_BOOL, {.boolean = false}};
    g_web_middlewares[g_web_middleware_count++] = args[0];
    retain_obj(AS_OBJ(args[0]));
    return (Value){VAL_BOOL, {.boolean = true}};
}

static Value native_web_static(int argCount, Value* args) {
    if (argCount != 2 || !is_string_value(args[0]) || !is_string_value(args[1])) {
        return (Value){VAL_BOOL, {.boolean = false}};
    }
    for (int i = 0; i < MAX_WEB_STATICS; i++) {
        if (g_web_static_mounts[i].used) continue;
        snprintf(g_web_static_mounts[i].prefix, sizeof(g_web_static_mounts[i].prefix), "%s", AS_STRING(args[0])->chars);
        snprintf(g_web_static_mounts[i].dir, sizeof(g_web_static_mounts[i].dir), "%s", AS_STRING(args[1])->chars);
        g_web_static_mounts[i].used = true;
        return (Value){VAL_BOOL, {.boolean = true}};
    }
    return (Value){VAL_BOOL, {.boolean = false}};
}

static Value native_web_cors(int argCount, Value* args) {
    if (argCount != 1 || !is_string_value(args[0])) return (Value){VAL_BOOL, {.boolean = false}};
    snprintf(g_web_cors_origin, sizeof(g_web_cors_origin), "%s", AS_STRING(args[0])->chars);
    return (Value){VAL_BOOL, {.boolean = true}};
}

static Value native_web_ws_serve(int argCount, Value* args) {
    if (argCount != 2 || !IS_NUMBER(args[0]) ||
        !IS_OBJ(args[1]) || AS_OBJ(args[1])->type != OBJ_FUNCTION) {
        return (Value){VAL_BOOL, {.boolean = false}};
    }

    int port = (int)args[0].as.number;
    ObjFunction* on_msg = (ObjFunction*)AS_OBJ(args[1]);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) return (Value){VAL_BOOL, {.boolean = false}};
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);
    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        close(server_fd);
        return (Value){VAL_BOOL, {.boolean = false}};
    }
    if (listen(server_fd, 16) < 0) {
        close(server_fd);
        return (Value){VAL_BOOL, {.boolean = false}};
    }

    printf("ViperLang WebSocket server listening on port %d...\n", port);

    while (true) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) continue;

        char req[8192];
        int got = recv(client_fd, req, sizeof(req) - 1, 0);
        if (got <= 0) {
            close(client_fd);
            continue;
        }
        req[got] = '\0';

        char ws_key[256];
        if (!ws_http_get_header(req, "Sec-WebSocket-Key", ws_key, sizeof(ws_key))) {
            const char* bad_req = "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n";
            (void)send(client_fd, bad_req, strlen(bad_req), 0);
            close(client_fd);
            continue;
        }

        char accept_key[128];
        if (!ws_make_accept_key(ws_key, accept_key, sizeof(accept_key))) {
            close(client_fd);
            continue;
        }

        char resp[512];
        int resp_len = snprintf(resp, sizeof(resp),
                                "HTTP/1.1 101 Switching Protocols\r\n"
                                "Upgrade: websocket\r\n"
                                "Connection: Upgrade\r\n"
                                "Sec-WebSocket-Accept: %s\r\n\r\n",
                                accept_key);
        if (send(client_fd, resp, (size_t)resp_len, 0) < 0) {
            close(client_fd);
            continue;
        }

        int client_id = ws_register_client(client_fd);
        if (client_id < 0) {
            close(client_fd);
            continue;
        }

        for (;;) {
            bool closed = false;
            char msg[4096];
            int n = ws_read_text_frame(client_fd, msg, sizeof(msg), &closed);
            if (n < 0 || closed) break;
            if (n == 0) continue;

            Value in = string_value(msg);
            Value out = call_viper_function(on_msg, 1, &in);
            if (is_string_value(out)) {
                (void)ws_send_text_fd(client_fd, AS_STRING(out)->chars);
            }
        }

        ws_unregister_client_by_id(client_id);
    }

    return (Value){VAL_NIL, {.number = 0}};
}

static Value native_web_ws_send(int argCount, Value* args) {
    if (argCount != 2 || !IS_NUMBER(args[0]) || !is_string_value(args[1])) {
        return (Value){VAL_BOOL, {.boolean = false}};
    }
    int client_id = (int)args[0].as.number;
    int slot = ws_find_slot_by_id(client_id);
    if (slot < 0) return (Value){VAL_BOOL, {.boolean = false}};
    bool ok = ws_send_text_fd(g_ws_clients[slot].fd, AS_STRING(args[1])->chars);
    return (Value){VAL_BOOL, {.boolean = ok}};
}

static Value native_web_serve(int argCount, Value* args) {
    if (argCount < 1 || !IS_NUMBER(args[0])) {
        printf("Runtime Error: web.serve expects (port, handler?).\n");
        exit(1);
    }
    int port = (int)args[0].as.number;
    Value fallback = (argCount >= 2) ? args[1] : string_value("<h1>Viper Web</h1>");

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket failed"); exit(1); }
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("bind failed");
        exit(1);
    }
    if (listen(server_fd, 16) < 0) {
        perror("listen failed");
        exit(1);
    }

    printf("ViperLang web server listening on port %d...\n", port);

    while (true) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) continue;

        char req_buf[8192];
        int got = recv(client_fd, req_buf, sizeof(req_buf) - 1, 0);
        if (got <= 0) {
            close(client_fd);
            continue;
        }
        req_buf[got] = '\0';

        char method[16];
        char path[256];
        parse_http_request_line(req_buf, method, sizeof(method), path, sizeof(path));

        Value req_val = string_value(req_buf);
        for (int i = 0; i < g_web_middleware_count; i++) {
            if (!IS_OBJ(g_web_middlewares[i]) || AS_OBJ(g_web_middlewares[i])->type != OBJ_FUNCTION) continue;
            ObjFunction* mw = (ObjFunction*)AS_OBJ(g_web_middlewares[i]);
            (void)call_viper_function(mw, 1, &req_val);
        }

        char* static_body = NULL;
        const char* static_type = NULL;
        if (web_try_static_file(path, &static_body, &static_type)) {
            web_send_response(client_fd, 200, static_type, static_body);
            free(static_body);
            close(client_fd);
            continue;
        }

        int route_idx = web_find_route(method, path);
        if (route_idx >= 0) {
            Value handler = g_web_routes[route_idx].handler;
            if (IS_OBJ(handler) && AS_OBJ(handler)->type == OBJ_FUNCTION) {
                Value out = call_viper_function((ObjFunction*)AS_OBJ(handler), 1, &req_val);
                if (is_string_value(out)) {
                    web_send_response(client_fd, 200, "text/html; charset=utf-8", AS_STRING(out)->chars);
                } else {
                    web_send_response(client_fd, 200, "text/plain; charset=utf-8", "ok");
                }
            } else if (is_string_value(handler)) {
                web_send_response(client_fd, 200, "text/html; charset=utf-8", AS_STRING(handler)->chars);
            } else {
                web_send_response(client_fd, 200, "text/plain; charset=utf-8", "ok");
            }
            close(client_fd);
            continue;
        }

        if (IS_OBJ(fallback) && AS_OBJ(fallback)->type == OBJ_FUNCTION) {
            Value out = call_viper_function((ObjFunction*)AS_OBJ(fallback), 1, &req_val);
            if (is_string_value(out)) {
                web_send_response(client_fd, 200, "text/html; charset=utf-8", AS_STRING(out)->chars);
            } else {
                web_send_response(client_fd, 404, "text/plain; charset=utf-8", "not found");
            }
        } else if (is_string_value(fallback)) {
            web_send_response(client_fd, 200, "text/html; charset=utf-8", AS_STRING(fallback)->chars);
        } else {
            web_send_response(client_fd, 404, "text/plain; charset=utf-8", "not found");
        }
        close(client_fd);
    }
    return (Value){VAL_NIL, {.number = 0}};
}

static Value native_web_hash(int argCount, Value* args) {
    if (argCount < 1 || !is_string_value(args[0])) return (Value){VAL_NIL, {.number = 0}};
    ObjString* s = AS_STRING(args[0]);
    uint32_t h = hash_string(s->chars, s->length);
    char out[16];
    snprintf(out, sizeof(out), "%08x", h);
    return string_value(out);
}

static Value native_web_jwt_sign(int argCount, Value* args) {
    if (argCount != 2 || !is_string_value(args[0]) || !is_string_value(args[1])) {
        return (Value){VAL_NIL, {.number = 0}};
    }
    const char* payload = AS_STRING(args[0])->chars;
    const char* secret = AS_STRING(args[1])->chars;

    char sign_input[MAX_NATIVE_TEXT];
    snprintf(sign_input, sizeof(sign_input), "%s:%s", secret, payload);
    uint32_t sig = hash_string(sign_input, (int)strlen(sign_input));

    char token[MAX_NATIVE_TEXT];
    snprintf(token, sizeof(token), "v1.%08x.%s", sig, payload);
    return string_value(token);
}

static Value native_web_jwt_verify(int argCount, Value* args) {
    if (argCount != 2 || !is_string_value(args[0]) || !is_string_value(args[1])) {
        return (Value){VAL_NIL, {.number = 0}};
    }
    char* tok = dup_cstr(AS_STRING(args[0])->chars);
    if (!tok) return (Value){VAL_NIL, {.number = 0}};
    const char* secret = AS_STRING(args[1])->chars;

    char* p1 = strchr(tok, '.');
    if (!p1) {
        free(tok);
        return (Value){VAL_NIL, {.number = 0}};
    }
    *p1 = '\0';
    char* p2 = strchr(p1 + 1, '.');
    if (!p2) {
        free(tok);
        return (Value){VAL_NIL, {.number = 0}};
    }
    *p2 = '\0';
    const char* version = tok;
    const char* sig_hex = p1 + 1;
    const char* payload = p2 + 1;
    if (strcmp(version, "v1") != 0) {
        free(tok);
        return (Value){VAL_NIL, {.number = 0}};
    }

    char sign_input[MAX_NATIVE_TEXT];
    snprintf(sign_input, sizeof(sign_input), "%s:%s", secret, payload);
    uint32_t expected = hash_string(sign_input, (int)strlen(sign_input));
    char expected_hex[16];
    snprintf(expected_hex, sizeof(expected_hex), "%08x", expected);
    bool ok = strcmp(expected_hex, sig_hex) == 0;
    Value out = ok ? string_value(payload) : (Value){VAL_NIL, {.number = 0}};
    free(tok);
    return out;
}

static Value native_http_method(const char* method, int argCount, Value* args) {
    if (argCount < 1 || !is_string_value(args[0])) return (Value){VAL_NIL, {.number = 0}};
    char q_url[MAX_NATIVE_TEXT];
    if (!shell_quote_double(AS_STRING(args[0])->chars, q_url, sizeof(q_url))) {
        return (Value){VAL_NIL, {.number = 0}};
    }

    char cmd[8192];
    snprintf(cmd, sizeof(cmd), "curl -sL -X %s %s", method, q_url);
    if (argCount >= 2 && is_string_value(args[1])) {
        char q_data[MAX_NATIVE_TEXT];
        if (!shell_quote_double(AS_STRING(args[1])->chars, q_data, sizeof(q_data))) {
            return (Value){VAL_NIL, {.number = 0}};
        }
        strncat(cmd, " --data ", sizeof(cmd) - strlen(cmd) - 1);
        strncat(cmd, q_data, sizeof(cmd) - strlen(cmd) - 1);
    }
    return run_shell_capture(cmd);
}

static Value native_web_post(int argCount, Value* args) { return native_http_method("POST", argCount, args); }
static Value native_web_put(int argCount, Value* args) { return native_http_method("PUT", argCount, args); }
static Value native_web_patch(int argCount, Value* args) { return native_http_method("PATCH", argCount, args); }
static Value native_web_delete(int argCount, Value* args) { return native_http_method("DELETE", argCount, args); }

static Value native_web_download(int argCount, Value* args) {
    if (argCount != 2 || !is_string_value(args[0]) || !is_string_value(args[1])) {
        return (Value){VAL_BOOL, {.boolean = false}};
    }
    char q_url[MAX_NATIVE_TEXT];
    char q_path[MAX_NATIVE_TEXT];
    if (!shell_quote_double(AS_STRING(args[0])->chars, q_url, sizeof(q_url)) ||
        !shell_quote_double(AS_STRING(args[1])->chars, q_path, sizeof(q_path))) {
        return (Value){VAL_BOOL, {.boolean = false}};
    }
    char cmd[MAX_NATIVE_TEXT * 2];
    snprintf(cmd, sizeof(cmd), "curl -sL %s -o %s", q_url, q_path);
    return (Value){VAL_BOOL, {.boolean = run_shell_status(cmd) == 0}};
}

static Value native_web_upload(int argCount, Value* args) {
    if (argCount != 2 || !is_string_value(args[0]) || !is_string_value(args[1])) {
        return (Value){VAL_NIL, {.number = 0}};
    }
    char q_url[MAX_NATIVE_TEXT];
    char q_path[MAX_NATIVE_TEXT];
    if (!shell_quote_double(AS_STRING(args[0])->chars, q_url, sizeof(q_url)) ||
        !shell_quote_double(AS_STRING(args[1])->chars, q_path, sizeof(q_path))) {
        return (Value){VAL_NIL, {.number = 0}};
    }
    char cmd[MAX_NATIVE_TEXT * 2];
    snprintf(cmd, sizeof(cmd), "curl -sL -F file=@%s %s", q_path, q_url);
    return run_shell_capture(cmd);
}

static Value vdb_connect(int argCount, Value* args) {
    if (argCount != 1 || !is_string_value(args[0])) return (Value){VAL_BOOL, {.boolean = false}};
    const char* raw = AS_STRING(args[0])->chars;
    if (strncmp(raw, "sqlite://", 9) == 0) {
        raw += 9;
    } else if (strstr(raw, "://") != NULL) {
        return (Value){VAL_BOOL, {.boolean = false}};
    }
    if (raw[0] == '\0' || strlen(raw) >= sizeof(g_db_path)) return (Value){VAL_BOOL, {.boolean = false}};
    snprintf(g_db_path, sizeof(g_db_path), "%s", raw);
    return (Value){VAL_BOOL, {.boolean = true}};
}

static Value vdb_sync(int argCount, Value* args) {
    if (argCount < 2 || !is_string_value(args[0]) || !is_string_value(args[1])) {
        return (Value){VAL_BOOL, {.boolean = false}};
    }
    const char* table = AS_STRING(args[0])->chars;
    const char* schema = AS_STRING(args[1])->chars;
    char sql[8192];
    if (!build_schema_sql(table, schema, sql, sizeof(sql))) return (Value){VAL_BOOL, {.boolean = false}};
    Value out = run_sql_capture(sql);
    return (Value){VAL_BOOL, {.boolean = !output_has_sql_error(out)}};
}

static Value vdb_query(int argCount, Value* args) {
    if (argCount != 1 || !is_string_value(args[0])) return (Value){VAL_NIL, {.number = 0}};
    return run_sql_capture(AS_STRING(args[0])->chars);
}

static Value vdb_insert(int argCount, Value* args) {
    if (argCount < 2 || !is_string_value(args[0]) || !is_string_value(args[1])) return (Value){VAL_NIL, {.number = 0}};
    const char* table = AS_STRING(args[0])->chars;
    if (!is_safe_ident(table)) return (Value){VAL_NIL, {.number = 0}};

    char cols[4096], vals[4096], sets[4096];
    bool has_id = false;
    int id_val = 0;
    if (!build_data_sql_parts(AS_STRING(args[1])->chars, cols, sizeof(cols), vals, sizeof(vals), sets, sizeof(sets), &has_id, &id_val)) {
        return (Value){VAL_NIL, {.number = 0}};
    }

    char sql[8192];
    snprintf(sql, sizeof(sql), "INSERT INTO %s (%s) VALUES (%s); SELECT last_insert_rowid();", table, cols, vals);
    return run_sql_capture(sql);
}

static Value vdb_update(int argCount, Value* args) {
    if (argCount < 3 || !is_string_value(args[0]) || !IS_NUMBER(args[1]) || !is_string_value(args[2])) {
        return (Value){VAL_BOOL, {.boolean = false}};
    }
    const char* table = AS_STRING(args[0])->chars;
    if (!is_safe_ident(table)) return (Value){VAL_BOOL, {.boolean = false}};

    char cols[4096], vals[4096], sets[4096];
    bool has_id = false;
    int id_dummy = 0;
    if (!build_data_sql_parts(AS_STRING(args[2])->chars, cols, sizeof(cols), vals, sizeof(vals), sets, sizeof(sets), &has_id, &id_dummy)) {
        return (Value){VAL_BOOL, {.boolean = false}};
    }
    int id = (int)args[1].as.number;
    char sql[8192];
    snprintf(sql, sizeof(sql), "UPDATE %s SET %s WHERE id=%d;", table, sets, id);
    Value out = run_sql_capture(sql);
    return (Value){VAL_BOOL, {.boolean = !output_has_sql_error(out)}};
}

static Value vdb_save(int argCount, Value* args) {
    if (argCount < 2 || !is_string_value(args[0]) || !is_string_value(args[1])) {
        return (Value){VAL_NIL, {.number = 0}};
    }
    const char* table = AS_STRING(args[0])->chars;
    if (!is_safe_ident(table)) return (Value){VAL_NIL, {.number = 0}};

    char cols[4096], vals[4096], sets[4096];
    bool has_id = false;
    int id_val = 0;
    if (!build_data_sql_parts(AS_STRING(args[1])->chars, cols, sizeof(cols), vals, sizeof(vals), sets, sizeof(sets), &has_id, &id_val)) {
        return (Value){VAL_NIL, {.number = 0}};
    }

    char sql[8192];
    if (has_id) {
        snprintf(sql, sizeof(sql), "UPDATE %s SET %s WHERE id=%d; SELECT %d;", table, sets, id_val, id_val);
    } else {
        snprintf(sql, sizeof(sql), "INSERT INTO %s (%s) VALUES (%s); SELECT last_insert_rowid();", table, cols, vals);
    }
    return run_sql_capture(sql);
}

static Value vdb_get(int argCount, Value* args) {
    if (argCount != 2 || !is_string_value(args[0]) || !IS_NUMBER(args[1])) return (Value){VAL_NIL, {.number = 0}};
    const char* table = AS_STRING(args[0])->chars;
    if (!is_safe_ident(table)) return (Value){VAL_NIL, {.number = 0}};
    int id = (int)args[1].as.number;
    char sql[1024];
    snprintf(sql, sizeof(sql), "SELECT * FROM %s WHERE id=%d LIMIT 1;", table, id);
    return run_sql_capture(sql);
}

static Value vdb_find(int argCount, Value* args) {
    if (argCount < 1 || !is_string_value(args[0])) return (Value){VAL_NIL, {.number = 0}};
    const char* table = AS_STRING(args[0])->chars;
    const char* where = (argCount >= 2 && is_string_value(args[1])) ? AS_STRING(args[1])->chars : "1=1";
    if (!is_safe_ident(table)) return (Value){VAL_NIL, {.number = 0}};
    char sql[8192];
    snprintf(sql, sizeof(sql), "SELECT * FROM %s WHERE %s;", table, where);
    return run_sql_capture(sql);
}

static Value vdb_first(int argCount, Value* args) {
    if (argCount < 1 || !is_string_value(args[0])) return (Value){VAL_NIL, {.number = 0}};
    const char* table = AS_STRING(args[0])->chars;
    const char* where = (argCount >= 2 && is_string_value(args[1])) ? AS_STRING(args[1])->chars : "1=1";
    if (!is_safe_ident(table)) return (Value){VAL_NIL, {.number = 0}};
    char sql[8192];
    snprintf(sql, sizeof(sql), "SELECT * FROM %s WHERE %s LIMIT 1;", table, where);
    return run_sql_capture(sql);
}

static Value vdb_upsert(int argCount, Value* args) {
    if (argCount < 3 || !is_string_value(args[0]) || !is_string_value(args[1]) || !is_string_value(args[2])) {
        return (Value){VAL_NIL, {.number = 0}};
    }
    const char* table = AS_STRING(args[0])->chars;
    const char* conflict_key = AS_STRING(args[1])->chars;
    if (!is_safe_ident(table) || !is_safe_ident(conflict_key)) return (Value){VAL_NIL, {.number = 0}};

    char cols[4096], vals[4096], sets[4096];
    bool has_id = false;
    int id_val = 0;
    if (!build_data_sql_parts(AS_STRING(args[2])->chars, cols, sizeof(cols), vals, sizeof(vals), sets, sizeof(sets), &has_id, &id_val)) {
        return (Value){VAL_NIL, {.number = 0}};
    }
    char sql[8192];
    snprintf(sql, sizeof(sql),
             "INSERT INTO %s (%s) VALUES (%s) ON CONFLICT(%s) DO UPDATE SET %s;",
             table, cols, vals, conflict_key, sets);
    return run_sql_capture(sql);
}

static Value vdb_delete(int argCount, Value* args) {
    if (argCount != 2 || !is_string_value(args[0]) || !IS_NUMBER(args[1])) return (Value){VAL_BOOL, {.boolean = false}};
    const char* table = AS_STRING(args[0])->chars;
    if (!is_safe_ident(table)) return (Value){VAL_BOOL, {.boolean = false}};
    int id = (int)args[1].as.number;
    char sql[1024];
    snprintf(sql, sizeof(sql), "DELETE FROM %s WHERE id=%d;", table, id);
    Value out = run_sql_capture(sql);
    return (Value){VAL_BOOL, {.boolean = !output_has_sql_error(out)}};
}

static Value vdb_begin(int argCount, Value* args) {
    (void)argCount;
    (void)args;
    (void)run_sql_capture("BEGIN TRANSACTION;");
    return (Value){VAL_NIL, {.number = 0}};
}

static Value vdb_commit(int argCount, Value* args) {
    (void)argCount;
    (void)args;
    (void)run_sql_capture("COMMIT;");
    return (Value){VAL_NIL, {.number = 0}};
}

static Value vdb_rollback(int argCount, Value* args) {
    (void)argCount;
    (void)args;
    (void)run_sql_capture("ROLLBACK;");
    return (Value){VAL_NIL, {.number = 0}};
}

static Value vdb_count(int argCount, Value* args) {
    if (argCount < 1 || !is_string_value(args[0])) return (Value){VAL_NIL, {.number = 0}};
    const char* table = AS_STRING(args[0])->chars;
    const char* where = (argCount >= 2 && is_string_value(args[1])) ? AS_STRING(args[1])->chars : "1=1";
    if (!is_safe_ident(table)) return (Value){VAL_NIL, {.number = 0}};
    char sql[4096];
    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s WHERE %s;", table, where);
    return run_sql_capture(sql);
}

static Value vdb_exists(int argCount, Value* args) {
    Value count = vdb_count(argCount, args);
    if (!is_string_value(count)) return (Value){VAL_BOOL, {.boolean = false}};
    double n = 0.0;
    if (!parse_double_text(AS_STRING(count)->chars, &n)) n = 0.0;
    return (Value){VAL_BOOL, {.boolean = n > 0.0}};
}

static bool escape_json_string(const char* in, char* out, size_t out_size) {
    if (!in || !out || out_size == 0) return false;
    size_t j = 0;
    for (size_t i = 0; in[i] != '\0'; i++) {
        char c = in[i];
        if (c == '"' || c == '\\') {
            if (j + 2 >= out_size) return false;
            out[j++] = '\\';
            out[j++] = c;
        } else if (c == '\n') {
            if (j + 2 >= out_size) return false;
            out[j++] = '\\';
            out[j++] = 'n';
        } else if (c == '\r') {
            if (j + 2 >= out_size) return false;
            out[j++] = '\\';
            out[j++] = 'r';
        } else if (c == '\t') {
            if (j + 2 >= out_size) return false;
            out[j++] = '\\';
            out[j++] = 't';
        } else {
            if (j + 1 >= out_size) return false;
            out[j++] = c;
        }
    }
    out[j] = '\0';
    return true;
}

static Value vdb_paginate(int argCount, Value* args) {
    if (argCount < 4 || !is_string_value(args[0]) || !is_string_value(args[1]) || !IS_NUMBER(args[2]) || !IS_NUMBER(args[3])) {
        return (Value){VAL_NIL, {.number = 0}};
    }
    const char* table = AS_STRING(args[0])->chars;
    const char* where = AS_STRING(args[1])->chars;
    int page = (int)args[2].as.number;
    int size = (int)args[3].as.number;
    if (page < 1) page = 1;
    if (size < 1) size = 20;
    if (!is_safe_ident(table)) return (Value){VAL_NIL, {.number = 0}};

    int offset = (page - 1) * size;
    char data_sql[4096];
    char count_sql[4096];
    snprintf(data_sql, sizeof(data_sql), "SELECT * FROM %s WHERE %s LIMIT %d OFFSET %d;", table, where, size, offset);
    snprintf(count_sql, sizeof(count_sql), "SELECT COUNT(*) FROM %s WHERE %s;", table, where);
    Value data = run_sql_capture(data_sql);
    Value total = run_sql_capture(count_sql);

    const char* rows = is_string_value(data) ? AS_STRING(data)->chars : "";
    const char* total_raw = is_string_value(total) ? AS_STRING(total)->chars : "0";
    char rows_json[8192];
    if (!escape_json_string(rows, rows_json, sizeof(rows_json))) return (Value){VAL_NIL, {.number = 0}};

    char json[10000];
    snprintf(json, sizeof(json), "{\"data\":\"%s\",\"total\":%s,\"pages\":%d}", rows_json, total_raw, (size > 0) ? (int)ceil(atof(total_raw) / (double)size) : 0);
    return string_value(json);
}

static Value vdb_table(int argCount, Value* args) {
    if (argCount != 1 || !is_string_value(args[0])) return (Value){VAL_NIL, {.number = 0}};
    return args[0];
}

static Value vdb_where(int argCount, Value* args) {
    if (argCount == 1) return args[0];
    if (argCount >= 3 && is_string_value(args[0]) && is_string_value(args[1]) && is_string_value(args[2])) {
        char out[1024];
        snprintf(out, sizeof(out), "%s %s %s", AS_STRING(args[0])->chars, AS_STRING(args[1])->chars, AS_STRING(args[2])->chars);
        return string_value(out);
    }
    return (Value){VAL_NIL, {.number = 0}};
}

static Value native_ai_config(int argCount, Value* args) {
    if (argCount != 2 || !is_string_value(args[0]) || !is_string_value(args[1])) {
        return (Value){VAL_BOOL, {.boolean = false}};
    }
    snprintf(g_ai_provider, sizeof(g_ai_provider), "%s", AS_STRING(args[0])->chars);
    snprintf(g_ai_key, sizeof(g_ai_key), "%s", AS_STRING(args[1])->chars);
    return (Value){VAL_BOOL, {.boolean = true}};
}

static Value ai_http_json(const char* url, const char* body) {
    if (!url || !body || g_ai_key[0] == '\0') return (Value){VAL_NIL, {.number = 0}};

    char q_url[2048];
    char q_body[8192];
    char q_auth[1024];
    char auth[512];
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", g_ai_key);

    if (!shell_quote_double(url, q_url, sizeof(q_url)) ||
        !shell_quote_double(body, q_body, sizeof(q_body)) ||
        !shell_quote_double(auth, q_auth, sizeof(q_auth))) {
        return (Value){VAL_NIL, {.number = 0}};
    }

    char cmd[16384];
    snprintf(cmd, sizeof(cmd),
             "curl -sS -X POST %s -H %s -H \"Content-Type: application/json\" -d %s",
             q_url, q_auth, q_body);
    return run_shell_capture(cmd);
}

static bool ai_provider_is_openai(void) {
    return strcmp(g_ai_provider, "openai") == 0 || strcmp(g_ai_provider, "OPENAI") == 0;
}

static const char* guess_image_mime(const char* path) {
    const char* dot = path ? strrchr(path, '.') : NULL;
    if (!dot) return "application/octet-stream";
    if (strcasecmp(dot, ".png") == 0) return "image/png";
    if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0) return "image/jpeg";
    if (strcasecmp(dot, ".gif") == 0) return "image/gif";
    if (strcasecmp(dot, ".webp") == 0) return "image/webp";
    return "application/octet-stream";
}

static Value native_ai_ask(int argCount, Value* args) {
    if (argCount != 1 || !is_string_value(args[0])) return (Value){VAL_NIL, {.number = 0}};
    if (!ai_provider_is_openai()) {
        if (!g_ai_features_notice_printed) {
            fprintf(stderr, "viper: ai.ask currently supports provider=openai.\n");
            g_ai_features_notice_printed = true;
        }
        return (Value){VAL_NIL, {.number = 0}};
    }

    char prompt_json[8192];
    if (!escape_json_string(AS_STRING(args[0])->chars, prompt_json, sizeof(prompt_json))) {
        return (Value){VAL_NIL, {.number = 0}};
    }
    char body[12288];
    snprintf(body, sizeof(body),
             "{\"model\":\"gpt-4.1-mini\",\"input\":\"%s\"}",
             prompt_json);
    return ai_http_json("https://api.openai.com/v1/responses", body);
}

static Value native_ai_chat(int argCount, Value* args) {
    if (argCount != 1) return (Value){VAL_NIL, {.number = 0}};
    if (!ai_provider_is_openai()) return (Value){VAL_NIL, {.number = 0}};

    char input_json[16384];
    if (is_string_value(args[0])) {
        char escaped[12000];
        if (!escape_json_string(AS_STRING(args[0])->chars, escaped, sizeof(escaped))) {
            return (Value){VAL_NIL, {.number = 0}};
        }
        snprintf(input_json, sizeof(input_json), "\"%s\"", escaped);
    } else {
        Value one = args[0];
        Value json_val = native_json(1, &one);
        if (!is_string_value(json_val)) return (Value){VAL_NIL, {.number = 0}};
        if (snprintf(input_json, sizeof(input_json), "%s", AS_STRING(json_val)->chars) >= (int)sizeof(input_json)) {
            return (Value){VAL_NIL, {.number = 0}};
        }
    }

    char body[20000];
    snprintf(body, sizeof(body), "{\"model\":\"gpt-4.1-mini\",\"input\":%s}", input_json);
    return ai_http_json("https://api.openai.com/v1/responses", body);
}

static Value native_ai_embed(int argCount, Value* args) {
    if (argCount != 1 || !is_string_value(args[0])) return (Value){VAL_NIL, {.number = 0}};
    if (!ai_provider_is_openai()) {
        return (Value){VAL_NIL, {.number = 0}};
    }
    char text_json[8192];
    if (!escape_json_string(AS_STRING(args[0])->chars, text_json, sizeof(text_json))) {
        return (Value){VAL_NIL, {.number = 0}};
    }
    char body[12288];
    snprintf(body, sizeof(body),
             "{\"model\":\"text-embedding-3-small\",\"input\":\"%s\"}",
             text_json);
    return ai_http_json("https://api.openai.com/v1/embeddings", body);
}

static Value native_ai_extract(int argCount, Value* args) {
    if (argCount < 1 || !is_string_value(args[0])) return (Value){VAL_NIL, {.number = 0}};
    if (!ai_provider_is_openai()) return (Value){VAL_NIL, {.number = 0}};

    const char* text = AS_STRING(args[0])->chars;
    const char* schema = (argCount >= 2 && is_string_value(args[1])) ? AS_STRING(args[1])->chars : NULL;
    char prompt[16000];
    if (schema && schema[0] != '\0') {
        snprintf(prompt, sizeof(prompt),
                 "Extract data from the following text. Return valid JSON only and match this schema exactly:\n%s\n\nText:\n%s",
                 schema, text);
    } else {
        snprintf(prompt, sizeof(prompt),
                 "Extract structured data from the following text. Return valid JSON only.\n\nText:\n%s",
                 text);
    }
    Value prompt_val = string_value(prompt);
    return native_ai_ask(1, &prompt_val);
}

static Value native_ai_vision(int argCount, Value* args) {
    if (argCount != 2 || !is_string_value(args[0]) || !is_string_value(args[1])) {
        return (Value){VAL_NIL, {.number = 0}};
    }
    if (!ai_provider_is_openai()) return (Value){VAL_NIL, {.number = 0}};

    const char* image_ref = AS_STRING(args[0])->chars;
    const char* prompt = AS_STRING(args[1])->chars;
    bool is_remote = strncmp(image_ref, "http://", 7) == 0 || strncmp(image_ref, "https://", 8) == 0;

    char* image_url = NULL;
    if (is_remote) {
        image_url = dup_cstr(image_ref);
    } else {
        char* bytes = NULL;
        size_t n = 0;
        if (!read_file_alloc(image_ref, &bytes, &n) || !bytes) return (Value){VAL_NIL, {.number = 0}};
        size_t b64_cap = ((n + 2) / 3) * 4 + 8;
        char* b64 = (char*)malloc(b64_cap);
        if (!b64) {
            free(bytes);
            return (Value){VAL_NIL, {.number = 0}};
        }
        if (base64_encode_buf((const uint8_t*)bytes, n, b64, b64_cap) == 0) {
            free(bytes);
            free(b64);
            return (Value){VAL_NIL, {.number = 0}};
        }
        free(bytes);

        const char* mime = guess_image_mime(image_ref);
        size_t url_cap = strlen("data:;base64,") + strlen(mime) + strlen(b64) + 1;
        image_url = (char*)malloc(url_cap);
        if (!image_url) {
            free(b64);
            return (Value){VAL_NIL, {.number = 0}};
        }
        snprintf(image_url, url_cap, "data:%s;base64,%s", mime, b64);
        free(b64);
    }
    if (!image_url) return (Value){VAL_NIL, {.number = 0}};

    char* prompt_json = escape_json_alloc(prompt);
    char* image_json = escape_json_alloc(image_url);
    free(image_url);
    if (!prompt_json || !image_json) {
        free(prompt_json);
        free(image_json);
        return (Value){VAL_NIL, {.number = 0}};
    }

    size_t body_cap = strlen(prompt_json) + strlen(image_json) + 1024;
    char* body = (char*)malloc(body_cap);
    if (!body) {
        free(prompt_json);
        free(image_json);
        return (Value){VAL_NIL, {.number = 0}};
    }
    snprintf(body, body_cap,
             "{\"model\":\"gpt-4.1-mini\",\"input\":[{\"role\":\"user\",\"content\":[{\"type\":\"input_text\",\"text\":\"%s\"},{\"type\":\"input_image\",\"image_url\":\"%s\"}]}]}",
             prompt_json, image_json);
    Value res = ai_http_json("https://api.openai.com/v1/responses", body);
    free(body);
    free(prompt_json);
    free(image_json);
    return res;
}

static Value native_ai_tool(int argCount, Value* args) {
    if (argCount != 1 || !IS_OBJ(args[0])) return (Value){VAL_BOOL, {.boolean = false}};
    int type = AS_OBJ(args[0])->type;
    if (type != OBJ_FUNCTION && type != OBJ_NATIVE) return (Value){VAL_BOOL, {.boolean = false}};
    if (g_ai_tool_count >= MAX_AI_TOOLS) return (Value){VAL_BOOL, {.boolean = false}};
    g_ai_tools[g_ai_tool_count++] = args[0];
    retain_obj(AS_OBJ(args[0]));
    return (Value){VAL_NUMBER, {.number = (double)g_ai_tool_count}};
}

static Value native_meta_symbols(int argCount, Value* args) {
    (void)argCount;
    (void)args;
    char out[8192];
    size_t len = 0;
    if (!append_fmt(out, sizeof(out), &len, "{\"native_count\":%d,\"ai_tool_count\":%d,\"natives\":[", _native_count, g_ai_tool_count)) {
        return (Value){VAL_NIL, {.number = 0}};
    }
    for (int i = 0; i < _native_count; i++) {
        if (i > 0 && !append_fmt(out, sizeof(out), &len, ",")) return (Value){VAL_NIL, {.number = 0}};
        if (!append_fmt(out, sizeof(out), &len, "\"%s\"", native_registry[i].name)) {
            return (Value){VAL_NIL, {.number = 0}};
        }
    }
    if (!append_fmt(out, sizeof(out), &len, "]}")) return (Value){VAL_NIL, {.number = 0}};
    return string_value(out);
}

static Value native_meta_ast(int argCount, Value* args) {
    if (argCount != 1 || !is_string_value(args[0])) return (Value){VAL_NIL, {.number = 0}};
    char q_path[2048];
    if (!shell_quote_double(AS_STRING(args[0])->chars, q_path, sizeof(q_path))) return (Value){VAL_NIL, {.number = 0}};
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "./viper --emit-index-json %s 2>/dev/null", q_path);
    return run_shell_capture(cmd);
}

static Value native_meta_eval_sandboxed(int argCount, Value* args) {
    if (argCount < 1 || !is_string_value(args[0])) return (Value){VAL_NIL, {.number = 0}};
    char tmp_vp[] = "/tmp/viper-meta-eval-XXXXXX.vp";
    int fd = mkstemps(tmp_vp, 3);
    if (fd < 0) return (Value){VAL_NIL, {.number = 0}};
    const char* code = AS_STRING(args[0])->chars;
    size_t n = strlen(code);
    if (write(fd, code, n) < 0) {
        close(fd);
        unlink(tmp_vp);
        return (Value){VAL_NIL, {.number = 0}};
    }
    close(fd);

    char q_path[2048];
    if (!shell_quote_double(tmp_vp, q_path, sizeof(q_path))) {
        unlink(tmp_vp);
        return (Value){VAL_NIL, {.number = 0}};
    }

    int timeout_sec = 5;
    if (argCount >= 2 && is_string_value(args[1])) {
        const char* perm = AS_STRING(args[1])->chars;
        if (strncmp(perm, "timeout=", 8) == 0) {
            int n = 0;
            if (parse_positive_int(perm + 8, &n) && n <= 300) timeout_sec = n;
        }
    }

    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "timeout %ds ./viper %s 2>&1", timeout_sec, q_path);
    Value out = run_shell_capture(cmd);
    unlink(tmp_vp);
    return out;
}

static Value native_meta_test_runner(int argCount, Value* args) {
    const char* dir = "tests/scripts";
    if (argCount >= 1 && is_string_value(args[0])) dir = AS_STRING(args[0])->chars;
    char q_dir[1024];
    if (!shell_quote_double(dir, q_dir, sizeof(q_dir))) return (Value){VAL_NIL, {.number = 0}};
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "./tests/run_tests.sh ./viper %s 10 2>&1", q_dir);
    return run_shell_capture(cmd);
}

static Value native_meta_compress_context(int argCount, Value* args) {
    (void)argCount;
    (void)args;
    return run_shell_capture(
        "printf '# files\\n'; "
        "find . -maxdepth 4 -type f | sed 's#^./##' | head -n 180; "
        "printf '\\n# directories\\n'; "
        "find . -maxdepth 3 -type d | sed 's#^./##' | head -n 80; "
        "printf '\\n# approx_loc\\n'; "
        "find src include lib tests -type f 2>/dev/null | xargs wc -l 2>/dev/null | tail -n 1"
    );
}

void init_native_core() {
    _native_count = 0;
    srand((unsigned int)time(NULL));
    for (int i = 0; i < MAX_CACHE_ENTRIES; i++) {
        g_cache[i].used = false;
        g_cache[i].key = NULL;
        g_cache[i].value = (Value){VAL_NIL, {.number = 0}};
        g_cache[i].expire_at_ms = 0;
    }
    web_clear_registry();
    ai_clear_tool_registry();

    // Core runtime
    push_native("pr", native_print);
    push_native("clock", native_clock);
    push_native("load_dl", native_load_dl);
    push_native("get_fn", native_get_fn);
    push_native("serve", native_serve);
    push_native("fetch", native_fetch);
    push_native("query", native_query);
    push_native("json", native_json);
    push_native("panic", native_panic);
    push_native("recover", native_recover);

    // Core dynamic helpers (method-call path)
    push_native("keys", native_keys_method);
    push_native("has", native_has_method);

    // OS primitives
    push_native("os_sh", native_sh);
    push_native("os_env", native_env);
    push_native("os_setenv", native_setenv);
    push_native("os_args", native_args);
    push_native("os_exit", native_exit_now);
    push_native("os_cwd", native_cwd);
    push_native("os_pid", native_pid);
    push_native("os_kill", native_kill_proc);
    push_native("os_sleep", native_sleep_ms);
    push_native("os_info", native_os_info);
    push_native("os_cron", native_os_cron);

    // FS primitives
    push_native("fs_read", native_read);
    push_native("fs_read_bytes", native_read_bytes);
    push_native("fs_write", native_write);
    push_native("fs_append", native_append);
    push_native("fs_delete", native_delete_file);
    push_native("fs_copy", native_copy_file);
    push_native("fs_move", native_move_file);
    push_native("fs_exists", native_exists);
    push_native("fs_is_dir", native_is_dir);
    push_native("fs_mkdir", native_mkdir);
    push_native("fs_ls", native_ls);
    push_native("fs_watch", native_watch);
    push_native("fs_stream_read", native_stream_read);

    // Time & math utility primitives
    push_native("time_now", native_time_now);
    push_native("time_format", native_time_format);
    push_native("time_parse", native_time_parse);
    push_native("time_add", native_time_add);
    push_native("math_rand", native_math_rand);
    push_native("math_uuid", native_math_uuid);
    push_native("math_round", native_math_round);
    push_native("math_hash", native_math_hash);

    // Cache primitives
    push_native("cache_set", native_cache_set);
    push_native("cache_get", native_cache_get);
    push_native("cache_delete", native_cache_delete);
    push_native("cache_has", native_cache_has);
    push_native("cache_increment", native_cache_increment);
    push_native("cache_clear", native_cache_clear);
    push_native("cache_keys", native_cache_keys);

    // Web primitives
    push_native("web_serve", native_web_serve);
    push_native("web_route", native_web_route);
    push_native("web_middleware", native_web_middleware);
    push_native("web_static", native_web_static);
    push_native("web_cors", native_web_cors);
    push_native("web_jwt_sign", native_web_jwt_sign);
    push_native("web_jwt_verify", native_web_jwt_verify);
    push_native("web_hash", native_web_hash);
    push_native("web_fetch", native_fetch);
    push_native("web_post", native_web_post);
    push_native("web_put", native_web_put);
    push_native("web_patch", native_web_patch);
    push_native("web_delete", native_web_delete);
    push_native("web_ws_serve", native_web_ws_serve);
    push_native("web_ws_send", native_web_ws_send);
    push_native("web_download", native_web_download);
    push_native("web_upload", native_web_upload);

    // DB primitives
    push_native("vdb_connect", vdb_connect);
    push_native("vdb_sync", vdb_sync);
    push_native("vdb_get", vdb_get);
    push_native("vdb_find", vdb_find);
    push_native("vdb_first", vdb_first);
    push_native("vdb_save", vdb_save);
    push_native("vdb_insert", vdb_insert);
    push_native("vdb_update", vdb_update);
    push_native("vdb_upsert", vdb_upsert);
    push_native("vdb_delete", vdb_delete);
    push_native("vdb_query", vdb_query);
    push_native("vdb_begin", vdb_begin);
    push_native("vdb_commit", vdb_commit);
    push_native("vdb_rollback", vdb_rollback);
    push_native("vdb_count", vdb_count);
    push_native("vdb_exists", vdb_exists);
    push_native("vdb_paginate", vdb_paginate);
    push_native("vdb_table", vdb_table);
    push_native("vdb_where", vdb_where);

    // AI primitives
    push_native("ai_config", native_ai_config);
    push_native("ai_ask", native_ai_ask);
    push_native("ai_chat", native_ai_chat);
    push_native("ai_embed", native_ai_embed);
    push_native("ai_extract", native_ai_extract);
    push_native("ai_vision", native_ai_vision);
    push_native("ai_tool", native_ai_tool);

    // Meta primitives
    push_native("meta_symbols", native_meta_symbols);
    push_native("meta_ast", native_meta_ast);
    push_native("meta_eval_sandboxed", native_meta_eval_sandboxed);
    push_native("meta_test_runner", native_meta_test_runner);
    push_native("meta_compress_context", native_meta_compress_context);
}
