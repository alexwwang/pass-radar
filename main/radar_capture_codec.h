#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RADAR_CAPTURE_CODEC_ID 1u
#define RADAR_CAPTURE_CODEC_MAX_PAYLOAD 8192u
#define RADAR_CAPTURE_MAX_RECORDS 128u
#define RADAR_CAPTURE_INVALID_U16 0xffffu

typedef struct __attribute__((packed)) {
    uint32_t elapsed_ms;
    int8_t local_rssi;
    int8_t peer_rssi;
    uint8_t flags;
    int16_t ftm_status;
    float ftm_distance_m;
    float ftm_spread_m;
    uint16_t ftm_session_count;
    int8_t ble_rssi;
    int32_t ble_age_ms;
    uint32_t csi_frames;
    uint8_t env_level;
    uint8_t reserved[3];
} radar_capture_record_t;

size_t radar_capture_codec_encode(const radar_capture_record_t *records,
                                  size_t record_count,
                                  uint8_t *out,
                                  size_t out_capacity);

bool radar_capture_codec_decode(const uint8_t *payload,
                                size_t payload_length,
                                radar_capture_record_t *records,
                                size_t record_capacity,
                                size_t expected_count);

uint32_t radar_capture_codec_crc32(const void *data, size_t length);
