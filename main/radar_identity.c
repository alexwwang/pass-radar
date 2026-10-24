// 设备身份实现:MAC 派生 + Ed25519 签名。
//
// 历史方案走 PSA(PSA_ALG_PURE_EDDSA),但 IDF 5.5.3 的 mbedTLS 3.6.5
// 裁掉了全部 Ed25519/EdDSA 实现(library 里无任何 Edwards 代码,
// Kconfig 也无对应选项),psa_import_key 恒报 INVALID_ARGUMENT
// (真机 /api/identity 500)。现使用 vendor 的 orlp/ed25519(zlib license,
// public domain 等价声明,ref10 派生),签名语义不变:标准 RFC 8032
// Ed25519(= PSA_ALG_PURE_EDDSA),验签端无需改动。
#include "radar_identity.h"

#include "esp_mac.h"
#include "esp_log.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/sha256.h"
#include "vendor/ed25519/ed25519.h"

#include <string.h>

static const char *TAG = "radar_identity";

static bool s_ready;
static uint8_t s_hash[16];
static uint8_t s_pubkey[32];
static uint8_t s_priv[64];   // Ed25519 扩展私钥(seed||pub),仅内存,不落盘

// 一次读取双 MAC,各自派生:hash = SHA-256(hash_domain || macs)[:16],
// seed = SHA-256(key_domain || macs)(Ed25519 种子,32B)。
static esp_err_t derive(uint8_t hash[16], uint8_t seed[32]) {
    uint8_t macs[12];
    if (esp_read_mac(macs, ESP_MAC_WIFI_STA) != ESP_OK) {
        ESP_LOGE(TAG, "read WIFI_STA mac failed");
        return ESP_FAIL;
    }
    if (esp_read_mac(macs + 6, ESP_MAC_BT) != ESP_OK) {
        ESP_LOGE(TAG, "read BT mac failed");
        return ESP_FAIL;
    }

    uint8_t digest[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    int rc = mbedtls_sha256_starts(&ctx, 0);
    if (rc == 0) rc = mbedtls_sha256_update(
        &ctx, (const uint8_t *)RADAR_IDENTITY_HASH_DOMAIN,
        strlen(RADAR_IDENTITY_HASH_DOMAIN));
    if (rc == 0) rc = mbedtls_sha256_update(&ctx, macs, sizeof(macs));
    if (rc == 0) rc = mbedtls_sha256_finish(&ctx, digest);
    if (rc == 0) memcpy(hash, digest, 16);
    mbedtls_sha256_free(&ctx);
    if (rc != 0) return ESP_FAIL;

    mbedtls_sha256_init(&ctx);
    rc = mbedtls_sha256_starts(&ctx, 0);
    if (rc == 0) rc = mbedtls_sha256_update(
        &ctx, (const uint8_t *)RADAR_IDENTITY_KEY_DOMAIN,
        strlen(RADAR_IDENTITY_KEY_DOMAIN));
    if (rc == 0) rc = mbedtls_sha256_update(&ctx, macs, sizeof(macs));
    if (rc == 0) rc = mbedtls_sha256_finish(&ctx, seed);
    mbedtls_sha256_free(&ctx);
    mbedtls_platform_zeroize(digest, sizeof(digest));
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

// 由派生种子生成密钥对并常驻(微秒级,一次即可)。
static esp_err_t ensure_ready(void) {
    if (s_ready) return ESP_OK;
    uint8_t seed[32];
    if (derive(s_hash, seed) != ESP_OK) {
        mbedtls_platform_zeroize(seed, sizeof(seed));
        return ESP_FAIL;
    }
    ed25519_create_keypair(s_pubkey, s_priv, seed);
    mbedtls_platform_zeroize(seed, sizeof(seed));
    s_ready = true;
    return ESP_OK;
}

esp_err_t radar_identity_get(uint8_t hash[16], uint8_t pubkey[32]) {
    if (!hash || !pubkey) return ESP_ERR_INVALID_ARG;
    if (ensure_ready() != ESP_OK) return ESP_FAIL;
    memcpy(hash, s_hash, 16);
    memcpy(pubkey, s_pubkey, 32);
    return ESP_OK;
}

esp_err_t radar_identity_sign(const uint8_t *msg, size_t len, uint8_t sig[64]) {
    if (!msg || !sig || len == 0) return ESP_ERR_INVALID_ARG;
    if (ensure_ready() != ESP_OK) return ESP_FAIL;
    ed25519_sign(sig, msg, len, s_pubkey, s_priv);
    return ESP_OK;
}
