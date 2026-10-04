#include "checksum.h"

/* =========================================================================
 * SHA-256 Implementation (FIPS 180-4)
 * ========================================================================= */

#define ROTRIGHT(a,b) (((a) >> (b)) | ((a) << (32-(b))))
#define CH(x,y,z)     (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x,y,z)    (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x)        (ROTRIGHT(x,2) ^ ROTRIGHT(x,13) ^ ROTRIGHT(x,22))
#define EP1(x)        (ROTRIGHT(x,6) ^ ROTRIGHT(x,11) ^ ROTRIGHT(x,25))
#define SIG0(x)       (ROTRIGHT(x,7) ^ ROTRIGHT(x,18) ^ ((x) >> 3))
#define SIG1(x)       (ROTRIGHT(x,17) ^ ROTRIGHT(x,19) ^ ((x) >> 10))

static const uint32_t k256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static void sha256_transform(sha256_ctx_t *ctx, const uint8_t data[]) {
    uint32_t a, b, c, d, e, f, g, h, i, j, t1, t2, m[64];

    for (i = 0, j = 0; i < 16; ++i, j += 4)
        m[i] = ((uint32_t)data[j] << 24) | ((uint32_t)data[j + 1] << 16) |
               ((uint32_t)data[j + 2] << 8) | ((uint32_t)data[j + 3]);
    for (; i < 64; ++i)
        m[i] = SIG1(m[i - 2]) + m[i - 7] + SIG0(m[i - 15]) + m[i - 16];

    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    f = ctx->state[5];
    g = ctx->state[6];
    h = ctx->state[7];

    for (i = 0; i < 64; ++i) {
        t1 = h + EP1(e) + CH(e, f, g) + k256[i] + m[i];
        t2 = EP0(a) + MAJ(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

static void sha256_init(sha256_ctx_t *ctx) {
    ctx->count = 0;
    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
}

static void sha256_update(sha256_ctx_t *ctx, const uint8_t *data, size_t len) {
    size_t i;
    for (i = 0; i < len; ++i) {
        ctx->buffer[ctx->count % 64] = data[i];
        ctx->count++;
        if ((ctx->count % 64) == 0)
            sha256_transform(ctx, ctx->buffer);
    }
}

static void sha256_final(sha256_ctx_t *ctx, uint8_t hash[32]) {
    size_t i = ctx->count % 64;
    ctx->buffer[i++] = 0x80;

    if (i > 56) {
        while (i < 64)
            ctx->buffer[i++] = 0x00;
        sha256_transform(ctx, ctx->buffer);
        memset(ctx->buffer, 0, 56);
    } else {
        while (i < 56)
            ctx->buffer[i++] = 0x00;
    }

    uint64_t bit_len = ctx->count * 8;
    ctx->buffer[63] = (uint8_t)(bit_len);
    ctx->buffer[62] = (uint8_t)(bit_len >> 8);
    ctx->buffer[61] = (uint8_t)(bit_len >> 16);
    ctx->buffer[60] = (uint8_t)(bit_len >> 24);
    ctx->buffer[59] = (uint8_t)(bit_len >> 32);
    ctx->buffer[58] = (uint8_t)(bit_len >> 40);
    ctx->buffer[57] = (uint8_t)(bit_len >> 48);
    ctx->buffer[56] = (uint8_t)(bit_len >> 56);
    sha256_transform(ctx, ctx->buffer);

    for (i = 0; i < 4; ++i) {
        for (int k = 0; k < 8; ++k) {
            hash[k * 4 + i] = (uint8_t)((ctx->state[k] >> (24 - i * 8)) & 0x000000ff);
        }
    }
}

/* =========================================================================
 * xxHash64 Implementation
 * ========================================================================= */

#define XXH_PRIME64_1 11400714785074694791ULL
#define XXH_PRIME64_2 14029467366897019727ULL
#define XXH_PRIME64_3 1609587929392839161ULL
#define XXH_PRIME64_4 9650029242287828579ULL
#define XXH_PRIME64_5 2870177450012600261ULL

static inline uint64_t xxh_rotl64(uint64_t x, int r) {
    return (x << r) | (x >> (64 - r));
}

static inline uint64_t xxh_round(uint64_t acc, uint64_t input) {
    acc += input * XXH_PRIME64_2;
    acc = xxh_rotl64(acc, 31);
    acc *= XXH_PRIME64_1;
    return acc;
}

static inline uint64_t xxh_merge_round(uint64_t acc, uint64_t val) {
    val = xxh_round(0, val);
    acc ^= val;
    acc = acc * XXH_PRIME64_1 + XXH_PRIME64_4;
    return acc;
}

static void xxh64_init(xxh64_ctx_t *ctx, uint64_t seed) {
    ctx->total_len = 0;
    ctx->v1 = seed + XXH_PRIME64_1 + XXH_PRIME64_2;
    ctx->v2 = seed + XXH_PRIME64_2;
    ctx->v3 = seed + 0;
    ctx->v4 = seed - XXH_PRIME64_1;
    ctx->memsize = 0;
}

static void xxh64_update(xxh64_ctx_t *ctx, const void *input, size_t len) {
    const uint8_t *p = (const uint8_t *)input;
    const uint8_t *const bEnd = p + len;
    ctx->total_len += len;

    if (ctx->memsize + len < 32) {
        memcpy(ctx->mem + ctx->memsize, input, len);
        ctx->memsize += (uint32_t)len;
        return;
    }

    if (ctx->memsize > 0) {
        memcpy(ctx->mem + ctx->memsize, input, 32 - ctx->memsize);
        const uint64_t *p64 = (const uint64_t *)(const void *)ctx->mem;
        ctx->v1 = xxh_round(ctx->v1, p64[0]);
        ctx->v2 = xxh_round(ctx->v2, p64[1]);
        ctx->v3 = xxh_round(ctx->v3, p64[2]);
        ctx->v4 = xxh_round(ctx->v4, p64[3]);
        p += 32 - ctx->memsize;
        ctx->memsize = 0;
    }

    if (p <= bEnd - 32) {
        const uint8_t *const limit = bEnd - 32;
        do {
            uint64_t w0, w1, w2, w3;
            memcpy(&w0, p, 8);
            memcpy(&w1, p + 8, 8);
            memcpy(&w2, p + 16, 8);
            memcpy(&w3, p + 24, 8);
            ctx->v1 = xxh_round(ctx->v1, w0);
            ctx->v2 = xxh_round(ctx->v2, w1);
            ctx->v3 = xxh_round(ctx->v3, w2);
            ctx->v4 = xxh_round(ctx->v4, w3);
            p += 32;
        } while (p <= limit);
    }

    if (p < bEnd) {
        memcpy(ctx->mem, p, (size_t)(bEnd - p));
        ctx->memsize = (uint32_t)(bEnd - p);
    }
}

static uint64_t xxh64_digest(const xxh64_ctx_t *ctx) {
    uint64_t h64;
    const uint8_t *p = ctx->mem;
    const uint8_t *const bEnd = ctx->mem + ctx->memsize;

    if (ctx->total_len >= 32) {
        h64 = xxh_rotl64(ctx->v1, 1) + xxh_rotl64(ctx->v2, 7) +
              xxh_rotl64(ctx->v3, 12) + xxh_rotl64(ctx->v4, 18);
        h64 = xxh_merge_round(h64, ctx->v1);
        h64 = xxh_merge_round(h64, ctx->v2);
        h64 = xxh_merge_round(h64, ctx->v3);
        h64 = xxh_merge_round(h64, ctx->v4);
    } else {
        h64 = ctx->v3 + XXH_PRIME64_5;
    }

    h64 += (uint64_t)ctx->total_len;

    while (p + 8 <= bEnd) {
        uint64_t k1;
        memcpy(&k1, p, 8);
        k1 = xxh_round(0, k1);
        h64 ^= k1;
        h64 = xxh_rotl64(h64, 27) * XXH_PRIME64_1 + XXH_PRIME64_4;
        p += 8;
    }

    if (p + 4 <= bEnd) {
        uint32_t k2;
        memcpy(&k2, p, 4);
        h64 ^= (uint64_t)k2 * XXH_PRIME64_1;
        h64 = xxh_rotl64(h64, 23) * XXH_PRIME64_2 + XXH_PRIME64_3;
        p += 4;
    }

    while (p < bEnd) {
        h64 ^= (*p) * XXH_PRIME64_5;
        h64 = xxh_rotl64(h64, 11) * XXH_PRIME64_1;
        p++;
    }

    h64 ^= h64 >> 33;
    h64 *= XXH_PRIME64_2;
    h64 ^= h64 >> 29;
    h64 *= XXH_PRIME64_3;
    h64 ^= h64 >> 32;

    return h64;
}

/* =========================================================================
 * Public Unified Checksum Interface
 * ========================================================================= */

void checksum_init(checksum_ctx_t *ctx, checksum_type_t type) {
    if (!ctx) return;
    ctx->type = type;
    if (type == CHECKSUM_SHA256) {
        sha256_init(&ctx->u.sha);
    } else if (type == CHECKSUM_XXH64) {
        xxh64_init(&ctx->u.xxh, 0);
    }
}

void checksum_update(checksum_ctx_t *ctx, const void *data, size_t len) {
    if (!ctx || !data || len == 0) return;
    if (ctx->type == CHECKSUM_SHA256) {
        sha256_update(&ctx->u.sha, (const uint8_t *)data, len);
    } else if (ctx->type == CHECKSUM_XXH64) {
        xxh64_update(&ctx->u.xxh, data, len);
    }
}

void checksum_update_zeros(checksum_ctx_t *ctx, size_t count) {
    if (!ctx || count == 0) return;
    static const uint8_t zero_page[4096] = {0};
    while (count > 0) {
        size_t chunk = count < sizeof(zero_page) ? count : sizeof(zero_page);
        checksum_update(ctx, zero_page, chunk);
        count -= chunk;
    }
}

void checksum_final(checksum_ctx_t *ctx, char *out_hex, size_t out_hex_size) {
    if (!ctx || !out_hex || out_hex_size == 0) return;

    if (ctx->type == CHECKSUM_SHA256) {
        uint8_t hash[32];
        sha256_final(&ctx->u.sha, hash);
        for (int i = 0; i < 32 && (size_t)(i * 2 + 2) < out_hex_size; ++i) {
            snprintf(out_hex + (i * 2), out_hex_size - (i * 2), "%02x", hash[i]);
        }
    } else if (ctx->type == CHECKSUM_XXH64) {
        uint64_t hash = xxh64_digest(&ctx->u.xxh);
        snprintf(out_hex, out_hex_size, "%016lx", hash);
    } else {
        out_hex[0] = '\0';
    }
}

int checksum_file(const char *path, checksum_type_t type, char *out_hex, size_t out_hex_size) {
    if (!path || !out_hex || type == CHECKSUM_NONE) return -1;

    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    /* Posix advise sequential access */
    posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);

    const size_t buf_size = 1024 * 1024; /* 1MB read buffer */
    void *buf = malloc(buf_size);
    if (!buf) {
        close(fd);
        return -1;
    }

    checksum_ctx_t ctx;
    checksum_init(&ctx, type);

    ssize_t n;
    while ((n = read(fd, buf, buf_size)) > 0) {
        checksum_update(&ctx, buf, (size_t)n);
    }

    free(buf);
    close(fd);

    if (n < 0) return -1;

    checksum_final(&ctx, out_hex, out_hex_size);
    return 0;
}
