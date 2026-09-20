#include "../main/radar_capture_codec.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(condition, ...) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fputc('\n', stderr); \
        failures++; \
    } \
} while (0)

static void fill_record(radar_capture_record_t *record, unsigned i) {
    memset(record, 0, sizeof(*record));
    record->elapsed_ms = i * 303u;
    record->local_rssi = (int8_t)(-58 - (i % 5));
    record->peer_rssi = (int8_t)(-61 - (i % 4));
    record->flags = (i / 8u) & 0x0fu;
    record->ftm_status = (i % 17u == 0) ? 5 : 0;
    record->ftm_distance_m = 7.03f + (float)(i % 4u) * 0.01f;
    record->ftm_spread_m = (float)(i % 3u) * 0.23f;
    record->ftm_session_count = (uint16_t)(32u + i);
    record->ble_rssi = (int8_t)(-74 + (int)(i % 3u));
    record->ble_age_ms = (int32_t)((i % 12u) * 1000u);
    record->csi_frames = 100u + i * 2u;
    record->env_level = (uint8_t)(i / 32u);
}

static void test_round_trip(void) {
    radar_capture_record_t input[32];
    radar_capture_record_t output[32];
    uint8_t payload[RADAR_CAPTURE_CODEC_MAX_PAYLOAD];
    for (unsigned i = 0; i < 32; ++i) fill_record(&input[i], i);

    size_t encoded = radar_capture_codec_encode(input, 32, payload, sizeof(payload));
    CHECK(encoded > 0, "codec should encode records");
    CHECK(encoded < sizeof(input), "delta codec should reduce representative data");
    CHECK(radar_capture_codec_decode(payload, encoded, output, 32, 32),
          "codec should decode its payload");
    for (unsigned i = 0; i < 32; ++i) {
        CHECK(output[i].elapsed_ms == input[i].elapsed_ms, "elapsed mismatch at %u", i);
        CHECK(output[i].local_rssi == input[i].local_rssi, "local RSSI mismatch at %u", i);
        CHECK(output[i].peer_rssi == input[i].peer_rssi, "peer RSSI mismatch at %u", i);
        CHECK(output[i].flags == input[i].flags, "flags mismatch at %u", i);
        CHECK(fabsf(output[i].ftm_distance_m - input[i].ftm_distance_m) <= 0.01f,
              "distance mismatch at %u", i);
        CHECK(fabsf(output[i].ftm_spread_m - input[i].ftm_spread_m) <= 0.01f,
              "spread mismatch at %u", i);
        CHECK(output[i].ble_age_ms == input[i].ble_age_ms, "age mismatch at %u", i);
    }
    printf("  capture_codec_round_trip: PASS (%zu bytes)\n", encoded);
}

static void test_max_capture_budget(void) {
    radar_capture_record_t input[RADAR_CAPTURE_MAX_RECORDS];
    uint8_t payload[RADAR_CAPTURE_CODEC_MAX_PAYLOAD];
    for (unsigned i = 0; i < RADAR_CAPTURE_MAX_RECORDS; ++i) fill_record(&input[i], i);
    size_t encoded = radar_capture_codec_encode(input, RADAR_CAPTURE_MAX_RECORDS,
                                                payload, sizeof(payload));
    CHECK(encoded > 0, "128-record capture should encode");
    CHECK(encoded < sizeof(input), "128-record capture should be smaller than raw payload");
    printf("  capture_codec_max_budget: PASS (%zu/%zu bytes)\n",
           encoded, sizeof(input));
}

static void test_repeat_and_corruption(void) {
    radar_capture_record_t input[8];
    radar_capture_record_t output[8];
    uint8_t payload[RADAR_CAPTURE_CODEC_MAX_PAYLOAD];
    for (unsigned i = 0; i < 8; ++i) fill_record(&input[i], 0);
    size_t encoded = radar_capture_codec_encode(input, 8, payload, sizeof(payload));
    CHECK(encoded > 0 && encoded < 8 * sizeof(input[0]), "repeats should be compact");
    CHECK(radar_capture_codec_decode(payload, encoded, output, 8, 8),
          "repeated records should decode");
    CHECK(!radar_capture_codec_decode(payload, encoded - 1u, output, 8, 8),
          "truncated varint stream should be rejected");
    printf("  capture_codec_repeat_corruption: PASS\n");
}

int main(void) {
    test_round_trip();
    test_max_capture_budget();
    test_repeat_and_corruption();
    if (failures) {
        fprintf(stderr, "FAILED: %d check(s)\n", failures);
        return 1;
    }
    printf("PASS: capture codec rules verified\n");
    return 0;
}
