#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RADAR_CAPTURE_CODEC_ID 2u
#define RADAR_CAPTURE_MAX_RECORDS 128u
#define RADAR_CAPTURE_INVALID_U16 0xffffu

// Worst case per encoded record: 1 tag byte + 12 fields, each at most a
// 3-byte varint (uint16 fields, and int16 fields after zigzag, fit in 3
// bytes; int8/uint8 fields fit in 2). TAG_REPEAT is strictly smaller.
// The payload ceiling is therefore the exact worst case for MAX_RECORDS,
// not a rounded-up guess: 128 * 32 = 4096 bytes.
#define RADAR_CAPTURE_CODEC_WORST_RECORD_BYTES 32u
#define RADAR_CAPTURE_CODEC_MAX_PAYLOAD \
    (RADAR_CAPTURE_MAX_RECORDS * RADAR_CAPTURE_CODEC_WORST_RECORD_BYTES)

typedef struct __attribute__((packed)) {
    uint16_t elapsed_ms;
    int8_t local_rssi;
    int8_t peer_rssi;
    uint8_t flags;
    int16_t ftm_status;
    uint16_t ftm_distance_cm;
    uint16_t ftm_spread_cm;
    uint16_t ftm_session_count;
    int8_t ble_rssi;
    uint16_t ble_age_ms;
    uint16_t csi_frames;
    uint8_t env_level;
    uint8_t reserved;
} radar_capture_record_t;

_Static_assert(sizeof(radar_capture_record_t) == 20,
               "compact capture record must remain 20 bytes");

size_t radar_capture_codec_encode(const radar_capture_record_t *records,
                                  size_t record_count,
                                  uint8_t *out,
                                  size_t out_capacity);
bool radar_capture_codec_decode(const uint8_t *payload, size_t payload_length,
                                radar_capture_record_t *records,
                                size_t record_capacity, size_t expected_count);
uint32_t radar_capture_codec_crc32(const void *data, size_t length);
