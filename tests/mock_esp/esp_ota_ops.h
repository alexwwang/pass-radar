#pragma once
#ifndef PASS_RADAR_MOCK_ESP_OTA_OPS_H
#define PASS_RADAR_MOCK_ESP_OTA_OPS_H

#include "esp_partition.h"

// Test-installable partition discovery hooks. The test points slot2 at a
// static partition instance so radar_capture_batch.c's find_partition()
// takes the same code path as on hardware.
extern const esp_partition_t *g_mock_slot2_partition;
extern const esp_partition_t *g_mock_running_partition;

static inline const esp_partition_t *esp_ota_get_running_partition(void) {
    return g_mock_running_partition;
}

static inline const esp_partition_t *esp_partition_find_first(
    int type, int subtype, const void *name) {
    (void)type; (void)subtype; (void)name;
    return g_mock_slot2_partition;
}

#endif
