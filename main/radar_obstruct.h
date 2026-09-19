#pragma once

#include "radar_fusion.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    OBS_NONE_LOS = 0,
    OBS_FAR_LOS,
    OBS_ATTENUATING_STATIC,
    OBS_FOLIAGE_DYNAMIC,
    OBS_REFLECTION,
    OBS_UNKNOWN,
} radar_obstruct_class_t;

typedef enum {
    HINT_NONE = 0,
    HINT_RAISE_CARD,
    HINT_WALK_TEST,
    HINT_PEER_STAND_STILL,
    HINT_SWEEP_AGAIN,
} radar_hint_t;

typedef struct {
    float excess_loss_db;
    float kfactor_db;
    float p2p_1s_db;
    float p2p_10s_db;
    float csi_sel_db;
    float mirror_err_deg;
    float mean_rssi_dbm;
    radar_level_t level;
} radar_obstruct_features_t;

float radar_obstruct_kfactor_db(const float *rssi_dbm, unsigned n);
float radar_obstruct_excess_loss_db(float d_rssi_m, float d_ftm_m, float path_loss_n);
float radar_obstruct_upper_quantile(const float *values, unsigned n, float q);
void radar_obstruct_p2p(const float *rssi_dbm, const uint32_t *timestamps_ms,
                        unsigned n, uint32_t now_ms,
                        float *out_p2p_1s_db, float *out_p2p_10s_db);
float radar_obstruct_mean_recent(const float *rssi_dbm, const uint32_t *timestamps_ms,
                                 unsigned n, uint32_t now_ms, uint32_t window_ms);

radar_obstruct_class_t radar_obstruct_classify(const radar_obstruct_features_t *features);

typedef struct {
    radar_obstruct_class_t current;
    radar_obstruct_class_t pending;
    unsigned pending_count;
} radar_obstruct_hold_t;

void radar_obstruct_hold_init(radar_obstruct_hold_t *hold);
radar_obstruct_class_t radar_obstruct_hold_update(radar_obstruct_hold_t *hold,
                                                   radar_obstruct_class_t observed,
                                                   unsigned hold_ticks);

radar_hint_t radar_obstruct_apply(radar_obstruct_class_t cls,
                                  const radar_obstruct_features_t *features,
                                  const float *rssi_history, unsigned history_count,
                                  float tx_ref, radar_env_t env,
                                  radar_fusion_result_t *result,
                                  float *sweep_spread_deg);

const char *radar_obstruct_class_label(radar_obstruct_class_t cls);
const char *radar_hint_label(radar_hint_t hint);
