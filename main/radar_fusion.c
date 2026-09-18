// radar_fusion.c —— 测距/定向融合引擎实现。纯 C,无 ESP-IDF 依赖。
#include "radar_fusion.h"
#include "radar_sweep.h"  // RADAR_RSSI_MIN_VALID
#include <math.h>
#include <stdlib.h>
#include <string.h>

// RSSI 是否"存在"(区分无数据与弱信号)
#define RSSI_PRESENT_TH (-126.0f)
// FTM 滤波单轮 burst 上限
#define FTM_MAX_BURST 32
// 严格 C11 下 math.h 不保证 M_PI
#define FUSION_PI 3.14159265358979f

float radar_env_path_loss_n(radar_env_t env) {
    switch (env) {
    case RADAR_ENV_INDOOR:     return 3.0f;
    case RADAR_ENV_OBSTRUCTED: return 3.5f;
    case RADAR_ENV_OUTDOOR:
    default:                   return 2.2f;
    }
}

// ---- 双向 RSSI ----

bool radar_fusion_sym_valid(float rssi_ab, float rssi_ba) {
    return isfinite(rssi_ab) && isfinite(rssi_ba) &&
           rssi_ab > RSSI_PRESENT_TH && rssi_ab <= 20.0f &&
           rssi_ba > RSSI_PRESENT_TH && rssi_ba <= 20.0f;
}

float radar_fusion_sym_rssi(float rssi_ab, float rssi_ba) {
    if (!radar_fusion_sym_valid(rssi_ab, rssi_ba)) return RADAR_RSSI_ABSENT;
    return 0.5f * (rssi_ab + rssi_ba);
}

float radar_fusion_sym_distance(float rssi_sym, float tx_ref_sym, radar_env_t env) {
    if (!isfinite(rssi_sym) || !isfinite(tx_ref_sym) ||
        rssi_sym <= RSSI_PRESENT_TH || tx_ref_sym < -100.0f || tx_ref_sym > 20.0f) {
        return RADAR_DIST_INVALID;
    }
    float n = radar_env_path_loss_n(env);
    float distance = powf(10.0f, (tx_ref_sym - rssi_sym) / (10.0f * n));
    return isfinite(distance) && distance > 0.0f ? distance : RADAR_DIST_INVALID;
}

// ---- FTM 滤波 ----

static int cmp_float(const void *a, const void *b) {
    float fa = *(const float *)a, fb = *(const float *)b;
    return (fa > fb) - (fa < fb);
}

float radar_fusion_ftm_filter(const float *raw_m, unsigned count, float *out_spread_m) {
    if (!raw_m || count == 0) return RADAR_DIST_INVALID;

    float buf[FTM_MAX_BURST];
    unsigned n = count > FTM_MAX_BURST ? FTM_MAX_BURST : count;
    unsigned valid = 0;
    for (unsigned i = 0; i < n; ++i) {
        if (isfinite(raw_m[i]) && raw_m[i] > 0.0f && raw_m[i] <= 1000.0f) {
            buf[valid++] = raw_m[i];
        }
    }
    if (valid == 0) return RADAR_DIST_INVALID;
    n = valid;
    qsort(buf, n, sizeof(float), cmp_float);

    float median = (n & 1u) ? buf[n / 2]
                            : 0.5f * (buf[n / 2 - 1] + buf[n / 2]);
    if (out_spread_m) {
        unsigned q1 = n / 4;
        unsigned q3 = (3 * n) / 4;
        if (q3 >= n) q3 = n - 1;
        *out_spread_m = buf[q3] - buf[q1];
    }
    return median;
}

bool radar_fusion_ftm_suspicious(float ftm_m, float rssi_m) {
    return ftm_m > rssi_m * RADAR_FTM_SUSPICIOUS_RATIO &&
           ftm_m > rssi_m + RADAR_FTM_SUSPICIOUS_MARGIN_M;
}

// ---- 融合主入口 ----

radar_fusion_result_t radar_fusion_update_ftm(
    const radar_fusion_ftm_observation_t *ftm,
    float local_rssi_dbm, float peer_rssi_dbm,
    float tx_ref_sym, radar_env_t env) {
    radar_fusion_result_t r = {
        .distance_m   = RADAR_DIST_INVALID,
        .conf         = RADAR_CONF_OOR,
        .sym_rssi_dbm = RADAR_RSSI_ABSENT,
        .rssi_dist_m  = RADAR_DIST_INVALID,
        .ftm_used     = false,
    };

    bool local_ok = isfinite(local_rssi_dbm) &&
                    local_rssi_dbm > RSSI_PRESENT_TH && local_rssi_dbm <= 20.0f;
    if (!local_ok) return r;

    bool peer_ok = peer_rssi_dbm > RSSI_PRESENT_TH;
    float sym = peer_ok ? radar_fusion_sym_rssi(local_rssi_dbm, peer_rssi_dbm)
                        : local_rssi_dbm;
    r.sym_rssi_dbm = sym;

    float rssi_d = radar_fusion_sym_distance(sym, tx_ref_sym, env);
    if (rssi_d <= 0.0f || !isfinite(rssi_d)) return r;
    r.rssi_dist_m = rssi_d;

    // 弱信号守卫:对称 RSSI 低于最小有效值 → 超界(保留距离供参考)
    if (sym < RADAR_RSSI_MIN_VALID) {
        r.distance_m = rssi_d;
        r.conf = RADAR_CONF_OOR;
        return r;
    }

    // FTM 主测距:新鲜且质量足够时融合;低质量/过期结果只允许降级。
    if (ftm && ftm->valid && ftm->fresh &&
        isfinite(ftm->distance_m) && ftm->distance_m > 0.0f &&
        ftm->distance_m <= 1000.0f) {
        bool enough_bursts = ftm->valid_bursts >= 4;
        bool spread_ok = isfinite(ftm->spread_m) && ftm->spread_m >= 0.0f;
        bool spread_high = spread_ok &&
            ftm->spread_m <= fmaxf(0.5f, ftm->distance_m * 0.20f);
        bool suspicious = radar_fusion_ftm_suspicious(ftm->distance_m, rssi_d);
        bool symmetric_link = peer_ok &&
            fabsf(local_rssi_dbm - peer_rssi_dbm) <= RADAR_SYM_MAX_GAP_DB;

        if (suspicious) {
            r.distance_m = rssi_d;
            r.conf = RADAR_CONF_MED;
            r.ftm_used = false;
        } else {
            r.distance_m = RADAR_FTM_FUSION_WEIGHT * ftm->distance_m +
                           (1.0f - RADAR_FTM_FUSION_WEIGHT) * rssi_d;
            r.ftm_used = true;
            r.conf = (enough_bursts && spread_high && symmetric_link)
                         ? RADAR_CONF_HIGH : RADAR_CONF_MED;
        }
        return r;
    }

    r.distance_m = rssi_d;
    r.conf = peer_ok ? RADAR_CONF_MED : RADAR_CONF_LOW;
    return r;
}

radar_fusion_result_t radar_fusion_update(bool ftm_valid, float ftm_m,
                                          float local_rssi_dbm, float peer_rssi_dbm,
                                          float tx_ref_sym, radar_env_t env) {
    radar_fusion_ftm_observation_t ftm = {
        .valid = ftm_valid,
        .fresh = ftm_valid,
        .distance_m = ftm_m,
        .spread_m = 0.0f,
        .valid_bursts = ftm_valid ? 16u : 0u,
    };
    return radar_fusion_update_ftm(&ftm, local_rssi_dbm, peer_rssi_dbm,
                                   tx_ref_sym, env);
}

// ---- 角度工具 ----

float radar_fusion_angle_diff(float a_deg, float b_deg) {
    float d = fmodf(fabsf(a_deg - b_deg), 360.0f);
    if (d > 180.0f) d = 360.0f - d;
    return d;
}

float radar_fusion_circular_mean(const float *angles_deg, unsigned count) {
    if (!angles_deg || count == 0) return RADAR_ANGLE_INVALID;
    float sx = 0.0f, sy = 0.0f;
    for (unsigned i = 0; i < count; i++) {
        float rad = angles_deg[i] * FUSION_PI / 180.0f;
        sx += cosf(rad);
        sy += sinf(rad);
    }
    float ang = atan2f(sy, sx) * 180.0f / FUSION_PI;
    if (ang < 0.0f) ang += 360.0f;
    return ang;
}

float radar_fusion_angle_pair(float local_deg, float peer_deg) {
    bool la = local_deg >= 0.0f;
    bool pa = peer_deg >= 0.0f;
    if (!la && !pa) return RADAR_ANGLE_INVALID;
    if (!la) return peer_deg;
    if (!pa) return local_deg;
    float arr[2] = { local_deg, peer_deg };
    return radar_fusion_circular_mean(arr, 2);
}

// ---- 环境自适应估计 ----

void radar_env_est_init(radar_env_est_t *est) {
    est->min_rssi = 0.0f;
    est->max_rssi = -200.0f;
    est->count = 0;
    est->env = RADAR_ENV_OUTDOOR;
}

radar_env_t radar_env_est_update(radar_env_est_t *est, float rssi_dbm) {
    if (!est || rssi_dbm <= RSSI_PRESENT_TH) return est ? est->env : RADAR_ENV_OUTDOOR;

    if (est->count == 0 || rssi_dbm < est->min_rssi) est->min_rssi = rssi_dbm;
    if (est->count == 0 || rssi_dbm > est->max_rssi) est->max_rssi = rssi_dbm;
    est->count++;

    // 窗口满:按峰峰抖动评估环境并重置窗口(周期性重估)
    if (est->count >= RADAR_ENV_WINDOW) {
        float p2p = est->max_rssi - est->min_rssi;
        est->env = (p2p > RADAR_ENV_P2P_INDOOR_DB) ? RADAR_ENV_INDOOR
                                                   : RADAR_ENV_OUTDOOR;
        est->count = 0;
        est->min_rssi = 0.0f;
        est->max_rssi = -200.0f;
    }
    return est->env;
}

// ---- 1m 标定 ----

float radar_fusion_calib_ref(float sym_rssi_at_1m) {
    if (sym_rssi_at_1m > -30.0f) return -30.0f;
    if (sym_rssi_at_1m < -60.0f) return -60.0f;
    return sym_rssi_at_1m;
}
