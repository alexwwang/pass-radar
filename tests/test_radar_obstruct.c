#include "../main/radar_obstruct.h"
#include <math.h>
#include <stdio.h>

static int failures;
#define CHECK(condition, ...) do { \
    if (!(condition)) { fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); failures++; } \
} while (0)

static void test_features(void) {
    float values[20];
    for (unsigned i = 0; i < 20; ++i) values[i] = -60.0f;
    CHECK(isnan(radar_obstruct_kfactor_db(values, 19)), "short K window");
    CHECK(radar_obstruct_excess_loss_db(20.0f, 6.0f, 2.2f) > 11.4f &&
          radar_obstruct_excess_loss_db(20.0f, 6.0f, 2.2f) < 11.6f,
          "excess loss");
    CHECK(isnan(radar_obstruct_upper_quantile(values, 9, 0.8f)), "short quantile");
    for (unsigned i = 0; i < 20; ++i) values[i] = (float)i;
    CHECK(fabsf(radar_obstruct_upper_quantile(values, 20, 0.8f) - 15.0f) < 1.0f,
          "P80");
}

static void test_classification(void) {
    radar_obstruct_features_t f = {
        .excess_loss_db = 11.0f, .kfactor_db = 9.0f,
        .p2p_1s_db = 2.0f, .p2p_10s_db = 3.0f,
        .csi_sel_db = 3.0f, .mirror_err_deg = NAN,
        .mean_rssi_dbm = -78.0f, .level = RADAR_LEVEL_L2_NLOS,
    };
    CHECK(radar_obstruct_classify(&f) == OBS_ATTENUATING_STATIC, "wall classification");
    f.excess_loss_db = 8.0f; f.kfactor_db = 1.0f;
    f.p2p_1s_db = 8.0f; f.p2p_10s_db = 9.0f; f.csi_sel_db = 6.0f;
    CHECK(radar_obstruct_classify(&f) == OBS_FOLIAGE_DYNAMIC, "foliage classification");
    f.excess_loss_db = NAN; f.kfactor_db = 8.0f; f.p2p_1s_db = 4.0f;
    f.p2p_10s_db = 3.0f; f.csi_sel_db = 3.0f; f.mean_rssi_dbm = -86.0f;
    f.level = RADAR_LEVEL_L0_LOS;
    CHECK(radar_obstruct_classify(&f) == OBS_FAR_LOS, "far LOS classification");
    f.excess_loss_db = -6.0f;
    CHECK(radar_obstruct_classify(&f) == OBS_REFLECTION, "reflection classification");
}

static void test_apply(void) {
    radar_fusion_result_t r = {
        .distance_m = 80.0f, .dist_lo_m = 30.0f, .dist_hi_m = 120.0f,
        .conf = RADAR_CONF_LOW, .level = RADAR_LEVEL_L2_NLOS,
        .sym_rssi_dbm = -86.0f, .rssi_dist_m = 80.0f,
        .ftm_dist_m = RADAR_DIST_INVALID,
    };
    radar_obstruct_features_t f = {
        .mean_rssi_dbm = -86.0f, .level = RADAR_LEVEL_L2_NLOS,
        .excess_loss_db = NAN, .kfactor_db = 8.0f,
        .p2p_1s_db = 4.0f, .p2p_10s_db = 3.0f,
        .csi_sel_db = 3.0f, .mirror_err_deg = NAN,
    };
    float history[20];
    for (unsigned i = 0; i < 20; ++i) history[i] = -86.0f + (float)(i % 4);
    CHECK(radar_obstruct_apply(OBS_ATTENUATING_STATIC, &f, history, 20,
                               -42.0f, RADAR_ENV_OUTDOOR, &r, NULL) == HINT_WALK_TEST,
          "blocked hint");
    CHECK(r.upper_bound_only && r.dist_hi_m > r.rssi_dist_m, "blocked upper bound");

    r.conf = RADAR_CONF_MED; r.ftm_dist_m = 6.0f;
    r.rssi_dist_m = 20.0f;
    CHECK(radar_obstruct_apply(OBS_ATTENUATING_STATIC, &f, history, 20,
                               -42.0f, RADAR_ENV_OUTDOOR, &r, NULL) == HINT_RAISE_CARD,
          "FTM wall hint");
    CHECK(fabsf(r.distance_m - 6.0f) < 0.01f && r.obstructed, "FTM wall distance");

    float spread = 12.0f;
    CHECK(radar_obstruct_apply(OBS_REFLECTION, &f, history, 20, -42.0f,
                               RADAR_ENV_OUTDOOR, &r, &spread) == HINT_SWEEP_AGAIN,
          "reflection hint");
    CHECK(fabsf(spread - 90.0f) < 0.01f, "reflection spread");
}

static void test_v3_fusion(void) {
    radar_fusion_result_t r = radar_fusion_update_v3(NULL, -90.0f, -90.0f,
                                                       -42.0f, RADAR_LEVEL_L2_NLOS, true);
    CHECK(r.conf == RADAR_CONF_LOW && r.upper_bound_only && r.distance_m > 0.0f,
          "weak alive link must not be OOR");
    r = radar_fusion_update_v3(NULL, -60.0f, -66.0f, -42.0f,
                               RADAR_LEVEL_L0_LOS, false);
    CHECK(r.conf == RADAR_CONF_OOR, "dead peer OOR");

    radar_fusion_ftm_observation_t ftm = {
        .valid = true, .fresh = true, .calibrated = true,
        .distance_m = 6.0f, .spread_m = 0.4f, .valid_bursts = 8,
    };
    r = radar_fusion_update_v3(&ftm, -60.0f, -66.0f, -42.0f,
                               RADAR_LEVEL_L0_LOS, true);
    CHECK(r.ftm_used && r.conf == RADAR_CONF_HIGH && fabsf(r.distance_m - 6.0f) < 0.01f,
          "calibrated FTM");
}

int main(void) {
    test_features();
    test_classification();
    test_apply();
    test_v3_fusion();
    if (failures) return 1;
    puts("test_radar_obstruct: PASS");
    return 0;
}
