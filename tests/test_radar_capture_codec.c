#include "../main/radar_capture_codec.h"
#include "../main/radar_capture_budget.h"

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
    record->ftm_distance_cm = (uint16_t)(703u + (i % 4u));
    record->ftm_spread_cm = (uint16_t)((i % 3u) * 23u);
    record->ftm_session_count = (uint16_t)(32u + i);
    record->ble_rssi = (int8_t)(-74 + (int)(i % 3u));
    record->ble_age_ms = (uint16_t)((i % 12u) * 1000u);
    record->csi_frames = (uint16_t)(100u + i * 2u);
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
        CHECK(output[i].ftm_distance_cm == input[i].ftm_distance_cm,
              "distance mismatch at %u", i);
        CHECK(output[i].ftm_spread_cm == input[i].ftm_spread_cm,
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

static void test_memory_budget(void) {
    size_t required = radar_capture_budget_required(
        RADAR_CAPTURE_BUDGET_MAX_RECORDS,
        RADAR_CAPTURE_BUDGET_RAW_RECORD_BYTES,
        RADAR_CAPTURE_BUDGET_STORAGE_HEADER_BYTES +
            RADAR_CAPTURE_BUDGET_CODEC_MAX_PAYLOAD);
    CHECK(required == RADAR_CAPTURE_BUDGET_ALLOC_BYTES,
          "budget calculation mismatch: %zu", required);
    size_t original_required = RADAR_CAPTURE_BUDGET_MAX_RECORDS * 32u +
                               RADAR_CAPTURE_BUDGET_STORAGE_HEADER_BYTES +
                               RADAR_CAPTURE_BUDGET_CODEC_MAX_PAYLOAD;
    CHECK(RADAR_CAPTURE_BUDGET_MEASURED_MIN_LARGEST < original_required + 2048u,
          "original raw-record budget must not fit measured block");
    CHECK(radar_capture_budget_fits(RADAR_CAPTURE_BUDGET_MEASURED_MIN_LARGEST,
                                    required),
          "compact budget must fit measured block");
    printf("  capture_memory_budget: PASS (%zu bytes + %u margin)\n",
           required, RADAR_CAPTURE_BUDGET_SAFETY_MARGIN);
}

static void test_malformed_ranges(void) {
    radar_capture_record_t input;
    radar_capture_record_t output;
    uint8_t payload[RADAR_CAPTURE_CODEC_MAX_PAYLOAD];
    fill_record(&input, 0);
    input.elapsed_ms = 1;
    size_t encoded = radar_capture_codec_encode(&input, 1, payload,
                                                sizeof(payload));
    CHECK(encoded > 2, "single record should encode for malformed test");
    // Replace the one-byte elapsed value after TAG_FIRST with an oversized
    // five-byte uint32 varint while preserving the remaining fields.
    memmove(payload + 6, payload + 2, encoded - 2);
    payload[1] = 0x80;
    payload[2] = 0x80;
    payload[3] = 0x80;
    payload[4] = 0x80;
    payload[5] = 0x0f;
    CHECK(!radar_capture_codec_decode(payload, encoded + 4, &output, 1, 1),
          "out-of-range uint16 field must be rejected");
    puts("  capture_codec_range_validation: PASS");
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
    test_memory_budget();
    test_malformed_ranges();
    test_repeat_and_corruption();
    if (failures) {
        fprintf(stderr, "FAILED: %d check(s)\n", failures);
        return 1;
    }
    printf("PASS: capture codec rules verified\n");
    return 0;
}
