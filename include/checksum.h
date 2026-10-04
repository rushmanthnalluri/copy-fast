#ifndef COPYFAST_CHECKSUM_H
#define COPYFAST_CHECKSUM_H

#include "copyfast.h"

#define SHA256_DIGEST_SIZE 32
#define SHA256_HEX_SIZE    65
#define XXH64_HEX_SIZE     17

typedef struct {
    uint32_t state[8];
    uint64_t count;
    uint8_t  buffer[64];
} sha256_ctx_t;

typedef struct {
    uint64_t total_len;
    uint64_t v1;
    uint64_t v2;
    uint64_t v3;
    uint64_t v4;
    uint8_t  mem[32];
    uint32_t memsize;
} xxh64_ctx_t;

typedef struct {
    checksum_type_t type;
    union {
        sha256_ctx_t sha;
        xxh64_ctx_t  xxh;
    } u;
} checksum_ctx_t;

void checksum_init(checksum_ctx_t *ctx, checksum_type_t type);
void checksum_update(checksum_ctx_t *ctx, const void *data, size_t len);
void checksum_update_zeros(checksum_ctx_t *ctx, size_t count);
void checksum_final(checksum_ctx_t *ctx, char *out_hex, size_t out_hex_size);

/* Compute checksum of an entire file directly */
int  checksum_file(const char *path, checksum_type_t type, char *out_hex, size_t out_hex_size);

#endif /* COPYFAST_CHECKSUM_H */
