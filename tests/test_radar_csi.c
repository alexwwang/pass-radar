#include "../main/radar_csi.h"
#include <math.h>
#include <stdio.h>

#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FAIL: " __VA_ARGS__); return 1; } } while (0)

int main(void) {
    radar_csi_history_t h;
    radar_csi_history_init(&h);
    CHECK(isnan(radar_csi_selectivity_db(&h)), "empty CSI should be NaN\n");

    float flat[16];
    for (unsigned i = 0; i < 16; ++i) flat[i] = 1.0f;
    CHECK(radar_csi_history_push(&h, flat, 16), "flat frame push\n");
    CHECK(fabsf(radar_csi_selectivity_db(&h)) < 0.001f, "flat selectivity\n");

    float selective[16];
    for (unsigned i = 0; i < 16; ++i) selective[i] = i < 8 ? 1.0f : 4.0f;
    CHECK(radar_csi_history_push(&h, selective, 16), "selective frame push\n");
    CHECK(radar_csi_selectivity_db(&h) > 10.0f, "selective frame feature\n");
    CHECK(!radar_csi_history_push(&h, selective, 4), "short frame rejected\n");
    puts("test_radar_csi: PASS");
    return 0;
}
