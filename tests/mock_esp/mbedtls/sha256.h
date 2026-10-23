#pragma once
#ifndef PASS_RADAR_MOCK_MBEDTLS_SHA256_H
#define PASS_RADAR_MOCK_MBEDTLS_SHA256_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

// Host mock of mbedtls/sha256.h: a compact, real SHA-256 (FIPS 180-4) so the
// firmware's radar_capture_device_hash() is cross-checked against known
// vectors (see tests/test_radar_capture_batch_data.c) instead of self-agreeing.

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t block[64];
    size_t blocklen;
} mbedtls_sha256_context;

static inline uint32_t mock_sha256_rotr(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32 - n));
}

static inline void mock_sha256_transform(mbedtls_sha256_context *ctx,
                                         const uint8_t *p) {
    static const uint32_t K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
    };
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = mock_sha256_rotr(w[i - 15], 7) ^ mock_sha256_rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = mock_sha256_rotr(w[i - 2], 17) ^ mock_sha256_rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    uint32_t e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = mock_sha256_rotr(e, 6) ^ mock_sha256_rotr(e, 11) ^ mock_sha256_rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = mock_sha256_rotr(a, 2) ^ mock_sha256_rotr(a, 13) ^ mock_sha256_rotr(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static inline void mbedtls_sha256_init(mbedtls_sha256_context *ctx) {
    memset(ctx, 0, sizeof(*ctx));
}

static inline int mbedtls_sha256_starts(mbedtls_sha256_context *ctx, int is224) {
    (void)is224;
    static const uint32_t INIT[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };
    memcpy(ctx->state, INIT, sizeof(INIT));
    ctx->bitlen = 0;
    ctx->blocklen = 0;
    return 0;
}

static inline int mbedtls_sha256_update(mbedtls_sha256_context *ctx,
                                        const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        ctx->block[ctx->blocklen++] = data[i];
        if (ctx->blocklen == 64) {
            mock_sha256_transform(ctx, ctx->block);
            ctx->blocklen = 0;
        }
    }
    ctx->bitlen += (uint64_t)len * 8;
    return 0;
}

static inline int mbedtls_sha256_finish(mbedtls_sha256_context *ctx,
                                        uint8_t out[32]) {
    uint64_t bitlen = ctx->bitlen;
    ctx->block[ctx->blocklen++] = 0x80;
    if (ctx->blocklen > 56) {
        while (ctx->blocklen < 64) ctx->block[ctx->blocklen++] = 0;
        mock_sha256_transform(ctx, ctx->block);
        ctx->blocklen = 0;
    }
    while (ctx->blocklen < 56) ctx->block[ctx->blocklen++] = 0;
    for (int i = 7; i >= 0; i--) ctx->block[ctx->blocklen++] = (uint8_t)(bitlen >> (i * 8));
    mock_sha256_transform(ctx, ctx->block);
    for (int i = 0; i < 8; i++) {
        out[i * 4] = (uint8_t)(ctx->state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        out[i * 4 + 3] = (uint8_t)ctx->state[i];
    }
    return 0;
}

static inline int mbedtls_sha256(const uint8_t *input, size_t ilen,
                                     uint8_t output[32], int is224) {
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, is224);
    mbedtls_sha256_update(&ctx, input, ilen);
    mbedtls_sha256_finish(&ctx, output);
    return 0;
}

static inline void mbedtls_sha256_free(mbedtls_sha256_context *ctx) {
    (void)ctx;
}

#endif
