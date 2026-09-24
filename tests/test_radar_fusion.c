// tests/test_radar_fusion.c —— 测距/定向融合引擎 host test。
// 覆盖:双向 RSSI 对称、FTM 中值滤波、反射径判别、距离融合规则、
//       置信度分级、环形角度工具、环境自适应、1m 标定。
#include "../main/radar_fusion.h"
#include <math.h>
#include <stdio.h>

static int s_failures = 0;

#define CHECK(cond, ...)                                   \
    do {                                                   \
        if (!(cond)) {                                     \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                  \
            fprintf(stderr, "\n");                         \
            s_failures++;                                  \
        }                                                  \
    } while (0)

static void test_sym_rssi(void) {
    CHECK(!radar_fusion_sym_valid(RADAR_RSSI_ABSENT, -60.0f), "sym_valid: absent peer should be invalid");
    CHECK(!radar_fusion_sym_valid(-60.0f, RADAR_RSSI_ABSENT), "sym_valid: absent local should be invalid");
    CHECK(radar_fusion_sym_valid(-60.0f, -70.0f), "sym_valid: both present should be valid");

    float sym = radar_fusion_sym_rssi(-60.0f, -70.0f);
    CHECK(fabsf(sym - (-65.0f)) < 0.01f, "sym_rssi: got %.2f, expected -65.00", sym);

    // 距离:对称参考 -42dBm@1m,室外 n=2.2
    float d1 = radar_fusion_sym_distance(-42.0f, -42.0f, RADAR_ENV_OUTDOOR);
    CHECK(fabsf(d1 - 1.0f) < 0.05f, "sym_distance at ref: %.2f, expected ~1.0", d1);
    float d2 = radar_fusion_sym_distance(-64.0f, -42.0f, RADAR_ENV_OUTDOOR);
    CHECK(d2 > 8.0f && d2 < 12.0f, "sym_distance -64dBm: %.2f, expected ~10", d2);

    // 室内 n 更大 → 同样 RSSI 差对应更近距离
    float d3 = radar_fusion_sym_distance(-64.0f, -42.0f, RADAR_ENV_INDOOR);
    CHECK(d3 < d2, "indoor n should give shorter distance (%.2f vs %.2f)", d3, d2);

    CHECK(fabsf(radar_env_path_loss_n(RADAR_ENV_OUTDOOR) - 2.2f) < 0.01f, "n outdoor");
    CHECK(fabsf(radar_env_path_loss_n(RADAR_ENV_INDOOR) - 3.0f) < 0.01f, "n indoor");
    CHECK(fabsf(radar_env_path_loss_n(RADAR_ENV_OBSTRUCTED) - 3.5f) < 0.01f, "n obstructed");

    printf("  sym_rssi: PASS\n");
}

static void test_ftm_filter(void) {
    // 含一个反射径离群值(12m)的 burst:中值应稳健
    float raw[5] = {2.0f, 2.1f, 1.9f, 12.0f, 2.05f};
    float spread = -1.0f;
    float d = radar_fusion_ftm_filter(raw, 5, &spread);
    CHECK(d > 1.9f && d < 2.2f, "ftm_filter median: %.2f, expected ~2.05", d);
    CHECK(spread >= 0.0f && spread < 0.5f, "ftm_filter spread: %.2f, expected small", spread);

    // 空输入
    CHECK(radar_fusion_ftm_filter(NULL, 0, NULL) == RADAR_DIST_INVALID,
          "ftm_filter empty should be invalid");

    // 最近 session 的 P25：高值反射径不应抬高窗口输出。
    float history[5] = {2.0f, 2.1f, 2.2f, 8.0f, 12.0f};
    float p25 = radar_fusion_ftm_low_quantile(history, 5);
    CHECK(fabsf(p25 - 2.1f) < 0.01f, "ftm P25: %.2f, expected 2.10", p25);
    CHECK(radar_fusion_ftm_low_quantile(NULL, 0) == RADAR_DIST_INVALID,
          "ftm P25 empty should be invalid");

    // 单样本
    float one[1] = {3.3f};
    CHECK(fabsf(radar_fusion_ftm_filter(one, 1, NULL) - 3.3f) < 0.01f,
          "ftm_filter single sample");

    // 一致性 burst:spread 应接近 0
    float tight[6] = {5.0f, 5.1f, 4.9f, 5.05f, 4.95f, 5.0f};
    d = radar_fusion_ftm_filter(tight, 6, &spread);
    CHECK(fabsf(d - 5.0f) < 0.1f, "ftm_filter tight median: %.2f", d);
    CHECK(spread < 0.3f, "ftm_filter tight spread: %.2f", spread);

    // 非法浮点和负距离应被过滤
    float invalid[5] = {NAN, INFINITY, -1.0f, 2.0f, 2.1f};
    d = radar_fusion_ftm_filter(invalid, 5, &spread);
    CHECK(d > 1.9f && d < 2.2f, "ftm_filter invalid values: %.2f", d);

    printf("  ftm_filter: PASS\n");
}

static void test_ftm_suspicious(void) {
    // FTM 10m vs RSSI 3m:10 > 3*1.8=5.4 且 10 > 3+2 → 存疑(反射径)
    CHECK(radar_fusion_ftm_suspicious(10.0f, 3.0f), "ftm 10 vs rssi 3 should be suspicious");
    // FTM 3m vs RSSI 3m:一致
    CHECK(!radar_fusion_ftm_suspicious(3.0f, 3.0f), "ftm 3 vs rssi 3 should be consistent");
    // 滞回带:5.5m vs 3m → 5.5 > 5.4 但 5.5 < 5.0+2? 否,5.5>5 → 存疑
    CHECK(radar_fusion_ftm_suspicious(5.5f, 3.0f), "ftm 5.5 vs rssi 3 → suspicious");
    // 5.3m vs 3m:超过比例但在滞回内 → 不存疑
    CHECK(!radar_fusion_ftm_suspicious(5.3f, 3.0f), "ftm 5.3 vs rssi 3 within margin");
    printf("  ftm_suspicious: PASS\n");
}

static void test_fusion_update(void) {
    // 1) 无任何链路 → OOR
    radar_fusion_result_t r = radar_fusion_update(false, 0.0f,
                                                  RADAR_RSSI_ABSENT, RADAR_RSSI_ABSENT,
                                                  -42.0f, RADAR_ENV_OUTDOOR);
    CHECK(r.conf == RADAR_CONF_OOR, "no link should be OOR, got %d", r.conf);
    CHECK(r.distance_m == RADAR_DIST_INVALID, "no link distance should be invalid");

    // 2) 单链路 RSSI → LOW
    r = radar_fusion_update(false, 0.0f, -60.0f, RADAR_RSSI_ABSENT,
                            -42.0f, RADAR_ENV_OUTDOOR);
    CHECK(r.conf == RADAR_CONF_LOW, "single-link should be LOW, got %d", r.conf);
    CHECK(r.distance_m > 0.0f, "single-link should still give distance");

    // 3) 双向 RSSI → MED,距离基于对称平均
    r = radar_fusion_update(false, 0.0f, -60.0f, -66.0f,
                            -42.0f, RADAR_ENV_OUTDOOR);
    CHECK(r.conf == RADAR_CONF_MED, "dual-rssi should be MED, got %d", r.conf);
    CHECK(fabsf(r.sym_rssi_dbm - (-63.0f)) < 0.01f, "sym rssi %.2f", r.sym_rssi_dbm);
    float expect = radar_fusion_sym_distance(-63.0f, -42.0f, RADAR_ENV_OUTDOOR);
    CHECK(fabsf(r.distance_m - expect) < 0.01f, "dual-rssi distance %.2f vs %.2f",
          r.distance_m, expect);

    // 4) FTM 有效且一致 → HIGH,按距离逆方差加权融合(D8)
    float rssi_d = radar_fusion_sym_distance(-63.0f, -42.0f, RADAR_ENV_OUTDOOR);
    r = radar_fusion_update(true, 8.0f, -60.0f, -66.0f, -42.0f, RADAR_ENV_OUTDOOR);
    CHECK(r.conf == RADAR_CONF_HIGH, "ftm+sym should be HIGH, got %d", r.conf);
    CHECK(r.ftm_used, "ftm should be used");
    float w4 = radar_fusion_ftm_weight(0.5f * (8.0f + rssi_d));
    float fused = w4 * 8.0f + (1.0f - w4) * rssi_d;
    CHECK(fabsf(r.distance_m - fused) < 0.01f, "fused distance %.2f vs %.2f",
          r.distance_m, fused);
    CHECK(fabsf(r.ftm_weight - w4) < 0.001f, "ftm_weight %.3f vs %.3f",
          r.ftm_weight, w4);
    CHECK(w4 > 0.5f, "weight near 9m should favor FTM, got %.2f", w4);

    // 5) FTM 疑似反射径 → 降级 RSSI,MED,ftm_used=false
    r = radar_fusion_update(true, 30.0f, -60.0f, -66.0f, -42.0f, RADAR_ENV_OUTDOOR);
    CHECK(r.conf == RADAR_CONF_MED, "suspicious ftm should degrade to MED, got %d", r.conf);
    CHECK(!r.ftm_used, "suspicious ftm should not be used");
    CHECK(fabsf(r.distance_m - rssi_d) < 0.01f, "degraded distance should be rssi-based");

    // 6) 双向不对称(差 > 8dB)→ FTM 有效也只有 MED
    r = radar_fusion_update(true, 8.0f, -55.0f, -75.0f, -42.0f, RADAR_ENV_OUTDOOR);
    CHECK(r.conf == RADAR_CONF_MED, "asymmetric link should cap at MED, got %d", r.conf);

    // 7) 信号低于最小有效值 → OOR
    r = radar_fusion_update(false, 0.0f, -90.0f, -90.0f, -42.0f, RADAR_ENV_OUTDOOR);
    CHECK(r.conf == RADAR_CONF_OOR, "weak signal should be OOR, got %d", r.conf);

    // 8) spread 过大时禁止 HIGH
    radar_fusion_ftm_observation_t noisy = {
        .valid = true, .fresh = true, .distance_m = 8.0f,
        .spread_m = 3.0f, .valid_bursts = 16,
    };
    r = radar_fusion_update_ftm(&noisy, -60.0f, -66.0f, -42.0f,
                                RADAR_ENV_OUTDOOR);
    CHECK(r.conf == RADAR_CONF_MED, "large spread should cap at MED, got %d", r.conf);

    // 9) burst 数不足时禁止 HIGH
    noisy.spread_m = 0.1f;
    noisy.valid_bursts = 2;
    r = radar_fusion_update_ftm(&noisy, -60.0f, -66.0f, -42.0f,
                                RADAR_ENV_OUTDOOR);
    CHECK(r.conf == RADAR_CONF_MED, "few bursts should cap at MED, got %d", r.conf);

    // 10) 过期 FTM 不应被采纳
    noisy.valid_bursts = 16;
    noisy.fresh = false;
    r = radar_fusion_update_ftm(&noisy, -60.0f, -66.0f, -42.0f,
                                RADAR_ENV_OUTDOOR);
    CHECK(!r.ftm_used && r.conf == RADAR_CONF_MED,
          "stale FTM should fall back to MED RSSI");

    // 11) 非法 RSSI 不应污染距离
    r = radar_fusion_update(false, 0.0f, NAN, -66.0f, -42.0f, RADAR_ENV_OUTDOOR);
    CHECK(r.distance_m == RADAR_DIST_INVALID, "NaN RSSI should be invalid");

    printf("  fusion_update: PASS\n");
}

static void test_angles(void) {
    // 环形差
    CHECK(fabsf(radar_fusion_angle_diff(10.0f, 350.0f) - 20.0f) < 0.01f,
          "diff 10 vs 350 should be 20");
    CHECK(fabsf(radar_fusion_angle_diff(180.0f, 0.0f) - 180.0f) < 0.01f,
          "diff 180 vs 0 should be 180");

    // 环形均值:10° 与 350° 的均值应为 0°(而非 180°)
    float a2[2] = {10.0f, 350.0f};
    float m = radar_fusion_circular_mean(a2, 2);
    float err = radar_fusion_angle_diff(m, 0.0f);
    CHECK(err < 1.0f, "circular mean of 10/350 should be ~0, got %.2f", m);

    // 三个角度 90/100/110 → ~100
    float a3[3] = {90.0f, 100.0f, 110.0f};
    m = radar_fusion_circular_mean(a3, 3);
    CHECK(radar_fusion_angle_diff(m, 100.0f) < 1.0f, "mean of 90/100/110 → %.2f", m);

    // 空 → 无效
    CHECK(radar_fusion_circular_mean(NULL, 0) == RADAR_ANGLE_INVALID, "empty mean invalid");

    // 双端融合:一端无效
    CHECK(fabsf(radar_fusion_angle_pair(120.0f, RADAR_ANGLE_INVALID) - 120.0f) < 0.01f,
          "pair with invalid peer returns local");
    CHECK(fabsf(radar_fusion_angle_pair(RADAR_ANGLE_INVALID, 45.0f) - 45.0f) < 0.01f,
          "pair with invalid local returns peer");
    CHECK(radar_fusion_angle_pair(RADAR_ANGLE_INVALID, RADAR_ANGLE_INVALID) == RADAR_ANGLE_INVALID,
          "both invalid");

    // 双端均有效:环形均值
    float p = radar_fusion_angle_pair(80.0f, 100.0f);
    CHECK(radar_fusion_angle_diff(p, 90.0f) < 1.0f, "pair 80/100 → %.2f", p);

    printf("  angles: PASS\n");
}

static void test_env_est(void) {
    radar_env_est_t est;
    radar_env_est_init(&est);
    CHECK(est.env == RADAR_ENV_OUTDOOR, "env init should be outdoor");

    // 喂 80 个稳定样本(抖动 ±2dB)→ 保持室外
    for (int i = 0; i < RADAR_ENV_WINDOW; i++) {
        radar_env_est_update(&est, -60.0f + (float)(i % 5) - 2.0f);
    }
    CHECK(est.env == RADAR_ENV_OUTDOOR, "stable signal should stay outdoor");

    // 喂 80 个强抖动样本(±8dB)→ 判室内
    radar_env_est_init(&est);
    radar_env_t env = RADAR_ENV_OUTDOOR;
    for (int i = 0; i < RADAR_ENV_WINDOW; i++) {
        env = radar_env_est_update(&est, -60.0f + ((i % 2) ? 8.0f : -8.0f));
    }
    CHECK(env == RADAR_ENV_INDOOR, "jittery signal should detect indoor, got %d", env);

    // 长期弱信号(均值 < -80dBm,抖动中等)→ 遮挡档(n=3.5)
    radar_env_est_init(&est);
    env = RADAR_ENV_OUTDOOR;
    for (int i = 0; i < RADAR_ENV_MIN_TOTAL + RADAR_ENV_WINDOW; i++) {
        env = radar_env_est_update(&est, -85.0f + ((i % 2) ? 3.0f : -3.0f));
    }
    CHECK(env == RADAR_ENV_OBSTRUCTED, "weak sustained signal should detect obstructed, got %d", env);
    CHECK(fabsf(radar_env_path_loss_n(env) - 3.5f) < 0.01f, "obstructed n should be 3.5");

    // 室内强信号不应误判遮挡(均值远高于门限)
    radar_env_est_init(&est);
    env = RADAR_ENV_OBSTRUCTED;
    for (int i = 0; i < RADAR_ENV_MIN_TOTAL + RADAR_ENV_WINDOW; i++) {
        env = radar_env_est_update(&est, -55.0f + ((i % 2) ? 6.0f : -6.0f));
    }
    CHECK(env == RADAR_ENV_INDOOR, "strong jittery signal should stay indoor, got %d", env);

    printf("  env_est: PASS\n");
}

static void test_calib(void) {
    // 1m 处对称 RSSI -45 → 参考值 -45;同样 RSSI 距离应为 1m
    float ref = radar_fusion_calib_ref(-45.0f);
    CHECK(fabsf(ref - (-45.0f)) < 0.01f, "calib ref %.2f", ref);
    float d = radar_fusion_sym_distance(-45.0f, ref, RADAR_ENV_OUTDOOR);
    CHECK(fabsf(d - 1.0f) < 0.05f, "calibrated distance at ref should be 1m, got %.2f", d);

    // 钳位:异常值不超界
    CHECK(fabsf(radar_fusion_calib_ref(-10.0f) - (-30.0f)) < 0.01f, "calib clamp high");
    CHECK(fabsf(radar_fusion_calib_ref(-90.0f) - (-60.0f)) < 0.01f, "calib clamp low");

    printf("  calib: PASS\n");
}

static void test_ftm_weight_model(void) {
    // 无效输入 → 中性权重
    CHECK(fabsf(radar_fusion_ftm_weight(NAN) - 0.5f) < 0.001f, "NaN → 0.5");
    CHECK(fabsf(radar_fusion_ftm_weight(0.0f) - 0.5f) < 0.001f, "0 → 0.5");
    CHECK(fabsf(radar_fusion_ftm_weight(-5.0f) - 0.5f) < 0.001f, "negative → 0.5");

    // 单调不减:FTM 权重随距离上升(绝对误差底被摊薄,n 斜率墙随 ln d 增高)
    float prev = 0.0f;
    for (float d = 0.5f; d <= 30.5f; d += 0.5f) {
        float w = radar_fusion_ftm_weight(d);
        CHECK(w >= prev - 0.0005f, "weight not monotone at %.1fm: %.3f < %.3f",
              d, w, prev);
        prev = w;
    }

    // 关键锚点:<=3m 偏 RSSI、约 5m 交叉、>=8m 偏 FTM(80% 精度评审 D8)
    float w3 = radar_fusion_ftm_weight(3.0f);
    float w5 = radar_fusion_ftm_weight(5.0f);
    float w8 = radar_fusion_ftm_weight(8.0f);
    float w25 = radar_fusion_ftm_weight(25.0f);
    CHECK(w3 < 0.5f, "3m should favor RSSI, w=%.3f", w3);
    CHECK(w5 > 0.35f && w5 < 0.65f, "5m should be the crossover, w=%.3f", w5);
    CHECK(w8 > 0.7f, "8m should favor FTM, w=%.3f", w8);
    CHECK(w25 > 0.9f, "25m should be FTM-dominant, w=%.3f", w25);

    printf("  ftm_weight_model: PASS\n");
}

static void test_fusion_v3_weighted(void) {
    // 近距:RSSI 3.0m / FTM 3.6m 一致 → 融合值应更靠近 RSSI
    radar_fusion_ftm_observation_t ftm = {
        .valid = true, .fresh = true, .calibrated = true,
        .distance_m = 3.6f, .spread_m = 0.3f, .valid_bursts = 16,
    };
    radar_fusion_result_t r = radar_fusion_update_v3(&ftm, -52.5f, -52.5f, -42.0f,
                                                     RADAR_LEVEL_L0_LOS, true);
    CHECK(r.ftm_used, "v3 near: ftm should contribute");
    CHECK(r.ftm_weight < 0.5f, "v3 near: weight should favor RSSI, w=%.3f", r.ftm_weight);
    CHECK(fabsf(r.distance_m - 3.0f) < fabsf(r.distance_m - 3.6f),
          "v3 near: fused %.2f should lean to rssi 3.0 over ftm 3.6", r.distance_m);

    // 远距:RSSI ~11.9m / FTM 12.6m 一致 → 融合值应更靠近 FTM
    ftm.distance_m = 12.6f;
    r = radar_fusion_update_v3(&ftm, -65.7f, -65.7f, -42.0f,
                               RADAR_LEVEL_L0_LOS, true);
    CHECK(r.ftm_used, "v3 far: ftm should contribute");
    CHECK(r.ftm_weight > 0.7f, "v3 far: weight should favor FTM, w=%.3f", r.ftm_weight);
    CHECK(fabsf(r.distance_m - 12.6f) < fabsf(r.distance_m - r.rssi_dist_m),
          "v3 far: fused %.2f should lean to ftm 12.6 over rssi %.2f",
          r.distance_m, r.rssi_dist_m);

    printf("  fusion_v3_weighted: PASS\n");
}

int main(void) {
    test_sym_rssi();
    test_ftm_filter();
    test_ftm_suspicious();
    test_fusion_update();
    test_ftm_weight_model();
    test_fusion_v3_weighted();
    test_angles();
    test_env_est();
    test_calib();
    if (s_failures != 0) {
        fprintf(stderr, "FAILED: %d check(s)\n", s_failures);
        return 1;
    }
    printf("PASS: radar fusion engine verified\n");
    return 0;
}
