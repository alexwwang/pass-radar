// Host test: verifies the meta-pass v2 (dynslot) primary storage path —
// the child-declared "capture" DATA carve resolved by label. Covers: region
// floor rejection, occupied-region rejection, expected-capture clamping to
// the partition-derived capacity, and a full append/export round trip under
// real NOR AND-write semantics on a 1 MiB carve (the declared minimum,
// bounding the batch to 85 worst-case captures).
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

#define DATA_CARVE_SIZE 0x100000u  // declared partitions.csv extent

static uint8_t flash_memory[DATA_CARVE_SIZE];
static esp_partition_t mock_partition = {
    .address = 0x360000,
    .size = DATA_CARVE_SIZE,
};
mock_flash_t g_mock_flash;  // defined here; esp_partition.h declares it extern
const esp_partition_t *g_mock_data_partition = &mock_partition;
const esp_partition_t *g_mock_slot2_partition = NULL;  // no legacy ota_2
const esp_partition_t *g_mock_running_partition = NULL;
// 默认无 cardid(全 0xFF 退化路径);身份用例在测试体内重装带 backing 的分区。
const esp_partition_t *g_mock_cardid_partition = NULL;

static void reset_flash(int write_mode) {
    memset(flash_memory, 0xff, sizeof(flash_memory));
    g_mock_flash.memory = flash_memory;
    g_mock_flash.size = sizeof(flash_memory);
    g_mock_flash.write_mode = write_mode;
}

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

int main(void) {
    const uint32_t batch_id = 493512032u;
    static uint8_t worst[RADAR_CAPTURE_BATCH_MAX_PAYLOAD];
    for (size_t i = 0; i < sizeof(worst); ++i) {
        worst[i] = (uint8_t)(i ^ 0xa5);
    }
    const uint32_t worst_crc = radar_capture_batch_crc32(worst, sizeof(worst));

    // --- Region floor: a carve below header + one worst-case span is
    // rejected, not silently wrapped.
    reset_flash(1);
    mock_partition.size = 0x3000u;
    CHECK(radar_capture_batch_open() == ESP_ERR_INVALID_STATE,
          "carve below the usable floor is rejected");

    // --- Occupied region: non-batch content is never erased or appended over.
    reset_flash(1);
    mock_partition.size = DATA_CARVE_SIZE;
    flash_memory[0] = 0x00;  // poison: neither erased nor an RCB1 header
    CHECK(radar_capture_batch_open() == ESP_ERR_INVALID_STATE,
          "occupied carve is rejected");

    // --- Happy path on a 1 MiB carve under AND-write semantics: begin(160)
    // must clamp to the 85 captures that fit (3 sectors per capture), all
    // appends succeed, and export returns every committed record.
    reset_flash(1);
    CHECK(radar_capture_batch_open() == ESP_OK,
          "open resolves the capture DATA carve");
    CHECK(radar_capture_batch_begin(batch_id, RADAR_CAPTURE_BATCH_MAX_CAPTURES) ==
              ESP_OK,
          "begin clamps the requested count instead of failing");
    radar_capture_batch_capture_meta_t meta;
    uint32_t appended = 0;
    while (radar_capture_batch_can_append(RADAR_CAPTURE_BATCH_MAX_PAYLOAD)) {
        fill_meta(&meta, batch_id, appended + 1u);
        if (radar_capture_batch_append(&meta, worst, sizeof(worst),
                                       worst_crc) != ESP_OK) {
            break;
        }
        appended++;
    }
    CHECK(appended == 85, "1 MiB carve holds exactly 85 worst-case captures");
    uint32_t exported = 0;
    CHECK(radar_capture_batch_export(export_cb, &exported) == ESP_OK,
          "export succeeds on the DATA carve path");
    CHECK(exported == 85, "every committed record exports");

    // --- v6+ 身份盖章:begin 把 SHA-256(cardid 前 16B) 截断写进头部
    // device_hash;期望值用 Python hashlib 独立计算(be45cb26…),与固件内
    // mbedtls 实现跨实现对拍。无 cardid 分区走全 0xFF 退化(5ac6a594…)。
    static const uint8_t cardid_bytes[16] = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
    };
    static const uint8_t expect_hash[16] = {
        0xbe, 0x45, 0xcb, 0x26, 0x05, 0xbf, 0x36, 0xbe,
        0xbd, 0xe6, 0x84, 0x84, 0x1a, 0x28, 0xf0, 0xfd,
    };
    static const uint8_t expect_erased[16] = {
        0x5a, 0xc6, 0xa5, 0x94, 0x5f, 0x16, 0x50, 0x09,
        0x11, 0x21, 0x91, 0x29, 0x98, 0x4b, 0xa8, 0xb3,
    };
    static const esp_partition_t mock_cardid = {
        .address = 0x356000,
        .size = 0x4000,
        .backing = cardid_bytes,
    };
    radar_capture_batch_header_t hdr;

    reset_flash(1);
    g_mock_cardid_partition = &mock_cardid;
    CHECK(radar_capture_batch_begin(batch_id, 1) == ESP_OK,
          "begin with cardid partition installed");
    memcpy(&hdr, flash_memory, sizeof(hdr));
    CHECK(hdr.magic == RADAR_CAPTURE_BATCH_MAGIC && hdr.version == 6,
          "header magic + v6 version written");
    CHECK(memcmp(hdr.device_hash, expect_hash, 16) == 0,
          "device_hash = SHA-256(cardid[0:16]) truncated (cross-checked)");
    g_mock_cardid_partition = NULL;

    reset_flash(1);
    CHECK(radar_capture_batch_begin(batch_id, 1) == ESP_OK,
          "begin without cardid partition still works");
    memcpy(&hdr, flash_memory, sizeof(hdr));
    CHECK(memcmp(hdr.device_hash, expect_erased, 16) == 0,
          "no cardid -> deterministic erased-input fallback hash");
    CHECK(hdr.header_crc32 != 0, "header crc covers the stamped hash");

    // --- 上行读取接口:容量可见、越界拒绝、不要求活动批次。
    reset_flash(1);
    CHECK(radar_capture_batch_data_size() == DATA_CARVE_SIZE,
          "data_size exposes the carve capacity");
    CHECK(radar_capture_batch_data_read(0, &hdr, sizeof(hdr)) == ESP_OK,
          "data_read serves the header without an active batch");
    CHECK(radar_capture_batch_data_read(DATA_CARVE_SIZE - 8, &hdr, 16) ==
              ESP_ERR_INVALID_SIZE,
          "data_read rejects out-of-bounds range");

    if (failures != 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("capture batch dynslot DATA carve: PASS");
    return 0;
}
