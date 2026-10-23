// 设备身份模块:身份源 = Wi-Fi STA MAC + BLE MAC(efuse 出厂值,唯一且不可改)。
// cardid 分区方案已弃用(cardid 可被整片擦除丢失,真机身份链断裂后无法恢复;
// MAC 派生无此单点)。cardid 明文永远不出设备的原则由 MAC 方案继承:
// 云上只见 device_hash = SHA-256(domain || mac_wifi || mac_ble)[:16]。
#pragma once

#include "esp_err.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 身份派生域分隔符(改动即身份体系变更,必须与 Worker/工具端保持一致)。
#define RADAR_IDENTITY_HASH_DOMAIN "mpr-id-v1"
#define RADAR_IDENTITY_KEY_DOMAIN  "mpr-ed25519-seed-v1"
// 上传批次签名 preimage 域(与 capture-sig-contract 一致)。
#define RADAR_IDENTITY_SIG_DOMAIN  "MPRCAP-SIG-v1"

// 取设备身份:hash[16] = SHA-256(domain || wifi_mac || ble_mac) 前 16B;
// pubkey[32] = 派生 Ed25519 公钥(RFC 8032 raw,小端)。结果常驻缓存,
// 首次调用后 O(1)。MAC 读取失败返回错误(理论上不可能,efuse 在则 MAC 在)。
esp_err_t radar_identity_get(uint8_t hash[16], uint8_t pubkey[32]);

// 用派生 Ed25519 私钥对 msg 做纯 EdDSA 签名(PSA_ALG_PURE_EDDSA,非
// Ed25519ph——验签端须对同一字节流直接验,不要预哈希)。每次调用内部
// 导入一次派生种子(32B,微秒级),不落盘、不常驻密钥句柄。
esp_err_t radar_identity_sign(const uint8_t *msg, size_t len, uint8_t sig[64]);

#ifdef __cplusplus
}
#endif
