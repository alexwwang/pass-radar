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
// cardid mock 已随身份源弃用移除(v7 起身份 = MAC 派生,见 radar_identity)。

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

    // --- Happy path on a 1 MiB carve under AND-write semantics: begin keeps
    // the requested count as the plan (160); the append ceiling is physical
    // (v8 packed worst case = 127 records of 8192B), all appends succeed,
    // and export returns every committed record.
    reset_flash(1);
    CHECK(radar_capture_batch_open() == ESP_OK,
          "open resolves the capture DATA carve");
    CHECK(radar_capture_batch_begin(batch_id, RADAR_CAPTURE_BATCH_MAX_CAPTURES) ==
              ESP_OK,
          "begin keeps the requested count as plan");
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
    CHECK(appended == 127, "1 MiB carve holds exactly 127 worst-case captures (v8 packed)");
    uint32_t exported = 0;
    CHECK(radar_capture_batch_export(export_cb, &exported) == ESP_OK,
          "export succeeds on the DATA carve path");
    CHECK(exported == 127, "every committed record exports");

    // --- 无数量上限:小载荷(32B)每条占 1 扇区(4096B),1MiB 分区物理
    // 容量 254 条全部落盘——采集次数只受存储空间限制(设计定稿回归守护;
    // 踩雷史:v7 expected clamp 84、v8.0 残留 MAX_CAPTURES=160 条数拒采)。
    reset_flash(1);
    CHECK(radar_capture_batch_open() == ESP_OK,
          "reopen after re-begin erase");
    CHECK(radar_capture_batch_begin(batch_id + 1u, 254) == ESP_OK,
          "begin small-payload batch with plan 254");
    uint8_t small[32];
    memset(small, 0x5a, sizeof(small));
    const uint32_t small_crc = radar_capture_batch_crc32(small, sizeof(small));
    appended = 0;
    while (radar_capture_batch_can_append(sizeof(small))) {
        fill_meta(&meta, batch_id + 1u, appended + 1u);
        if (radar_capture_batch_append(&meta, small, sizeof(small),
                                       small_crc) != ESP_OK) {
            break;
        }
        appended++;
    }
    CHECK(appended == 254, "254 small-payload captures all fit (storage-only limit)");
    exported = 0;
    CHECK(radar_capture_batch_export(export_cb, &exported) == ESP_OK,
          "export succeeds on the small-payload batch");
    CHECK(exported == 254, "every small-payload record exports");

    // --- v7 身份盖章:begin 把 MAC 派生 device_hash 写进头部。身份实现
    // (efuse MAC + PSA Ed25519)只在目标机构建,宿主机用确定性桩
    // (mock_radar_identity.c,0x5A*16);真机派生链由 bring-up 与云端验签
    // 交叉对拍。cardid 方案已弃用(分区整片可擦,身份链会断)。
    static const uint8_t expect_stub[16] = {
        0x5a, 0x5a, 0x5a, 0x5a, 0x5a, 0x5a, 0x5a, 0x5a,
        0x5a, 0x5a, 0x5a, 0x5a, 0x5a, 0x5a, 0x5a, 0x5a,
    };
    radar_capture_batch_header_t hdr;

    reset_flash(1);
    CHECK(radar_capture_batch_begin(batch_id, 1) == ESP_OK,
          "begin stamps identity into the header");
    memcpy(&hdr, flash_memory, sizeof(hdr));
    CHECK(hdr.magic == RADAR_CAPTURE_BATCH_MAGIC &&
              hdr.version == RADAR_CAPTURE_BATCH_FORMAT_VERSION,
          "header magic + current format version written");
    CHECK(memcmp(hdr.device_hash, expect_stub, 16) == 0,
          "device_hash = MAC-derived identity (host stub)");
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
