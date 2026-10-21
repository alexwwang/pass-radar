#pragma once
#ifndef PASS_RADAR_MOCK_ESP_PARTITION_H
#define PASS_RADAR_MOCK_ESP_PARTITION_H

#include <string.h>
#include "esp_log.h"

// Host-test flash mock: a RAM array plus global hooks installed by the test
// binary. radar_capture_batch.c calls these through the static-inline shims
// below, so the same compiled batch.c object runs against both the
// zero-fill-mock (old bug reproduction) and the AND-write mock (fix check).
typedef struct {
    uint8_t *memory;
    size_t size;
    int write_mode;  // 0 = plain copy, 1 = NOR AND-write semantics
} mock_flash_t;

extern mock_flash_t g_mock_flash;

typedef struct {
    uint32_t address;
    uint32_t size;
} esp_partition_t;

enum {
    ESP_PARTITION_TYPE_APP = 0,
    ESP_PARTITION_TYPE_DATA = 1,
    ESP_PARTITION_SUBTYPE_APP_OTA_2 = 2,
};

static inline esp_err_t esp_partition_read(const esp_partition_t *partition,
                                           size_t offset, void *data,
                                           size_t length) {
    if (!partition || !g_mock_flash.memory || offset + length > g_mock_flash.size)
        return ESP_ERR_INVALID_ARG;
    memcpy(data, g_mock_flash.memory + offset, length);
    return ESP_OK;
}

static inline esp_err_t esp_partition_write(const esp_partition_t *partition,
                                            size_t offset, const void *data,
                                            size_t length) {
    if (!partition || !g_mock_flash.memory || offset + length > g_mock_flash.size)
        return ESP_ERR_INVALID_ARG;
    const uint8_t *src = data;
    for (size_t i = 0; i < length; ++i) {
        if (g_mock_flash.write_mode == 1) {
            // Real NOR flash: bits can only be cleared, 1 -> 0.
            g_mock_flash.memory[offset + i] &= src[i];
        } else {
            // Plain RAM copy: simulates a flash that accepts any overwrite.
            g_mock_flash.memory[offset + i] = src[i];
        }
    }
    return ESP_OK;
}

static inline esp_err_t esp_partition_erase_range(
    const esp_partition_t *partition, size_t offset, size_t length) {
    if (!partition || !g_mock_flash.memory || offset + length > g_mock_flash.size)
        return ESP_ERR_INVALID_ARG;
    memset(g_mock_flash.memory + offset, 0xff, length);
    return ESP_OK;
}

#endif
