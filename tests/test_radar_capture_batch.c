#include "../main/radar_capture_batch.h"

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
          "full usable ota_2 capacity");
    CHECK(sizeof(radar_capture_batch_header_t) == 128,
          "batch header size");
    CHECK(radar_capture_batch_data_offset() == 0x1000u,
          "first capture starts on an erase-sector boundary");
    CHECK(radar_capture_batch_capacity_fits(78, 8192),
          "78 worst-case captures fit with aligned batch header");
    CHECK(sizeof(radar_capture_batch_capture_header_t) == 112,
          "capture header size");
    CHECK(radar_capture_batch_align(0) == 0, "zero alignment");
    CHECK(radar_capture_batch_align(SIZE_MAX) == 0,
          "alignment overflow rejected");
    CHECK(radar_capture_batch_capture_span(SIZE_MAX) == 0,
          "capture span overflow rejected");
    CHECK(!radar_capture_batch_capacity_fits(1, SIZE_MAX),
          "oversized payload capacity rejected");
    CHECK(radar_capture_batch_commit_offset(0, 1466) == 8192,
          "observed payload commit offset follows padded header and payload");
    CHECK(radar_capture_batch_capture_span(1466) == 12288,
          "observed payload span includes header/payload/commit sectors");
    CHECK(radar_capture_batch_commit_offset(0, 8192) == 12288,
          "worst payload commit offset follows padded header and payload");
    CHECK(radar_capture_batch_capture_span(8192) == 16384,
          "worst payload span includes header/payload/commit sectors");
    CHECK(radar_capture_batch_capacity_fits(78, 8192),
          "78 worst-case captures fit slot2 data area");
    CHECK(radar_capture_batch_capacity_fits(160, 8192),
          "160 worst-case captures fit unified batch capacity");
    CHECK(!radar_capture_batch_capacity_fits(161, 8192),
          "161 captures exceed the configured batch limit");
    CHECK(RADAR_CAPTURE_SCENE_I_LOS == (1u << 8),
          "indoor LOS scene encoding is stable");
    CHECK(RADAR_CAPTURE_SCENE_O_FOLIAGE ==
              ((RADAR_CAPTURE_OBSTRUCTION_FOLIAGE << 4) |
               RADAR_CAPTURE_PROPAGATION_NLOS),
          "foliage scene encoding is stable");
    CHECK(RADAR_CAPTURE_SCENE_I_MULTIPATH ==
              ((1u << 8) | RADAR_CAPTURE_PROPAGATION_MULTIPATH),
          "multipath scene encoding is stable");
    CHECK(radar_capture_batch_capacity_fits(78, 1466),
          "78 observed-size captures fit slot2 data area");

    if (failures != 0) return 1;
    puts("capture_batch_layout: PASS");
    return 0;
}
