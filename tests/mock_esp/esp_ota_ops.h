#pragma once
#ifndef PASS_RADAR_MOCK_ESP_OTA_OPS_H
#define PASS_RADAR_MOCK_ESP_OTA_OPS_H

#include <string.h>
#include "esp_partition.h"

// Test-installable partition discovery hooks. Each global points at a static
// partition instance (or NULL) so radar_capture_batch.c's find_partition()
// takes the same code paths as on hardware:
//   - g_mock_data_partition: the meta-pass v2 "capture" DATA carve; non-NULL
//     exercises the primary lookup, NULL forces the legacy ota_2 fallback.
//   - g_mock_slot2_partition: the meta-pass v1 empty ota_2 dual-use slot.
extern const esp_partition_t *g_mock_data_partition;
extern const esp_partition_t *g_mock_slot2_partition;
extern const esp_partition_t *g_mock_running_partition;
// cardid 分区(radar_capture_device_hash 用)。每个测试二进制必须定义
// (不需要身份时赋 NULL → 固件走全 0xFF 的"未身份"退化路径);需要覆盖
// 身份的测试安装带 backing 的分区实例。
extern const esp_partition_t *g_mock_cardid_partition;

static inline const esp_partition_t *esp_ota_get_running_partition(void) {
    return g_mock_running_partition;
}

// Mirrors the production discovery contract (radar_capture_batch.c
// find_partition): DATA subtype 0x81 under label "capture" first, then the
// APP ota_2 slot. Unknown queries return NULL.
static inline const esp_partition_t *esp_partition_find_first(
    int type, int subtype, const void *name) {
    if (type == ESP_PARTITION_TYPE_DATA && subtype == 0x81 &&
        name != NULL && strcmp((const char *)name, "capture") == 0) {
        return g_mock_data_partition;
    }
    if (type == ESP_PARTITION_TYPE_APP &&
        subtype == ESP_PARTITION_SUBTYPE_APP_OTA_2 && name == NULL) {
        return g_mock_slot2_partition;
    }
    if (type == ESP_PARTITION_TYPE_DATA &&
        subtype == ESP_PARTITION_SUBTYPE_ANY && name != NULL &&
        strcmp((const char *)name, "cardid") == 0) {
        return g_mock_cardid_partition;
    }
    return NULL;
}

#endif
