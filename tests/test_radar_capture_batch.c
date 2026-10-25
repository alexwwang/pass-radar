#include "../main/radar_capture_batch.h"
#include "../main/radar_capture_budget.h"
#include "../main/radar_capture_codec.h"
#include "../main/radar_capture_condition.h"

#include <stdio.h>

static int failures;

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s\n", message); \
        failures++; \
    } \
} while (0)

int main(void) {
    CHECK(RADAR_CAPTURE_BATCH_FLASH_SIZE == 0x29D000u,
          "legacy v1 ota_2 fallback capacity stays stable");
    CHECK(RADAR_CAPTURE_BATCH_DATA_SUBTYPE == 0x81u,
          "capture carve uses the meta-pass DATA subtype whitelist slot");
    CHECK(sizeof(radar_capture_batch_header_t) == 128,
          "batch header size");
    CHECK(radar_capture_batch_data_offset() == 0x1000u,
          "first capture starts on an erase-sector boundary");
    CHECK(RADAR_CAPTURE_BATCH_MAX_PAYLOAD == RADAR_CAPTURE_CODEC_MAX_PAYLOAD,
          "batch payload ceiling must equal the codec ceiling");
    CHECK(RADAR_CAPTURE_BATCH_MAX_PAYLOAD == 4096u,
          "codec worst case is 128 records x 32 bytes");
    CHECK(radar_capture_batch_capacity_fits(RADAR_CAPTURE_BATCH_FLASH_SIZE,
                                            78,
                                            RADAR_CAPTURE_BATCH_MAX_PAYLOAD),
          "78 worst-case captures fit with aligned batch header");
    CHECK(sizeof(radar_capture_batch_capture_header_t) == 112,
          "capture header size");
    CHECK(radar_capture_batch_align(0) == 0, "zero alignment");
    CHECK(radar_capture_batch_align(SIZE_MAX) == 0,
          "alignment overflow rejected");
    CHECK(radar_capture_batch_capture_span(SIZE_MAX) == 0,
          "capture span overflow rejected");
    CHECK(!radar_capture_batch_capacity_fits(RADAR_CAPTURE_BATCH_FLASH_SIZE,
                                             1, SIZE_MAX),
          "oversized payload capacity rejected");
    CHECK(radar_capture_batch_commit_offset(0, 1466) == 128 + 1468,
          "v8 packed commit offset follows 128B slot + round4(payload)");
    CHECK(radar_capture_batch_capture_span(1466) == 4096,
          "v8 packed small-payload span is one sector");
    CHECK(radar_capture_batch_commit_offset(0, RADAR_CAPTURE_BATCH_MAX_PAYLOAD) ==
              128 + 4096,
          "v8 worst payload commit offset follows 128B slot + payload");
    CHECK(radar_capture_batch_capture_span(RADAR_CAPTURE_BATCH_MAX_PAYLOAD) ==
              8192,
          "v8 worst payload span is two sectors");
    CHECK(radar_capture_batch_capacity_fits(RADAR_CAPTURE_BATCH_FLASH_SIZE,
                                            78,
                                            RADAR_CAPTURE_BATCH_MAX_PAYLOAD),
          "78 worst-case captures fit legacy ota_2 data area");
    CHECK(radar_capture_batch_capacity_fits(RADAR_CAPTURE_BATCH_FLASH_SIZE,
                                            160,
                                            RADAR_CAPTURE_BATCH_MAX_PAYLOAD),
          "160 worst-case captures fit legacy ota_2 capacity");
    CHECK(!radar_capture_batch_capacity_fits(RADAR_CAPTURE_BATCH_FLASH_SIZE,
                                             161,
                                             RADAR_CAPTURE_BATCH_MAX_PAYLOAD),
          "161 captures exceed the configured batch limit");
    // 1 MiB dynslot carve (the declared minimum): v8 紧凑布局最坏
    // align(128+4096+16)=8192B/条。capacity_fits 按裸容量算 = 127 条;
    // begin() 再减 trailer 扇区 → 实际 clamp 126(见 data 测试)。
    CHECK(radar_capture_batch_capacity_fits(0x100000u, 127,
                                            RADAR_CAPTURE_BATCH_MAX_PAYLOAD),
          "127 worst-case captures fit the 1 MiB declared carve (v8)");
    CHECK(!radar_capture_batch_capacity_fits(0x100000u, 128,
                                             RADAR_CAPTURE_BATCH_MAX_PAYLOAD),
          "128 worst-case captures exceed the 1 MiB declared carve (v8)");
    CHECK(radar_capture_batch_capacity_fits(0x200000u, 160,
                                            RADAR_CAPTURE_BATCH_MAX_PAYLOAD),
          "160 worst-case captures fit a 2 MiB carve");
    CHECK(RADAR_CAPTURE_SCENE_I_LOS == (1u << 8),
          "indoor LOS scene encoding is stable");
    CHECK(RADAR_CAPTURE_SCENE_O_DYNAMIC ==
              ((RADAR_CAPTURE_ENV_OUTDOOR << 8) |
               (RADAR_CAPTURE_LINK_DYNAMIC_ATTENUATION << 4)),
          "outdoor dynamic scene encoding is stable");
    CHECK(RADAR_CAPTURE_SCENE_I_MULTIPATH ==
              ((RADAR_CAPTURE_ENV_INDOOR << 8) |
               (RADAR_CAPTURE_LINK_MULTIPATH << 4)),
          "indoor multipath scene encoding is stable");
    CHECK(radar_capture_context_option_available(
              RADAR_CAPTURE_ENV_OUTDOOR, RADAR_CAPTURE_LINK_LOS,
              RADAR_CAPTURE_CONTEXT_SPACE,
              RADAR_CAPTURE_ROOM_GEOMETRY_OUTDOOR_OPEN),
          "outdoor open space is available");
    CHECK(!radar_capture_context_option_available(
              RADAR_CAPTURE_ENV_OUTDOOR, RADAR_CAPTURE_LINK_LOS,
              RADAR_CAPTURE_CONTEXT_SPACE,
              RADAR_CAPTURE_ROOM_GEOMETRY_ROOM),
          "indoor room is disabled outdoors");
    CHECK(!radar_capture_context_option_available(
              RADAR_CAPTURE_ENV_INDOOR, RADAR_CAPTURE_LINK_DYNAMIC_ATTENUATION,
              RADAR_CAPTURE_CONTEXT_OBSTACLE,
              RADAR_CAPTURE_OBSTRUCTION_FOLIAGE),
          "indoor foliage is disabled");
    CHECK(radar_capture_context_option_available_for_space(
              RADAR_CAPTURE_ENV_OUTDOOR, RADAR_CAPTURE_LINK_LOS,
              RADAR_CAPTURE_CONTEXT_ROOM_SIZE,
              RADAR_CAPTURE_ROOM_SIZE_NOT_APPLICABLE,
              RADAR_CAPTURE_ROOM_GEOMETRY_OUTDOOR_OPEN),
          "outdoor room size uses N/A");
    CHECK(!radar_capture_context_option_available_for_space(
              RADAR_CAPTURE_ENV_OUTDOOR, RADAR_CAPTURE_LINK_LOS,
              RADAR_CAPTURE_CONTEXT_ROOM_SIZE,
              RADAR_CAPTURE_ROOM_SIZE_SMALL,
              RADAR_CAPTURE_ROOM_GEOMETRY_OUTDOOR_OPEN),
          "outdoor room size tiers are unavailable");
    CHECK(radar_capture_context_option_available(
              RADAR_CAPTURE_ENV_OUTDOOR, RADAR_CAPTURE_LINK_DYNAMIC_ATTENUATION,
              RADAR_CAPTURE_CONTEXT_OBSTACLE,
              RADAR_CAPTURE_OBSTRUCTION_FOLIAGE),
          "outdoor foliage remains an available dynamic context");
    CHECK(radar_capture_condition_has_warning(
              RADAR_CAPTURE_ENV_INDOOR, RADAR_CAPTURE_LINK_MULTIPATH,
              RADAR_CAPTURE_OBSTRUCTION_NONE,
              RADAR_CAPTURE_ROOM_GEOMETRY_CORRIDOR),
          "multipath without explicit obstacle warns");
    CHECK(radar_capture_condition_has_warning(
              RADAR_CAPTURE_ENV_INDOOR, RADAR_CAPTURE_LINK_LOS,
              RADAR_CAPTURE_OBSTRUCTION_WALL,
              RADAR_CAPTURE_ROOM_GEOMETRY_ROOM),
          "LOS with wall context warns");
    CHECK(radar_capture_condition_scene_id(
              RADAR_CAPTURE_ENV_INDOOR, RADAR_CAPTURE_LINK_STATIC_ATTENUATION) ==
              RADAR_CAPTURE_SCENE_I_STATIC,
          "condition scene ID matches core dimensions");
    CHECK(radar_capture_condition_card_height_from_m(0.99f) ==
              RADAR_CAPTURE_CARD_HEIGHT_LOW,
          "height below one meter is low");
    CHECK(radar_capture_condition_card_height_from_m(1.00f) ==
              RADAR_CAPTURE_CARD_HEIGHT_CHEST,
          "one meter is chest range");
    CHECK(radar_capture_condition_card_height_from_m(1.79f) ==
              RADAR_CAPTURE_CARD_HEIGHT_CHEST,
          "height below 1.8 meters is chest range");
    CHECK(radar_capture_condition_card_height_from_m(1.80f) ==
              RADAR_CAPTURE_CARD_HEIGHT_OVERHEAD,
          "1.8 meters is overhead range");
    CHECK(radar_capture_condition_propagation(RADAR_CAPTURE_LINK_LOS) ==
              RADAR_CAPTURE_PROPAGATION_LOS,
          "LOS propagation mapping");
    CHECK(radar_capture_condition_propagation(RADAR_CAPTURE_LINK_STATIC_ATTENUATION) ==
              RADAR_CAPTURE_PROPAGATION_NLOS,
          "static attenuation propagation mapping");
    CHECK(radar_capture_condition_propagation(RADAR_CAPTURE_LINK_DYNAMIC_ATTENUATION) ==
              RADAR_CAPTURE_PROPAGATION_NLOS,
          "dynamic attenuation propagation mapping");
    CHECK(radar_capture_condition_propagation(RADAR_CAPTURE_LINK_MULTIPATH) ==
              RADAR_CAPTURE_PROPAGATION_MULTIPATH,
          "multipath propagation mapping");
    CHECK(radar_capture_batch_capacity_fits(RADAR_CAPTURE_BATCH_FLASH_SIZE,
                                            78, 1466),
          "78 observed-size captures fit legacy ota_2 data area");
    CHECK(radar_capture_budget_fits(RADAR_CAPTURE_BUDGET_MEASURED_MIN_LARGEST,
                                    RADAR_CAPTURE_BUDGET_RAW_RECORD_BYTES *
                                        RADAR_CAPTURE_BUDGET_MAX_RECORDS),
          "records block fits the measured 8192-byte floor");
    CHECK(radar_capture_budget_fits(
              RADAR_CAPTURE_BUDGET_MEASURED_MIN_LARGEST,
              RADAR_CAPTURE_BUDGET_STORAGE_HEADER_BYTES +
                  RADAR_CAPTURE_BUDGET_CODEC_MAX_PAYLOAD),
          "scratch block fits the measured 8192-byte floor");
    CHECK(RADAR_CAPTURE_CODEC_MAX_PAYLOAD == RADAR_CAPTURE_BATCH_MAX_PAYLOAD,
          "codec and batch payload ceilings agree");

    if (failures != 0) return 1;
    puts("capture_batch_layout: PASS");
    return 0;
}
