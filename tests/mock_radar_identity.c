#include "../main/radar_identity.h"

#include <string.h>

// 宿主机链接桩:真实实现依赖 efuse/PSA,仅目标机构建。批次模块的宿主机
// 测试只关心布局/边界,不关心密码学校验,故返回确定性伪值。
esp_err_t radar_identity_get(uint8_t hash[16], uint8_t pubkey[32]) {
    memset(hash, 0x5A, 16);
    memset(pubkey, 0xA5, 32);
    return ESP_OK;
}

esp_err_t radar_identity_sign(const uint8_t *msg, size_t len, uint8_t sig[64]) {
    (void)msg;
    (void)len;
    memset(sig, 0xA5, 64);
    return ESP_OK;
}
