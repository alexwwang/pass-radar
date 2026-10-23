#include "radar_identity.h"

#include "esp_mac.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/sha256.h"
#include "psa/crypto.h"

#include <string.h>

static bool s_ready;
static uint8_t s_hash[16];
static uint8_t s_pubkey[32];

// 一次读取双 MAC,各自派生:hash = SHA-256(hash_domain || macs)[:16],
// seed = SHA-256(key_domain || macs)(Ed25519 种子,32B)。
static esp_err_t derive(uint8_t hash[16], uint8_t seed[32]) {
    uint8_t macs[12];
    if (esp_read_mac(macs, ESP_MAC_WIFI_STA) != ESP_OK) return ESP_FAIL;
    if (esp_read_mac(macs + 6, ESP_MAC_BT) != ESP_OK) return ESP_FAIL;

    uint8_t digest[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    int rc = mbedtls_sha256_starts(&ctx, 0);
    if (rc == 0) rc = mbedtls_sha256_update(
        &ctx, (const uint8_t *)RADAR_IDENTITY_HASH_DOMAIN,
        strlen(RADAR_IDENTITY_HASH_DOMAIN));
    if (rc == 0) rc = mbedtls_sha256_update(&ctx, macs, sizeof(macs));
    if (rc == 0) rc = mbedtls_sha256_finish(&ctx, digest);
    if (rc == 0) {
        memcpy(hash, digest, 16);
        mbedtls_sha256_init(&ctx);
        rc = mbedtls_sha256_starts(&ctx, 0);
        if (rc == 0) rc = mbedtls_sha256_update(
            &ctx, (const uint8_t *)RADAR_IDENTITY_KEY_DOMAIN,
            strlen(RADAR_IDENTITY_KEY_DOMAIN));
        if (rc == 0) rc = mbedtls_sha256_update(&ctx, macs, sizeof(macs));
        if (rc == 0) rc = mbedtls_sha256_finish(&ctx, seed);
    }
    mbedtls_sha256_free(&ctx);
    mbedtls_platform_zeroize(digest, sizeof(digest));
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t key_import(const uint8_t seed[32], psa_key_id_t *key) {
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE |
                                       PSA_KEY_USAGE_EXPORT);
    psa_set_key_algorithm(&attr, PSA_ALG_PURE_EDDSA);
    psa_set_key_type(
        &attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_TWISTED_EDWARDS));
    psa_set_key_bits(&attr, 255);
    psa_set_key_lifetime(&attr, PSA_KEY_LIFETIME_VOLATILE);
    psa_status_t st = psa_import_key(&attr, seed, 32, key);
    psa_reset_key_attributes(&attr);
    return st == PSA_SUCCESS ? ESP_OK : ESP_FAIL;
}

esp_err_t radar_identity_get(uint8_t hash[16], uint8_t pubkey[32]) {
    if (!hash || !pubkey) return ESP_ERR_INVALID_ARG;
    if (!s_ready) {
        uint8_t seed[32];
        if (derive(s_hash, seed) != ESP_OK) return ESP_FAIL;
        psa_key_id_t key;
        if (key_import(seed, &key) != ESP_OK) {
            mbedtls_platform_zeroize(seed, sizeof(seed));
            return ESP_FAIL;
        }
        size_t out = 0;
        psa_status_t st =
            psa_export_public_key(key, s_pubkey, sizeof(s_pubkey), &out);
        psa_destroy_key(key);
        mbedtls_platform_zeroize(seed, sizeof(seed));
        if (st != PSA_SUCCESS || out != sizeof(s_pubkey)) return ESP_FAIL;
        s_ready = true;
    }
    memcpy(hash, s_hash, 16);
    memcpy(pubkey, s_pubkey, 32);
    return ESP_OK;
}

esp_err_t radar_identity_sign(const uint8_t *msg, size_t len, uint8_t sig[64]) {
    if (!msg || !sig || len == 0) return ESP_ERR_INVALID_ARG;
    uint8_t hash[16];
    uint8_t seed[32];
    if (derive(hash, seed) != ESP_OK) return ESP_FAIL;
    psa_key_id_t key;
    if (key_import(seed, &key) != ESP_OK) {
        mbedtls_platform_zeroize(seed, sizeof(seed));
        return ESP_FAIL;
    }
    size_t out = 0;
    psa_status_t st =
        psa_sign_message(key, PSA_ALG_PURE_EDDSA, msg, len, sig, 64, &out);
    psa_destroy_key(key);
    mbedtls_platform_zeroize(seed, sizeof(seed));
    (void)hash;
    return (st == PSA_SUCCESS && out == 64) ? ESP_OK : ESP_FAIL;
}
