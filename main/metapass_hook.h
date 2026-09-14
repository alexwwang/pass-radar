// main/metapass_hook.h —— 子固件适配 meta-pass 启动器的最小 hook(header-only)。
//
// 用法(子固件):
//   1. 启动自检通过后调用 metapass_mark_valid()  → 跨重启常驻,否则下次重启自动回启动器。
//      ★ 内部先验签:签名徽章存在且验签通过才允许常驻;未签名返回 ESP_ERR_NOT_SUPPORTED,
//        OTA 保持"待验证"态 → 下次重启自动回退启动器(trial boot)。
//   2. 在按键处理里响应 OK 键的 BSP_BTN_LONG2 → 调 metapass_return_to_launcher() 退回启动器。
//      (注意:LONG2 触发前会先触发一次 LONG,应用内"返回"与"退回启动器"会先后发生;
//       设计应用交互时把 LONG 安排为可安全先发生的动作,如返回上一页。)
//
// 只有返回启动器和常驻两个能力需要适配;不适配的子固件表现为"试运行"(重启即回启动器)。
//
// CMakeLists 依赖:REQUIRES mbedtls
// 签名徽章格式见 docs/assets/meta-pass-design.md §7。
// 彩蛋数据格式见 docs/assets/meta-pass-design.md §7.1。
// 签名工具:tools/signing/sign-firmware.sh <app.bin> [private.pem] [--egg-text "..."]

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "esp_err.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_image_format.h"   // esp_image_verify / esp_image_metadata_t
#include "mbedtls/sha256.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509.h"

// ── 签名徽章格式(与 main/meta_sign.h 一致) ──────────────────────────
#define MP_SIG_MAGIC     "MSIG"
#define MP_SIG_HDR_LEN   8u   // magic(4) + payload_len(4)
#define MP_SIG_SIG_LEN   72u  // ECDSA-P256 DER 签名最大长度
#define MP_SIG_TOTAL     81u  // 8 + 72 + 1(xor), 可变长度(实际取 payload_len)
#define MP_SIG_SECTOR    4096u

// ── 彩蛋格式(同一 metadata sector 中部,不影响 MSIG) ───────────────
// MAEG 字段定长 3928B:magic(4) + payload_len(4) + text 区(3919B, 0xFF padding) + xor(1)。
// xor 固定在窗口末字节(MP_EGG_XOR_OFF),覆盖前 3927 字节(含 padding);位置与文本长度无关。
#define MP_EGG_MAGIC       "MAEG"
#define MP_EGG_HDR_LEN     8u    // magic(4) + payload_len(4)
#define MP_EGG_TEXT_LEN    3919u // ASCII payload max
#define MP_EGG_WINDOW_OFF  128u  // 彩蛋窗口: 128..4055
#define MP_EGG_TOTAL       (MP_EGG_HDR_LEN + MP_EGG_TEXT_LEN + 1u)
#define MP_EGG_XOR_OFF     (MP_EGG_HDR_LEN + MP_EGG_TEXT_LEN)
// meta-pass 内置公钥(ECDSA-P256, SubjectPublicKeyInfo DER)。
// 生成:tools/signing/gen-pubkey.py(从 private.pem 读取真实 DER,勿手抄)
// 更新此数组后所有子固件需重新编译才能验证新签名。
// MP_PUBKEY_BEGIN — 下方数组由 tools/signing/gen-pubkey.py 生成,勿手改
#define MP_PUBKEY_LEN 91
static const unsigned char mp_pubkey_der[] = {
  0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02,
  0x01, 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07, 0x03,
  0x42, 0x00, 0x04, 0x61, 0x46, 0xdb, 0x54, 0x40, 0x17, 0xbd, 0xa4, 0xa1,
  0xac, 0xc2, 0x2f, 0x15, 0x53, 0xe9, 0x83, 0x04, 0x8e, 0x37, 0x14, 0x66,
  0x38, 0xee, 0x21, 0xb1, 0x28, 0x2d, 0x79, 0xa0, 0x16, 0x1f, 0x7b, 0x84,
  0xe9, 0xf3, 0xf2, 0xc5, 0xb8, 0x75, 0x13, 0xb5, 0x1e, 0x26, 0x5d, 0x1f,
  0xec, 0x53, 0x76, 0xdc, 0x7a, 0xda, 0x24, 0x62, 0x68, 0x30, 0x6a, 0x98,
  0x7a, 0xa8, 0x00, 0x86, 0xf6, 0x61, 0x62,
};
// MP_PUBKEY_END

// ── 内部:流式计算分区前 n 字节 SHA-256(4KB 分块,无整包入 RAM) ──────
static int mp_stream_sha256(const esp_partition_t *part, uint32_t len, uint8_t out[32])
{
    uint8_t *buf = malloc(4096);
    if (!buf) return -1;
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    for (uint32_t off = 0; off < len;) {
        uint32_t chunk = (len - off < 4096) ? (len - off) : 4096;
        if (esp_partition_read(part, off, buf, chunk) != ESP_OK) {
            free(buf); mbedtls_sha256_free(&sha); return -1;
        }
        mbedtls_sha256_update(&sha, buf, chunk);
        off += chunk;
    }
    free(buf);
    mbedtls_sha256_finish(&sha, out);
    mbedtls_sha256_free(&sha);
    return 0;
}

// ── 验签:检查当前运行固件是否携带有效签名徽章 ──────────────────────
// 布局:[app image (image_len)] [metadata sector (4KB: MSIG/MAEG/MNAM)]
// esp_image_verify 只校验 image_len 范围;metadata sector 在其后,安全。
// 返回 true = 签名有效;false = 无签名或验签失败。
static bool mp_is_current_app_signed(void)
{
    const esp_partition_t *part = esp_ota_get_running_partition();
    if (!part) return false;

    // 1. 取 image_len
    esp_image_metadata_t meta = {0};
    esp_partition_pos_t pos = { .offset = part->address, .size = part->size };
    if (esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &meta) != ESP_OK) return false;

    // 2. metadata sector 偏移:image_len 之后,4K 对齐;整个 sector 必须在分区内
    uint32_t sig_off = (meta.image_len + MP_SIG_SECTOR - 1u) & ~(MP_SIG_SECTOR - 1u);
    if (sig_off + MP_SIG_SECTOR > part->size) return false; // 空间不够

    // 3. 读签名 sector 前 81 字节
    uint8_t sig[MP_SIG_TOTAL];
    if (esp_partition_read(part, sig_off, sig, sizeof(sig)) != ESP_OK) return false;

    // 4. 检查 magic
    if (memcmp(sig, MP_SIG_MAGIC, 4) != 0) return false;

    // 5. payload_len
    uint32_t payload_len = (uint32_t)sig[4]
                         | ((uint32_t)sig[5] << 8)
                         | ((uint32_t)sig[6] << 16)
                         | ((uint32_t)sig[7] << 24);
    if (payload_len == 0 || payload_len > MP_SIG_SIG_LEN) return false;

    // 6. xor checksum(header+signature 的异或应等于紧随其后的 1 字节)
    uint32_t xor_off = MP_SIG_HDR_LEN + payload_len;
    uint8_t xor = 0;
    for (uint32_t i = 0; i < xor_off; i++) xor ^= sig[i];
    if (xor != sig[xor_off]) return false;

    // 7. 计算 image 的 SHA-256
    uint8_t digest[32];
    if (mp_stream_sha256(part, meta.image_len, digest) != 0) return false;

    // 8. ECDSA-P256 验签(DER 编码,payload_len 字节)
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    if (mbedtls_pk_parse_public_key(&pk, mp_pubkey_der, MP_PUBKEY_LEN) != 0) {
        mbedtls_pk_free(&pk); return false;
    }
    int rc = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256,
                               digest, 32,
                               sig + MP_SIG_HDR_LEN, payload_len);
    mbedtls_pk_free(&pk);
    return rc == 0;
}

// ── 自检通过后调用:标记当前固件有效,取消自动回滚 ────────────────────
// 签名固件 → 允许常驻;未签名 → 返回错误,OTA 保持待验证态 → 下次重启自动回启动器。
// 返回 ESP_OK 表示已常驻;ESP_ERR_NOT_SUPPORTED 表示未签名(试运行);
// 其他值表示非 OTA 启动(如直接从 factory 调试运行),可忽略。
static inline esp_err_t metapass_mark_valid(void)
{
    if (!mp_is_current_app_signed()) {
        return ESP_ERR_NOT_SUPPORTED;  // 未签名 → 不常驻,下次重启回启动器
    }
    return esp_ota_mark_app_valid_cancel_rollback();
}

// 把启动分区切回 factory(meta-pass 启动器)并立即重启,不返回。
static inline void metapass_return_to_launcher(void)
{
    const esp_partition_t *factory =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    if (factory) {
        esp_ota_set_boot_partition(factory);
    }
    esp_restart();
}
