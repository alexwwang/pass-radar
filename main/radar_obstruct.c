#include "radar_obstruct.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define OBS_EXCESS_MIN_DB       6.0f
#define OBS_REFLECT_EXCESS_DB  (-3.0f)
#define OBS_K_FOLIAGE_DB        2.0f
#define OBS_K_STATIC_DB         4.0f
#define OBS_P2P1S_DYNAMIC_DB    8.0f
#define OBS_P2P1S_STATIC_DB     5.0f
#define OBS_P2P10S_DYNAMIC_DB   6.0f
#define OBS_SEL_LOS_MAX_DB      5.0f
#define OBS_MIRROR_REFLECT_DEG 45.0f
#define OBS_FAR_RSSI_DBM      (-80.0f)
#define OBS_RAISE_HINT_RSSI   (-75.0f)
#define OBS_UPPER_QUANTILE     0.8f
#define OBS_FOLIAGE_INTERVAL_E 0.5f
#define OBS_BLOCKED_INTERVAL_E 0.6f
#define OBS_FTM_BAND_M          1.5f

static int cmp_float(const void *a, const void *b) {
    float left = *(const float *)a;
    float right = *(const float *)b;
    return (left > right) - (left < right);
}

float radar_obstruct_kfactor_db(const float *x, unsigned n) {
    if (!x || n < 20) return NAN;
    double mean = 0.0;
    double second = 0.0;
    for (unsigned i = 0; i < n; ++i) {
        if (!isfinite(x[i])) return NAN;
        double power = pow(10.0, x[i] / 10.0);
        mean += power;
        second += power * power;
    }
    mean /= (double)n;
    second /= (double)n;
    if (!(mean > 0.0)) return NAN;
    double gamma = (second - mean * mean) / (mean * mean);
    if (gamma >= 1.0) return -10.0f;
    if (gamma < 0.0) gamma = 0.0;
    double s = sqrt(1.0 - gamma);
    double k = s / (1.0 - s + 1e-9);
    return (float)(10.0 * log10(k + 1e-9));
}

float radar_obstruct_excess_loss_db(float d_rssi, float d_ftm, float n) {
    if (!(d_rssi > 0.0f) || !(d_ftm > 0.0f) || !(n > 0.0f) ||
        !isfinite(d_rssi) || !isfinite(d_ftm) || !isfinite(n)) return NAN;
    return 10.0f * n * log10f(d_rssi / d_ftm);
}

float radar_obstruct_upper_quantile(const float *values, unsigned n, float q) {
    if (!values || n < 10 || !(q > 0.0f && q < 1.0f)) return NAN;
    float copy[256];
    if (n > 256) n = 256;
    for (unsigned i = 0; i < n; ++i) {
        if (!isfinite(values[i])) return NAN;
        copy[i] = values[i];
    }
    qsort(copy, n, sizeof(copy[0]), cmp_float);
    unsigned index = (unsigned)(q * (float)(n - 1) + 0.5f);
    return copy[index];
}

void radar_obstruct_p2p(const float *x, const uint32_t *timestamps,
                        unsigned n, uint32_t now, float *p1, float *p10) {
    float min_1s = INFINITY, max_1s = -INFINITY;
    unsigned count_1s = 0;
    float sums[10] = {0};
    unsigned counts[10] = {0};
    if (!x || !timestamps) {
        if (p1) *p1 = NAN;
        if (p10) *p10 = NAN;
        return;
    }
    for (unsigned i = 0; i < n; ++i) {
        uint32_t age = now - timestamps[i];
        if (age < 1000) {
            if (x[i] < min_1s) min_1s = x[i];
            if (x[i] > max_1s) max_1s = x[i];
            count_1s++;
        }
        if (age < 10000) {
            unsigned bucket = age / 1000;
            if (bucket < 10) {
                sums[bucket] += x[i];
                counts[bucket]++;
            }
        }
    }
    if (p1) *p1 = count_1s >= 5 ? max_1s - min_1s : NAN;

    float min_bucket = INFINITY, max_bucket = -INFINITY;
    unsigned bucket_count = 0;
    for (unsigned i = 0; i < 10; ++i) {
        if (counts[i] < 3) continue;
        float mean = sums[i] / (float)counts[i];
        if (mean < min_bucket) min_bucket = mean;
        if (mean > max_bucket) max_bucket = mean;
        bucket_count++;
    }
    if (p10) *p10 = bucket_count >= 5 ? max_bucket - min_bucket : NAN;
}

float radar_obstruct_mean_recent(const float *x, const uint32_t *timestamps,
                                 unsigned n, uint32_t now, uint32_t window_ms) {
    if (!x || !timestamps) return NAN;
    double sum = 0.0;
    unsigned count = 0;
    for (unsigned i = 0; i < n; ++i) {
        if (now - timestamps[i] < window_ms && isfinite(x[i])) {
            sum += x[i];
            count++;
        }
    }
    return count ? (float)(sum / (double)count) : NAN;
}

radar_obstruct_class_t radar_obstruct_classify(const radar_obstruct_features_t *f) {
    if (!f) return OBS_UNKNOWN;
    bool has_ex = isfinite(f->excess_loss_db);
    bool has_k = isfinite(f->kfactor_db);
    bool has_p1 = isfinite(f->p2p_1s_db);
    bool has_p10 = isfinite(f->p2p_10s_db);
    bool has_sel = isfinite(f->csi_sel_db);
    bool has_mirror = isfinite(f->mirror_err_deg);

    if ((has_ex && f->excess_loss_db < OBS_REFLECT_EXCESS_DB) ||
        (has_mirror && f->mirror_err_deg > OBS_MIRROR_REFLECT_DEG)) {
        return OBS_REFLECTION;
    }
    if (has_p1 && f->p2p_1s_db >= OBS_P2P1S_DYNAMIC_DB &&
        ((has_k && f->kfactor_db < OBS_K_FOLIAGE_DB) ||
         (has_p10 && f->p2p_10s_db >= OBS_P2P10S_DYNAMIC_DB))) {
        return OBS_FOLIAGE_DYNAMIC;
    }
    if (has_ex && f->excess_loss_db > OBS_EXCESS_MIN_DB && has_p1 &&
        f->p2p_1s_db < OBS_P2P1S_STATIC_DB) {
        return OBS_ATTENUATING_STATIC;
    }
    if (!has_ex && f->level == RADAR_LEVEL_L2_NLOS && has_p1 &&
        f->p2p_1s_db < OBS_P2P1S_STATIC_DB && has_k &&
        f->kfactor_db > OBS_K_STATIC_DB) {
        return OBS_ATTENUATING_STATIC;
    }
    if (f->mean_rssi_dbm < OBS_FAR_RSSI_DBM &&
        (!has_p1 || f->p2p_1s_db < OBS_P2P1S_DYNAMIC_DB) &&
        (!has_k || f->kfactor_db > OBS_K_STATIC_DB) &&
        (!has_sel || f->csi_sel_db < OBS_SEL_LOS_MAX_DB)) {
        return OBS_FAR_LOS;
    }
    if (f->level == RADAR_LEVEL_L0_LOS) return OBS_NONE_LOS;
    return OBS_UNKNOWN;
}

void radar_obstruct_hold_init(radar_obstruct_hold_t *hold) {
    if (!hold) return;
    hold->current = OBS_NONE_LOS;
    hold->pending = OBS_NONE_LOS;
    hold->pending_count = 0;
}

radar_obstruct_class_t radar_obstruct_hold_update(radar_obstruct_hold_t *hold,
                                                   radar_obstruct_class_t observed,
                                                   unsigned hold_ticks) {
    if (!hold || hold_ticks == 0) return hold ? hold->current : OBS_UNKNOWN;
    if (observed == hold->current) {
        hold->pending_count = 0;
        return hold->current;
    }
    if (observed != hold->pending) {
        hold->pending = observed;
        hold->pending_count = 1;
    } else {
        hold->pending_count++;
    }
    if (hold->pending_count >= hold_ticks) {
        hold->current = observed;
        hold->pending_count = 0;
    }
    return hold->current;
}

radar_hint_t radar_obstruct_apply(radar_obstruct_class_t cls,
                                  const radar_obstruct_features_t *f,
                                  const float *history, unsigned history_count,
                                  float tx_ref, radar_env_t env,
                                  radar_fusion_result_t *r,
                                  float *spread) {
    if (!f || !r || r->conf == RADAR_CONF_OOR) return HINT_NONE;
    bool ftm_ok = r->ftm_dist_m > 0.0f && isfinite(r->ftm_dist_m);
    bool weak = f->mean_rssi_dbm < OBS_RAISE_HINT_RSSI;

    switch (cls) {
    case OBS_FOLIAGE_DYNAMIC: {
        float p80 = radar_obstruct_upper_quantile(history, history_count,
                                                  OBS_UPPER_QUANTILE);
        double sum = 0.0;
        bool valid = history && history_count >= 10;
        if (valid) {
            for (unsigned i = 0; i < history_count; ++i) {
                if (!isfinite(history[i])) { valid = false; break; }
                sum += history[i];
            }
        }
        if (!ftm_ok && valid && isfinite(p80)) {
            float delta = p80 - (float)(sum / (double)history_count);
            if (delta < 0.0f) delta = 0.0f;
            r->distance_m = radar_fusion_sym_distance(r->sym_rssi_dbm + delta,
                                                       tx_ref, env);
        }
        r->dist_lo_m = r->distance_m / (1.0f + OBS_FOLIAGE_INTERVAL_E);
        r->dist_hi_m = r->distance_m * (1.0f + OBS_FOLIAGE_INTERVAL_E);
        r->obstructed = true;
        if (r->conf > RADAR_CONF_MED) r->conf = RADAR_CONF_MED;
        return weak ? HINT_RAISE_CARD : HINT_NONE;
    }
    case OBS_ATTENUATING_STATIC:
        r->obstructed = true;
        if (ftm_ok) {
            r->distance_m = r->ftm_dist_m;
            r->dist_lo_m = fmaxf(0.1f, r->ftm_dist_m - OBS_FTM_BAND_M);
            r->dist_hi_m = r->ftm_dist_m + OBS_FTM_BAND_M;
            r->upper_bound_only = false;
            if (r->conf > RADAR_CONF_MED) r->conf = RADAR_CONF_MED;
            return weak ? HINT_RAISE_CARD : HINT_NONE;
        }
        r->dist_lo_m = 0.0f;
        r->dist_hi_m = r->rssi_dist_m * 1.6f;
        r->distance_m = r->dist_hi_m;
        r->upper_bound_only = true;
        r->conf = RADAR_CONF_LOW;
        return f->level == RADAR_LEVEL_L2_NLOS ? HINT_WALK_TEST : HINT_RAISE_CARD;
    case OBS_REFLECTION:
        if (spread) *spread = 90.0f;
        r->distance_m = ftm_ok && r->ftm_dist_m < r->rssi_dist_m
                           ? r->ftm_dist_m : r->rssi_dist_m;
        r->dist_lo_m = r->distance_m * 0.5f;
        r->dist_hi_m = r->distance_m * 2.0f;
        r->obstructed = true;
        r->conf = RADAR_CONF_LOW;
        return HINT_SWEEP_AGAIN;
    case OBS_FAR_LOS:
        return weak ? HINT_RAISE_CARD : HINT_NONE;
    default:
        return HINT_NONE;
    }
}

const char *radar_obstruct_class_label(radar_obstruct_class_t cls) {
    switch (cls) {
    case OBS_FAR_LOS: return "FAR";
    case OBS_ATTENUATING_STATIC: return "BLOCKED?";
    case OBS_FOLIAGE_DYNAMIC: return "FOLIAGE?";
    case OBS_REFLECTION: return "REFLECT?";
    default: return "";
    }
}

const char *radar_hint_label(radar_hint_t hint) {
    switch (hint) {
    case HINT_RAISE_CARD: return "RAISE CARD 3s";
    case HINT_WALK_TEST: return "WALK 5m THEN DOWN";
    case HINT_PEER_STAND_STILL: return "PEER: HOLD STILL";
    case HINT_SWEEP_AGAIN: return "SWEEP AGAIN";
    default: return "";
    }
}
