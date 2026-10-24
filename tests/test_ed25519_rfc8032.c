// RFC 8032 §7.1 Test 1 向量:验证 vendor ed25519 的 keypair/sign 与标准
// 一致(签名链的根基,实现被替换/损坏时必须在这里爆掉)。
#include "vendor/ed25519/ed25519.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

// RFC 8032 公开测试向量(非秘密材料)。
static const char *SEED_HEX =
    "9d61b19deffd5a60ba844af492ec2cc4"
    "4449c5697b326919703bac031cae7f60";
static const char *PUB_HEX =
    "d75a980182b10ab7d54bfed3c964073a"
    "0ee172f3daa62325af021a68f707511a";
static const char *SIG_EMPTY_HEX =
    "e5564300c360ac729086e2cc806e828a"
    "84877f1eb8e5d974d873e06522490155"
    "5fb8821590a33bacc61e39701cf9b46b"
    "d25bf5f0595bbe24655141438e7a100b";
// Test 2(注意:RFC 8032 每条向量密钥不同,不能复用 Test 1 的 keypair)。
static const char *SEED2_HEX =
    "4ccd089b28ff96da9db6c346ec114e0f"
    "5b8a319f35aba624da8cf6ed4fb8a6fb";
static const char *PUB2_HEX =
    "3d4017c3e843895a92b70aa74d1b7ebc"
    "9c982ccf2ec4968cc0cd55f12af4660c";
static const char *MSG1_SIG_HEX =
    "92a009a9f0d4cab8720e820b5f642540"
    "a2b27b5416503f8fb3762223ebdb69da"
    "085ac1e43e15996e458f3613d0f11d8c"
    "387b2eaeb4302aeeb00d291612bb0c00";
// Test 3:2 字节消息 af82(覆盖非空多字节消息的流式路径)。
static const char *SEED3_HEX =
    "c5aa8df43f9f837bedb7442f31dcb7b1"
    "66d38535076f094b85ce3a2e0b4458f7";
static const char *PUB3_HEX =
    "fc51cd8e6218a1a38da47ed00230f058"
    "0816ed13ba3303ac5deb911548908025";
static const char *MSG2_SIG_HEX =
    "6291d657deec24024827e69c3abe01a3"
    "0ce548a284743a445e3680d7db5ac3ac"
    "18ff9b538d16f290ae67f760984dc659"
    "4a7c15e9716ed28dc027beceea1ec40a";

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static size_t unhex(const char *hex, uint8_t *out, size_t cap) {
    size_t n = 0;
    while (hex[0] && hex[1] && n < cap) {
        out[n++] = (uint8_t)((hexval(hex[0]) << 4) | hexval(hex[1]));
        hex += 2;
    }
    return n;
}

static int check(const char *name, const uint8_t *got, const uint8_t *want,
                 size_t len) {
    if (memcmp(got, want, len) == 0) return 0;
    fprintf(stderr, "%s mismatch\n  got  ", name);
    for (size_t i = 0; i < len; i++) fprintf(stderr, "%02x", got[i]);
    fprintf(stderr, "\n  want ");
    for (size_t i = 0; i < len; i++) fprintf(stderr, "%02x", want[i]);
    fprintf(stderr, "\n");
    return 1;
}

int main(void) {
    uint8_t seed[32], want_pub[32], want_sig[64];
    uint8_t pub[32], priv[64], sig[64];
    int fails = 0;

    unhex(SEED_HEX, seed, sizeof(seed));
    unhex(PUB_HEX, want_pub, sizeof(want_pub));
    ed25519_create_keypair(pub, priv, seed);
    fails += check("pubkey", pub, want_pub, sizeof(pub));

    unhex(SIG_EMPTY_HEX, want_sig, sizeof(want_sig));
    ed25519_sign(sig, (const uint8_t *)"", 0, pub, priv);
    fails += check("sign(empty)", sig, want_sig, sizeof(sig));

    unhex(SEED2_HEX, seed, sizeof(seed));
    unhex(PUB2_HEX, want_pub, sizeof(want_pub));
    ed25519_create_keypair(pub, priv, seed);
    fails += check("pubkey2", pub, want_pub, sizeof(pub));

    unhex(MSG1_SIG_HEX, want_sig, sizeof(want_sig));
    ed25519_sign(sig, (const uint8_t *)"\x72", 1, pub, priv);
    fails += check("sign(0x72)", sig, want_sig, sizeof(sig));

    unhex(SEED3_HEX, seed, sizeof(seed));
    unhex(PUB3_HEX, want_pub, sizeof(want_pub));
    ed25519_create_keypair(pub, priv, seed);
    fails += check("pubkey3", pub, want_pub, sizeof(pub));

    unhex(MSG2_SIG_HEX, want_sig, sizeof(want_sig));
    ed25519_sign(sig, (const uint8_t *)"\xaf\x82", 2, pub, priv);
    fails += check("sign(af82)", sig, want_sig, sizeof(sig));

    if (fails) return 1;
    printf("ed25519 RFC8032 vectors: PASS\n");
    return 0;
}
