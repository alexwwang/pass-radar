// Host test: verifies radar_capture_batch append/export survives real NOR
// flash AND-write semantics, and demonstrates the historical failure mode:
// a zero-filled padded header write used to clear the flash bytes that the
// subsequent payload write needed, corrupting the payload CRC on export
// (on-device symptom: append OK, export ESP_ERR_INVALID_CRC).
#include "radar_capture_batch.h"
#include "radar_capture_condition.h"

#include "esp_partition.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s\n", (message)); \
        failures++; \
    } \
} while (0)

static uint8_t flash_memory[RADAR_CAPTURE_BATCH_LEGACY_SLOT2_SIZE];
static esp_partition_t mock_partition = {
    .address = 0x560000,
    .size = RADAR_CAPTURE_BATCH_LEGACY_SLOT2_SIZE,
};
mock_flash_t g_mock_flash;  // defined here; esp_partition.h declares it extern
const esp_partition_t *g_mock_data_partition = NULL;
const esp_partition_t *g_mock_slot2_partition = &mock_partition;
const esp_partition_t *g_mock_running_partition = NULL;
const esp_partition_t *g_mock_cardid_partition = NULL;

static void fill_meta(radar_capture_batch_capture_meta_t *meta,
                      uint32_t batch_id, uint32_t run_id) {
    memset(meta, 0, sizeof(*meta));
    meta->batch_id = batch_id;
    meta->run_id = run_id;
    meta->capture_id = run_id;
    meta->scene_id = RADAR_CAPTURE_SCENE_I_LOS;
    meta->condition_environment = RADAR_CAPTURE_ENV_INDOOR;
    meta->condition_link = RADAR_CAPTURE_LINK_LOS;
    meta->condition_obstruction = RADAR_CAPTURE_OBSTRUCTION_NONE;
    meta->condition_schema_version = RADAR_CAPTURE_CONDITION_SCHEMA_VERSION;
    meta->condition_propagation = RADAR_CAPTURE_PROPAGATION_LOS;
    meta->observed_level = RADAR_CAPTURE_OBS_LEVEL_L1_INDOOR;
    meta->observed_obstruction = RADAR_CAPTURE_OBS_CLASS_UNKNOWN;
    meta->room_geometry = RADAR_CAPTURE_ROOM_GEOMETRY_ROOM;
    meta->room_size = RADAR_CAPTURE_ROOM_SIZE_LARGE;
    meta->material = RADAR_CAPTURE_MATERIAL_MIXED;
    meta->furniture_density = RADAR_CAPTURE_FURNITURE_LOW;
    meta->boundary = RADAR_CAPTURE_BOUNDARY_OPEN;
    meta->card_height = RADAR_CAPTURE_CARD_HEIGHT_CHEST;
    meta->card_pose = RADAR_CAPTURE_CARD_POSE_UPRIGHT;
    meta->target_distance_m = 3.0f;
    meta->measured_distance_m = -1.0f;
    meta->record_count = 4;
    meta->boot_sequence = 43;
    meta->reset_reason = 3;
}

static esp_err_t export_cb(const radar_capture_batch_capture_header_t *header,
                           const uint8_t *payload, size_t payload_length,
                           void *context) {
    (void)header; (void)payload; (void)payload_length;
    (*(uint32_t *)context)++;
    return ESP_OK;
}

static void reset_flash(int write_mode) {
    memset(flash_memory, 0xff, sizeof(flash_memory));
    g_mock_flash.memory = flash_memory;
    g_mock_flash.size = sizeof(flash_memory);
    g_mock_flash.write_mode = write_mode;
    g_mock_data_partition = NULL;   // force the legacy ota_2 fallback path
    g_mock_slot2_partition = &mock_partition;
    g_mock_running_partition = NULL;
}

int main(void) {
    const uint32_t batch_id = 493512032u;
    uint8_t payload[1505];
    for (size_t i = 0; i < sizeof(payload); ++i) {
        payload[i] = (uint8_t)(i * 7 + 3);
    }
    uint32_t payload_crc = radar_capture_batch_crc32(payload, sizeof(payload));

    // --- Plain-copy mock (any-overwrite flash): with the fixed 0xFF padding
    // append and export must succeed regardless of write semantics.
    radar_capture_batch_capture_meta_t meta;
    reset_flash(0);
    CHECK(radar_capture_batch_open() == ESP_OK, "open on plain-copy mock");
    CHECK(radar_capture_batch_begin(batch_id, 2u) == ESP_OK,
          "begin batch on plain-copy mock");
    fill_meta(&meta, batch_id, 1u);
    CHECK(radar_capture_batch_append(&meta, payload, sizeof(payload),
                                     payload_crc) == ESP_OK,
          "append succeeds with 0xFF padding (plain-copy mock)");
    uint32_t exported = 0;
    CHECK(radar_capture_batch_export(export_cb, &exported) == ESP_OK,
          "export succeeds with 0xFF padding (plain-copy mock)");
    CHECK(exported == 1, "one record exported (plain-copy mock)");

    // --- Real NOR semantics: AND-write mock. This is the mock that caught
    // the original bug: append with 0x00 padding succeeded, but the AND
    // write corrupted the payload and export failed with INVALID_CRC.
    reset_flash(1);
    CHECK(radar_capture_batch_open() == ESP_OK, "open on AND-write mock");
    CHECK(radar_capture_batch_begin(batch_id, 2u) == ESP_OK,
          "begin batch on AND-write mock");
    fill_meta(&meta, batch_id, 1u);
    CHECK(radar_capture_batch_append(&meta, payload, sizeof(payload),
                                     payload_crc) == ESP_OK,
          "append succeeds with 0xFF padding (AND-write mock)");
    exported = 0;
    CHECK(radar_capture_batch_export(export_cb, &exported) == ESP_OK,
          "export succeeds with 0xFF padding (AND-write mock)");
    CHECK(exported == 1, "one record exported (AND-write mock)");

    // --- AND-write round-trip with the worst-case payload length too.
    reset_flash(1);
    static uint8_t worst[RADAR_CAPTURE_BATCH_MAX_PAYLOAD];
    for (size_t i = 0; i < sizeof(worst); ++i) {
        worst[i] = (uint8_t)(i ^ 0xa5);
    }
    CHECK(radar_capture_batch_open() == ESP_OK, "re-open on AND-write mock");
    CHECK(radar_capture_batch_begin(batch_id, 1u) == ESP_OK,
          "begin worst-case batch");
    fill_meta(&meta, batch_id, 1u);
    CHECK(radar_capture_batch_append(&meta, worst, sizeof(worst),
                                     radar_capture_batch_crc32(
                                         worst, sizeof(worst))) == ESP_OK,
          "append worst-case payload (AND-write mock)");
    exported = 0;
    CHECK(radar_capture_batch_export(export_cb, &exported) == ESP_OK,
          "export worst-case payload (AND-write mock)");
    CHECK(exported == 1, "one worst-case record exported");

    // --- FORMAT_VERSION 4 header validation: raw session aggregates live in
    // the former reserved area; v3 records read back as all-zero (sessions=0).
    {
        radar_capture_batch_capture_header_t header;
        memset(&header, 0, sizeof(header));
        header.magic = RADAR_CAPTURE_BATCH_CAPTURE_MAGIC;
        header.version = RADAR_CAPTURE_BATCH_FORMAT_VERSION;
        header.header_size = sizeof(header);
        header.batch_id = 1u;
        header.run_id = 1u;
        header.scene_id = RADAR_CAPTURE_SCENE_I_LOS;
        header.condition_environment = RADAR_CAPTURE_ENV_INDOOR;
        header.condition_link = RADAR_CAPTURE_LINK_LOS;
        header.condition_obstruction = RADAR_CAPTURE_OBSTRUCTION_NONE;
        header.condition_propagation = RADAR_CAPTURE_PROPAGATION_LOS;
        header.condition_schema_version = RADAR_CAPTURE_CONDITION_SCHEMA_VERSION;
        header.observed_level = RADAR_CAPTURE_OBS_LEVEL_L1_INDOOR;
        header.observed_obstruction = RADAR_CAPTURE_OBS_CLASS_UNKNOWN;
        header.room_geometry = RADAR_CAPTURE_ROOM_GEOMETRY_ROOM;
        header.room_size = RADAR_CAPTURE_ROOM_SIZE_LARGE;
        header.true_bearing = RADAR_CAPTURE_BEARING_UNSET;
        header.material = RADAR_CAPTURE_MATERIAL_MIXED;
        header.furniture_density = RADAR_CAPTURE_FURNITURE_LOW;
        header.boundary = RADAR_CAPTURE_BOUNDARY_OPEN;
        header.card_height = RADAR_CAPTURE_CARD_HEIGHT_CHEST;
        header.card_pose = RADAR_CAPTURE_CARD_POSE_UPRIGHT;
        header.target_distance_m = 3.0f;
        header.measured_distance_m = -1.0f;
        header.record_count = 4u;
        header.payload_length = 64u;
        // v3 compatibility: reserved-zero pattern decodes as sessions=0.
        CHECK(radar_capture_batch_capture_header_valid(&header),
              "v4 header accepts the v3 zero-aggregate pattern");
        header.ftm_raw_sessions = 3u;
        header.ftm_raw_median_m = 1.42f;
        header.ftm_raw_iqr_m = 0.08f;
        header.ftm_raw_bursts = 36u;
        CHECK(radar_capture_batch_capture_header_valid(&header),
              "v4 header accepts populated raw session aggregates");
        header.ftm_raw_median_m = 0.0f;
        CHECK(!radar_capture_batch_capture_header_valid(&header),
              "sessions>0 with zero median is rejected");
        header.ftm_raw_median_m = 1.42f;
        header.ftm_raw_bursts = 0u;
        CHECK(!radar_capture_batch_capture_header_valid(&header),
              "sessions>0 with zero bursts is rejected");
        header.ftm_raw_bursts = 36u;
        header.ftm_raw_median_m = -0.5f;
        CHECK(!radar_capture_batch_capture_header_valid(&header),
              "negative raw median is rejected");
        header.ftm_raw_median_m = 1.42f;
        header.ftm_raw_iqr_m = -0.1f;
        CHECK(!radar_capture_batch_capture_header_valid(&header),
              "negative raw IQR is rejected");
        header.ftm_raw_iqr_m = 0.08f;
        header.ftm_raw_median_m = 2000.0f;
        CHECK(!radar_capture_batch_capture_header_valid(&header),
              "raw median beyond range is rejected");
    }

    if (failures != 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("capture batch AND-write semantics: PASS");
    return 0;
}
