#include "crypto.h"
#include <string.h>

#define CHW (uint32_t)32
#define CHL (uint32_t)64

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define SHR(x, n) ((x) >> (n))
#define SIG0(x) (ROTR(x, 7) ^ ROTR(x, 18) ^ SHR(x, 3))
#define SIG1(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ SHR(x, 10))
#define EPS0(x) (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define EPS1(x) (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))

typedef struct {
    uint32_t h[8];
    uint64_t len;
    uint8_t buf[64];
    uint32_t buf_len;
} SHA256_CTX;

static void sha256_transform(SHA256_CTX* ctx, const uint8_t* data) {
    uint32_t a, b, c, d, e, f, g, h, i, j, t1, t2, w[64];
    for (i = 0, j = 0; i < 16; i++, j += 4)
        w[i] = (data[j] << 24) | (data[j + 1] << 16) | (data[j + 2] << 8) | (data[j + 3]);
    for (; i < 64; i++)
        w[i] = SIG1(w[i - 2]) + w[i - 7] + SIG0(w[i - 15]) + w[i - 16];
    a = ctx->h[0]; b = ctx->h[1]; c = ctx->h[2]; d = ctx->h[3];
    e = ctx->h[4]; f = ctx->h[5]; g = ctx->h[6]; h = ctx->h[7];
    for (i = 0; i < 64; i++) {
        t1 = h + EPS1(e) + CH(e, f, g) + K[i] + w[i];
        t2 = EPS0(a) + MAJ(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    ctx->h[0] += a; ctx->h[1] += b; ctx->h[2] += c; ctx->h[3] += d;
    ctx->h[4] += e; ctx->h[5] += f; ctx->h[6] += g; ctx->h[7] += h;
}

void viper_sha256(const uint8_t* data, size_t len, uint8_t hash[32]) {
    SHA256_CTX ctx;
    ctx.h[0] = 0x6a09e667; ctx.h[1] = 0xbb67ae85; ctx.h[2] = 0x3c6ef372; ctx.h[3] = 0xa54ff53a;
    ctx.h[4] = 0x510e527f; ctx.h[5] = 0x9b05688c; ctx.h[6] = 0x1f83d9ab; ctx.h[7] = 0x5be0cd19;
    ctx.len = 0; ctx.buf_len = 0;

    for (size_t i = 0; i < len; i++) {
        ctx.buf[ctx.buf_len++] = data[i];
        if (ctx.buf_len == 64) {
            sha256_transform(&ctx, ctx.buf);
            ctx.len += 512;
            ctx.buf_len = 0;
        }
    }
    uint64_t total_len = ctx.len + ctx.buf_len * 8;
    ctx.buf[ctx.buf_len++] = 0x80;
    if (ctx.buf_len > 56) {
        while (ctx.buf_len < 64) ctx.buf[ctx.buf_len++] = 0;
        sha256_transform(&ctx, ctx.buf);
        ctx.buf_len = 0;
    }
    while (ctx.buf_len < 56) ctx.buf[ctx.buf_len++] = 0;
    for (int i = 7; i >= 0; i--) ctx.buf[56 + i] = (uint8_t)(total_len >> (8 * (7 - i)));
    sha256_transform(&ctx, ctx.buf);
    for (int i = 0; i < 8; i++) {
        hash[i * 4] = (ctx.h[i] >> 24) & 0xff;
        hash[i * 4 + 1] = (ctx.h[i] >> 16) & 0xff;
        hash[i * 4 + 2] = (ctx.h[i] >> 8) & 0xff;
        hash[i * 4 + 3] = ctx.h[i] & 0xff;
    }
}

void viper_hmac_sha256(const uint8_t* key, size_t key_len, const uint8_t* data, size_t data_len, uint8_t mac[32]) {
    uint8_t k[64] = {0};
    if (key_len > 64) viper_sha256(key, key_len, k);
    else memcpy(k, key, key_len);

    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }

    uint8_t inner[64 + 4096]; // Max data size for now
    if (data_len > 4096) return; // Error handling needed
    memcpy(inner, ipad, 64);
    memcpy(inner + 64, data, data_len);
    uint8_t inner_hash[32];
    viper_sha256(inner, 64 + data_len, inner_hash);

    uint8_t outer[64 + 32];
    memcpy(outer, opad, 64);
    memcpy(outer + 64, inner_hash, 32);
    viper_sha256(outer, 64 + 32, mac);
}

static const char* b64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int viper_base64_encode(const uint8_t* src, size_t len, char* dst, size_t dst_len) {
    size_t i, j;
    for (i = 0, j = 0; i < len; i += 3) {
        if (j + 4 >= dst_len) return -1;
        uint32_t v = (src[i] << 16) | ((i + 1 < len ? src[i + 1] : 0) << 8) | (i + 2 < len ? src[i + 2] : 0);
        dst[j++] = b64_chars[(v >> 18) & 0x3F];
        dst[j++] = b64_chars[(v >> 12) & 0x3F];
        dst[j++] = i + 1 < len ? b64_chars[(v >> 6) & 0x3F] : '=';
        dst[j++] = i + 2 < len ? b64_chars[v & 0x3F] : '=';
    }
    if (j >= dst_len) return -1;
    dst[j] = '\0';
    return (int)j;
}
