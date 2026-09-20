// radar_sweep.c —— 扫盘峰值检测与距离估算。纯 C,无 ESP-IDF 依赖。
#include "radar_sweep.h"
#include "radar_fusion.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

// 单信道分组采样上限(快跳下每信道约 27 个样本,64 充裕)
#define SWEEP_GROUP_CAP 64
// 信道分组检测的最小样本数
#define SWEEP_GROUP_MIN 6
#define SWEEP_MAX_RMS_RESIDUAL_DB 5.0f
// 严格 C11 下 math.h 不保证 M_PI
#define SWEEP_PI 3.14159265358979f

float radar_sweep_calc_distance(float rssi) {
    // 自由空间路径损耗:distance = 10^((txPower - rssi) / (10 * n))
    return powf(10.0f, (RADAR_TX_POWER_REF - rssi) / (10.0f * RADAR_PATH_LOSS_N));
}

int radar_sweep_peak_angle(const radar_sample_t *samples, unsigned count,
                           uint32_t sweep_duration_ms) {
    if (count == 0 || sweep_duration_ms == 0) return -1;

    float max_rssi = -999.0f;
    uint32_t peak_time = 0;
    unsigned i;

    for (i = 0; i < count; i++) {
        if (samples[i].rssi > max_rssi) {
            max_rssi = samples[i].rssi;
            peak_time = samples[i].timestamp_ms;
        }
    }

    float ratio = (float)peak_time / (float)sweep_duration_ms;
    if (ratio > 1.0f) ratio = 1.0f;

    return (int)(ratio * 360.0f);
}

float radar_sweep_peak_rssi(const radar_sample_t *samples, unsigned count) {
    if (count == 0) return -999.0f;

    float max_rssi = samples[0].rssi;
    for (unsigned i = 1; i < count; i++) {
        if (samples[i].rssi > max_rssi) max_rssi = samples[i].rssi;
    }
    return max_rssi;
}

// ---- v2 增强 ----

float radar_sweep_peak_angle_interp(const radar_sample_t *samples, unsigned count,
                                    uint32_t sweep_duration_ms) {
    if (!samples || count == 0 || sweep_duration_ms == 0) return -1.0f;

    unsigned mi = 0;
    for (unsigned i = 1; i < count; i++) {
        if (samples[i].rssi > samples[mi].rssi) mi = i;
    }

    float t = (float)samples[mi].timestamp_ms;

    // 抛物线插值只允许时间顺序有效且三点属于同一信道。
    if (mi > 0 && mi < count - 1 &&
        samples[mi - 1].timestamp_ms < samples[mi].timestamp_ms &&
        samples[mi].timestamp_ms < samples[mi + 1].timestamp_ms &&
        samples[mi - 1].channel == samples[mi].channel &&
        samples[mi].channel == samples[mi + 1].channel) {
        float y0 = samples[mi - 1].rssi;
        float y1 = samples[mi].rssi;
        float y2 = samples[mi + 1].rssi;
        float denom = y0 - 2.0f * y1 + y2;
        if (denom < 0.0f) {
            float delta = 0.5f * (y0 - y2) / denom;
            if (delta >= -0.5f && delta <= 0.5f) {
                float dt = 0.5f * ((float)samples[mi + 1].timestamp_ms -
                                   (float)samples[mi - 1].timestamp_ms);
                t += delta * dt;
            }
        }
    }

    float ratio = t / (float)sweep_duration_ms;
    if (ratio < 0.0f) ratio = 0.0f;
    if (ratio > 1.0f) ratio = 1.0f;
    return ratio * 360.0f;
}

static int cmp_float(const void *a, const void *b) {
    float fa = *(const float *)a, fb = *(const float *)b;
    return (fa > fb) - (fa < fb);
}

float radar_sweep_peak_prominence(const radar_sample_t *samples, unsigned count) {
    if (!samples || count == 0) return 0.0f;

    float buf[RADAR_MAX_SAMPLES_CAP];
    unsigned n = count > RADAR_MAX_SAMPLES_CAP ? RADAR_MAX_SAMPLES_CAP : count;
    for (unsigned i = 0; i < n; i++) buf[i] = samples[i].rssi;
    qsort(buf, n, sizeof(float), cmp_float);

    float baseline = buf[n / 10];  // P10 分位
    float peak = buf[n - 1];
    return peak - baseline;
}

uint8_t radar_sweep_channel_at(uint32_t t_ms, uint32_t sweep_duration_ms) {
    (void)sweep_duration_ms;
    // 快跳轮询:每 RADAR_SWEEP_HOP_DWELL_MS 换一个信道,1/6/11 循环。
    // 每个信道都能覆盖完整 360° 旋转,从而获得独立的方向图用于投票。
    uint32_t slot = t_ms / RADAR_SWEEP_HOP_DWELL_MS;
    switch (slot % 3u) {
    case 0:  return RADAR_SWEEP_CH_A;
    case 1:  return RADAR_SWEEP_CH_B;
    default: return RADAR_SWEEP_CH_C;
    }
}

// ---- 一次谐波最小二乘拟合 ----

// 3×3 线性方程组求解(高斯消元 + 部分主元)。退化返回 false。
static bool solve3(float a[3][3], float b[3], float x[3]) {
    for (unsigned col = 0; col < 3; col++) {
        unsigned piv = col;
        for (unsigned row = col + 1; row < 3; row++) {
            if (fabsf(a[row][col]) > fabsf(a[piv][col])) piv = row;
        }
        if (fabsf(a[piv][col]) < 1e-6f) return false;
        if (piv != col) {
            for (unsigned k = col; k < 3; k++) {
                float tmp = a[col][k]; a[col][k] = a[piv][k]; a[piv][k] = tmp;
            }
            float tmp = b[col]; b[col] = b[piv]; b[piv] = tmp;
        }
        for (unsigned row = col + 1; row < 3; row++) {
            float f = a[row][col] / a[col][col];
            for (unsigned k = col; k < 3; k++) a[row][k] -= f * a[col][k];
            b[row] -= f * b[col];
        }
    }
    for (int row = 2; row >= 0; row--) {
        float s = b[row];
        for (unsigned k = (unsigned)row + 1; k < 3; k++) s -= a[row][k] * x[k];
        x[row] = s / a[row][row];
    }
    return true;
}

float radar_sweep_harmonic_angle_ex(const radar_sample_t *samples, unsigned count,
                                    uint32_t sweep_duration_ms,
                                    float *out_amplitude_db,
                                    float *out_residual_db) {
    float amp = 0.0f, res = INFINITY;
    float ang = radar_sweep_harmonic_fit(samples, count, sweep_duration_ms,
                                         &amp, &res);
    if (ang < 0.0f) return -1.0f;
    // 质量门限:拟合残差过大说明方向图非一次谐波(多径/噪声),拒绝该结果。
    if (!isfinite(res) || res > SWEEP_MAX_RMS_RESIDUAL_DB) return -1.0f;
    if (out_amplitude_db) *out_amplitude_db = amp;
    if (out_residual_db) *out_residual_db = res;
    return ang;
}

float radar_sweep_harmonic_fit(const radar_sample_t *samples, unsigned count,
                               uint32_t sweep_duration_ms,
                               float *out_amplitude_db, float *out_residual_db) {
    if (out_amplitude_db) *out_amplitude_db = 0.0f;
    if (out_residual_db) *out_residual_db = INFINITY;
    if (!samples || count < SWEEP_GROUP_MIN || sweep_duration_ms == 0) return -1.0f;

    // 法方程:y = a + b·cosθ + c·sinθ,θ = 2π·t/duration
    float s_c = 0.0f, s_s = 0.0f;             // Σcos, Σsin
    float s_cc = 0.0f, s_ss = 0.0f, s_cs = 0.0f;
    float s_y = 0.0f, s_yc = 0.0f, s_ys = 0.0f;
    for (unsigned i = 0; i < count; i++) {
        float th = 2.0f * SWEEP_PI * (float)samples[i].timestamp_ms /
                   (float)sweep_duration_ms;
        float c = cosf(th), sn = sinf(th), y = samples[i].rssi;
        s_c += c;       s_s += sn;
        s_cc += c * c;  s_ss += sn * sn;  s_cs += c * sn;
        s_y += y;       s_yc += y * c;    s_ys += y * sn;
    }

    float n = (float)count;
    float a[3][3] = {
        { n,    s_c,  s_s  },
        { s_c,  s_cc, s_cs },
        { s_s,  s_cs, s_ss },
    };
    float b[3] = { s_y, s_yc, s_ys };
    float x[3];
    if (!solve3(a, b, x)) return -1.0f;

    float amp = sqrtf(x[1] * x[1] + x[2] * x[2]);
    if (!isfinite(amp)) return -1.0f;
    if (out_amplitude_db) *out_amplitude_db = amp;

    float residual_sum = 0.0f;
    for (unsigned i = 0; i < count; ++i) {
        float th = 2.0f * SWEEP_PI * (float)samples[i].timestamp_ms /
                   (float)sweep_duration_ms;
        float predicted = x[0] + x[1] * cosf(th) + x[2] * sinf(th);
        float residual = samples[i].rssi - predicted;
        residual_sum += residual * residual;
    }
    float rms = sqrtf(residual_sum / (float)count);
    if (out_residual_db) *out_residual_db = rms;
    if (!isfinite(amp)) return -1.0f;

    float ang = atan2f(x[2], x[1]) * 180.0f / SWEEP_PI;
    if (ang < 0.0f) ang += 360.0f;
    if (ang >= 360.0f) ang -= 360.0f;
    return ang;
}

float radar_sweep_harmonic_angle(const radar_sample_t *samples, unsigned count,
                                 uint32_t sweep_duration_ms, float *out_amplitude_db) {
    return radar_sweep_harmonic_angle_ex(samples, count, sweep_duration_ms,
                                         out_amplitude_db, NULL);
}

unsigned radar_sweep_channel_angles(const radar_sample_t *samples, unsigned count,
                                    uint32_t sweep_duration_ms,
                                    float *out_angles, unsigned max_out) {
    if (!samples || !out_angles || count == 0 ||
        sweep_duration_ms == 0 || max_out == 0) return 0;

    // 收集出现过的信道值(最多 4 种,含 0=未标记)
    uint8_t chans[4];
    unsigned nch = 0;
    for (unsigned i = 0; i < count && nch < 4; i++) {
        uint8_t ch = samples[i].channel;
        bool seen = false;
        for (unsigned j = 0; j < nch; j++) {
            if (chans[j] == ch) { seen = true; break; }
        }
        if (!seen) chans[nch++] = ch;
    }

    unsigned n = 0;
    for (unsigned c = 0; c < nch && n < max_out; c++) {
        radar_sample_t sub[SWEEP_GROUP_CAP];
        unsigned m = 0;
        for (unsigned i = 0; i < count && m < SWEEP_GROUP_CAP; i++) {
            if (samples[i].channel == chans[c]) sub[m++] = samples[i];
        }
        // 谐波拟合测角;幅度或残差不达标时拒绝该信道投票。
        float amp = 0.0f;
        float residual = INFINITY;
        float ang = radar_sweep_harmonic_angle_ex(sub, m, sweep_duration_ms,
                                                   &amp, &residual);
        if (ang < 0.0f || !isfinite(residual)) continue;
        if (2.0f * amp < RADAR_SWEEP_MIN_PROMINENCE_DB) continue;
        out_angles[n++] = ang;
    }
    return n;
}

float radar_sweep_angle_vote(const float *angles, unsigned count, float *out_spread_deg) {
    if (out_spread_deg) *out_spread_deg = 0.0f;
    if (!angles || count == 0) return -1.0f;
    for (unsigned i = 0; i < count; ++i) {
        if (!isfinite(angles[i])) return -1.0f;
    }
    if (count == 1) {
        float angle = fmodf(angles[0], 360.0f);
        if (angle < 0.0f) angle += 360.0f;
        return angle;
    }

    // 环形中值:与其他角度环形差之和最小者获胜
    unsigned best = 0;
    float best_cost = 1e9f;
    for (unsigned i = 0; i < count; i++) {
        float cost = 0.0f;
        for (unsigned j = 0; j < count; j++) {
            cost += radar_fusion_angle_diff(angles[i], angles[j]);
        }
        if (cost < best_cost) {
            best_cost = cost;
            best = i;
        }
    }

    if (out_spread_deg) {
        float spread = 0.0f;
        for (unsigned j = 0; j < count; j++) {
            float d = radar_fusion_angle_diff(angles[best], angles[j]);
            if (d > spread) spread = d;
        }
        *out_spread_deg = spread;
    }
    float voted = fmodf(angles[best], 360.0f);
    if (voted < 0.0f) voted += 360.0f;
    return voted;
}

// ---- 旋转速度自校正(调研 5.4 P1) ----

// 周期搜索范围与接受门限
#define SWEEP_PERIOD_MIN_MS      2000u
#define SWEEP_PERIOD_STEP_MS     100u
#define SWEEP_PERIOD_REFINE_MS   20u
#define SWEEP_PERIOD_RESIDUAL_GAIN 0.7f   // 残差需比当前最优低 30% 以上

// 样本前缀中时间 ≤ t_ms 的个数(样本已按时间排序)
static unsigned prefix_count(const radar_sample_t *samples, unsigned count,
                             uint32_t t_ms) {
    unsigned m = 0;
    while (m < count && samples[m].timestamp_ms <= t_ms) m++;
    return m;
}

uint32_t radar_sweep_estimate_period_ms(const radar_sample_t *samples, unsigned count,
                                        uint32_t sweep_duration_ms) {
    if (!samples || count < 24 ||
        sweep_duration_ms < SWEEP_PERIOD_MIN_MS + 2u * SWEEP_PERIOD_STEP_MS)
        return sweep_duration_ms;

    // 名义拟合基线:幅度不足或拟合失败则不校正(交给上层回退路径)。
    // 注意谐波幅度是峰-基线的一半,与信道门 2·amp 保持同一约定。
    float amp0 = 0.0f, res0 = INFINITY;
    if (radar_sweep_harmonic_fit(samples, count, sweep_duration_ms,
                                 &amp0, &res0) < 0.0f ||
        2.0f * amp0 < RADAR_SWEEP_MIN_PROMINENCE_DB) {
        return sweep_duration_ms;
    }

    // 粗搜索:候选周期 T ∈ [2.0s, duration-100ms],截断到 t ≤ T 后重新拟合,
    // 残差显著优于当前最优者胜出。提前停转时,尾部样本(恒定姿态,无方向
    // 信息)会污染名义拟合;正确 T 下一次谐波拟合残差接近纯噪声水平。
    uint32_t best_t = sweep_duration_ms;
    float best_res = res0;
    for (uint32_t t = sweep_duration_ms - SWEEP_PERIOD_STEP_MS;
         t >= SWEEP_PERIOD_MIN_MS; t -= SWEEP_PERIOD_STEP_MS) {
        unsigned m = prefix_count(samples, count, t);
        if (m < 24) break;
        float amp = 0.0f, res = INFINITY;
        if (radar_sweep_harmonic_fit(samples, m, t, &amp, &res) < 0.0f) continue;
        if (2.0f * amp < RADAR_SWEEP_MIN_PROMINENCE_DB) continue;
        if (res < best_res * SWEEP_PERIOD_RESIDUAL_GAIN) {
            best_res = res;
            best_t = t;
        }
    }
    if (best_t == sweep_duration_ms) return sweep_duration_ms;

    // 细化:在最优 T 附近 plus/minus 步长内以 20ms 网格重搜(角度粒度 ≤ ~2.5°)。
    uint32_t lo = best_t > SWEEP_PERIOD_STEP_MS
                      ? best_t - SWEEP_PERIOD_STEP_MS : best_t;
    uint32_t hi = best_t + SWEEP_PERIOD_STEP_MS;
    if (hi >= sweep_duration_ms) hi = sweep_duration_ms - 1;
    for (uint32_t t = lo; t <= hi; t += SWEEP_PERIOD_REFINE_MS) {
        unsigned m = prefix_count(samples, count, t);
        if (m < 24) break;
        float amp = 0.0f, res = INFINITY;
        if (radar_sweep_harmonic_fit(samples, m, t, &amp, &res) < 0.0f) continue;
        if (2.0f * amp < RADAR_SWEEP_MIN_PROMINENCE_DB) continue;
        if (res < best_res) {
            best_res = res;
            best_t = t;
        }
    }
    return best_t;
}

unsigned radar_sweep_effective_duration_ms(const radar_sample_t *samples, unsigned count,
                                           uint32_t sweep_duration_ms,
                                           uint32_t *out_duration_ms) {
    if (!out_duration_ms) return count;
    *out_duration_ms = sweep_duration_ms;
    if (!samples || count == 0) return count;

    uint32_t t = radar_sweep_estimate_period_ms(samples, count, sweep_duration_ms);
    if (t >= sweep_duration_ms) return count;

    unsigned m = prefix_count(samples, count, t);
    if (m < 12) return count;   // 截断后样本不足,放弃修正
    *out_duration_ms = t;
    return m;
}
