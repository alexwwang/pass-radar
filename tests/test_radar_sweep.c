// tests/test_radar_sweep.c —— 扫盘峰值检测与距离估算 host test。
// v2:抛物线插值、峰显著性、信道调度、多信道投票。
#include "../main/radar_sweep.h"
#include "../main/radar_fusion.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define SWEEP_MS 4000

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

static void test_distance(void) {
    // 1m 参考点
    float d1 = radar_sweep_calc_distance(-42.0f);
    CHECK(fabsf(d1 - 1.0f) <= 0.2f, "distance at -42dBm = %.2f, expected ~1.0", d1);

    // 距离应随 RSSI 降低而增大
    float d2 = radar_sweep_calc_distance(-64.0f);
    CHECK(d2 >= 5.0f && d2 <= 15.0f, "distance at -64dBm = %.2f, expected ~8.5", d2);

    // 信号越弱距离越远
    float d3 = radar_sweep_calc_distance(-80.0f);
    CHECK(d3 > d2, "distance should increase with weaker signal");

    printf("  distance: PASS (-42->%.1fm, -64->%.1fm, -80->%.1fm)\n", d1, d2, d3);
}

static void test_peak_angle(void) {
    // 构造 80 个采样,峰值在 t=2000ms(扫盘中点)
    radar_sample_t samples[80];
    for (int i = 0; i < 80; i++) {
        samples[i].timestamp_ms = (uint32_t)(i * 50);  // 50ms 间隔
        samples[i].rssi = -80.0f + (float)(rand() % 5);  // 基础噪声
        samples[i].channel = 0;
    }
    // 在 t=2000ms 处插入峰值(第 40 个采样)
    samples[40].rssi = -55.0f;

    int angle = radar_sweep_peak_angle(samples, 80, SWEEP_MS);
    // t=2000 / 4000 = 0.5 → 180°
    CHECK(angle >= 175 && angle <= 185, "peak at t=2000/4000 -> angle %d, expected ~180", angle);

    // 峰值在起点
    radar_sample_t start_peak[4] = {
        { .timestamp_ms = 0,    .rssi = -50.0f, .channel = 0 },
        { .timestamp_ms = 1000, .rssi = -80.0f, .channel = 0 },
        { .timestamp_ms = 2000, .rssi = -80.0f, .channel = 0 },
        { .timestamp_ms = 3000, .rssi = -80.0f, .channel = 0 },
    };
    angle = radar_sweep_peak_angle(start_peak, 4, SWEEP_MS);
    CHECK(angle == 0, "peak at t=0 -> angle %d, expected 0", angle);

    // 无采样 → -1
    angle = radar_sweep_peak_angle(NULL, 0, SWEEP_MS);
    CHECK(angle == -1, "zero samples -> angle %d, expected -1", angle);

    printf("  peak_angle: PASS\n");
}

static void test_peak_rssi(void) {
    radar_sample_t samples[3] = {
        { .timestamp_ms = 0,    .rssi = -70.0f, .channel = 0 },
        { .timestamp_ms = 1000, .rssi = -55.0f, .channel = 0 },
        { .timestamp_ms = 2000, .rssi = -80.0f, .channel = 0 },
    };
    float peak = radar_sweep_peak_rssi(samples, 3);
    CHECK(fabsf(peak - (-55.0f)) <= 0.1f, "peak_rssi = %.1f, expected -55.0", peak);

    // 空数组
    peak = radar_sweep_peak_rssi(NULL, 0);
    CHECK(peak == -999.0f, "empty peak_rssi = %.1f, expected -999", peak);

    printf("  peak_rssi: PASS\n");
}

// ---- v2 增强 ----

// 构造余弦形人体遮挡方向图:peak_time_ms 处最亮,遮挡深度 depth_db。
static unsigned build_pattern(radar_sample_t *out, unsigned max,
                              uint32_t sweep_ms, float peak_time_ms, float depth_db,
                              uint8_t channel) {
    unsigned n = 0;
    for (uint32_t t = 0; t < sweep_ms && n < max; t += 50) {
        float phase = ((float)t - peak_time_ms) / (float)sweep_ms * 2.0f * 3.14159265f;
        out[n].timestamp_ms = t;
        out[n].rssi = -70.0f + depth_db * 0.5f * (1.0f + cosf(phase));
        out[n].channel = channel;
        n++;
    }
    return n;
}

static void test_peak_angle_interp(void) {
    radar_sample_t samples[80];
    // 峰值精确落在两个采样点之间(t=2025ms,采样间隔 50ms)
    unsigned n = build_pattern(samples, 80, SWEEP_MS, 2025.0f, 12.0f, 0);
    float angle = radar_sweep_peak_angle_interp(samples, n, SWEEP_MS);
    // 2025/4000*360 = 182.25°;亚采样插值应显著优于取整样本(180° 或 184.5°)
    CHECK(fabsf(angle - 182.25f) < 2.0f, "interp angle %.2f, expected ~182.25", angle);

    // 空输入
    CHECK(radar_sweep_peak_angle_interp(NULL, 0, SWEEP_MS) == -1.0f, "interp empty -> -1");
    CHECK(radar_sweep_peak_angle_interp(samples, n, 0) == -1.0f, "interp dur=0 -> -1");

    // 跨信道相邻样本禁止插值，应返回离散峰时间 180°
    radar_sample_t cross_channel[3] = {
        { .timestamp_ms = 1950, .rssi = -60.0f, .channel = RADAR_SWEEP_CH_A },
        { .timestamp_ms = 2000, .rssi = -50.0f, .channel = RADAR_SWEEP_CH_B },
        { .timestamp_ms = 2050, .rssi = -61.0f, .channel = RADAR_SWEEP_CH_A },
    };
    float cross = radar_sweep_peak_angle_interp(cross_channel, 3, SWEEP_MS);
    CHECK(fabsf(cross - 180.0f) < 0.01f,
          "cross-channel interpolation must be disabled, got %.2f", cross);

    // 与整数版本一致性(峰在采样点上时两者接近)
    n = build_pattern(samples, 80, SWEEP_MS, 2000.0f, 12.0f, 0);
    float fi = radar_sweep_peak_angle_interp(samples, n, SWEEP_MS);
    int   ii = radar_sweep_peak_angle(samples, n, SWEEP_MS);
    CHECK(fabsf(fi - (float)ii) < 5.0f, "interp %.1f vs int %d should be close", fi, ii);

    printf("  peak_angle_interp: PASS (half-sample peak -> %.2f deg)\n", 182.25f);
}

static void test_peak_prominence(void) {
    radar_sample_t samples[80];

    // 12dB 遮挡深度 → 显著性应 > 10dB
    unsigned n = build_pattern(samples, 80, SWEEP_MS, 2000.0f, 12.0f, 0);
    float p = radar_sweep_peak_prominence(samples, n);
    CHECK(p > 10.0f, "prominence of 12dB pattern = %.1f, expected > 10", p);

    // 纯噪声(无方向峰)→ 显著性应 < 6dB 门限
    for (unsigned i = 0; i < 80; i++) {
        samples[i].timestamp_ms = i * 50;
        samples[i].rssi = -70.0f + (float)(rand() % 20) / 10.0f;  // ±1dB
        samples[i].channel = 0;
    }
    p = radar_sweep_peak_prominence(samples, 80);
    CHECK(p < RADAR_SWEEP_MIN_PROMINENCE_DB,
          "prominence of flat noise = %.1f, expected < %.1f", p, RADAR_SWEEP_MIN_PROMINENCE_DB);

    // 空输入
    CHECK(radar_sweep_peak_prominence(NULL, 0) == 0.0f, "empty prominence -> 0");

    printf("  peak_prominence: PASS\n");
}

static void test_channel_schedule(void) {
    // 快跳轮询:RADAR_SWEEP_HOP_DWELL_MS 驻留,1 → 6 → 11 → 1 …
    uint32_t dwell = RADAR_SWEEP_HOP_DWELL_MS;
    CHECK(radar_sweep_channel_at(0, SWEEP_MS) == RADAR_SWEEP_CH_A, "t=0 -> ch1");
    CHECK(radar_sweep_channel_at(dwell + dwell / 2, SWEEP_MS) == RADAR_SWEEP_CH_B,
          "t=1.5 dwell -> ch6");
    CHECK(radar_sweep_channel_at(2 * dwell + dwell / 2, SWEEP_MS) == RADAR_SWEEP_CH_C,
          "t=2.5 dwell -> ch11");
    CHECK(radar_sweep_channel_at(3 * dwell + dwell / 2, SWEEP_MS) == RADAR_SWEEP_CH_A,
          "t=3.5 dwell -> ch1(循环)");
    CHECK(radar_sweep_channel_at(3999, SWEEP_MS) == RADAR_SWEEP_CH_A, "t=3999 -> ch1");
    printf("  channel_schedule: PASS\n");
}

// 构造快跳交织采样:每信道独立方向图,真峰在 true_peak_ms;
// fake_ch 指定的信道被多径污染,峰在 fake_peak_ms。
static unsigned build_interleaved(radar_sample_t *out, unsigned max,
                                  float true_peak_ms, uint8_t fake_ch, float fake_peak_ms,
                                  bool fake_flat) {
    unsigned n = 0;
    for (uint32_t t = 0; t < SWEEP_MS && n < max; t += 50) {
        uint8_t ch = radar_sweep_channel_at(t, SWEEP_MS);
        out[n].timestamp_ms = t;
        out[n].channel = ch;
        if (fake_flat && ch == fake_ch) {
            out[n].rssi = -70.0f + (float)(n % 3) * 0.5f;  // 平坦无峰
        } else {
            float peak_t = (ch == fake_ch) ? fake_peak_ms : true_peak_ms;
            float phase = ((float)t - peak_t) / (float)SWEEP_MS * 2.0f * 3.14159265f;
            out[n].rssi = -70.0f + 12.0f * 0.5f * (1.0f + cosf(phase));
        }
        n++;
    }
    return n;
}

static void test_channel_angles_vote(void) {
    radar_sample_t samples[240];

    // ch1/ch11 真峰 180°(t=2000),ch6 假峰 90°(t=1000)
    unsigned n = build_interleaved(samples, 240, 2000.0f, RADAR_SWEEP_CH_B, 1000.0f, false);

    float angles[4];
    unsigned cnt = radar_sweep_channel_angles(samples, n, SWEEP_MS, angles, 4);
    CHECK(cnt == 3, "should detect 3 channel angles, got %u", cnt);

    // 投票(环形中值)应选中 180° 附近,抑制 ch6 假峰
    float spread = -1.0f;
    float voted = radar_sweep_angle_vote(angles, cnt, &spread);
    CHECK(radar_fusion_angle_diff(voted, 180.0f) < 10.0f,
          "voted angle %.1f, expected ~180 (ch6 outlier rejected)", voted);
    CHECK(spread > 45.0f, "spread should reflect the 90-deg outlier, got %.1f", spread);

    // 全部一致时 spread 小
    float good[3] = {178.0f, 182.0f, 180.0f};
    voted = radar_sweep_angle_vote(good, 3, &spread);
    CHECK(radar_fusion_angle_diff(voted, 180.0f) < 3.0f, "good vote %.1f", voted);
    CHECK(spread < 6.0f, "good spread %.1f", spread);

    // 单角度与空输入
    float one[1] = {42.0f};
    CHECK(fabsf(radar_sweep_angle_vote(one, 1, NULL) - 42.0f) < 0.01f, "single vote");
    CHECK(radar_sweep_angle_vote(NULL, 0, NULL) == -1.0f, "empty vote");

    // 显著性不足的信道应被剔除:ch6 平坦无峰
    n = build_interleaved(samples, 240, 2000.0f, RADAR_SWEEP_CH_B, 0.0f, true);
    cnt = radar_sweep_channel_angles(samples, n, SWEEP_MS, angles, 4);
    CHECK(cnt == 2, "flat channel should be rejected, got %u angles", cnt);

    printf("  channel_angles_vote: PASS\n");
}

// ---- 旋转速度自校正(阶段 2) ----

// 名义 4s 窗口内旋转 early_stop_ms 后停转:完整余弦方向图压缩进
// [0, early_stop_ms](一圈提前转完),尾部为平坦噪声(无方向信息)。
static unsigned build_early_stop(radar_sample_t *out, unsigned max,
                                 uint32_t early_stop_ms, float peak_ms,
                                 uint8_t channel) {
    unsigned n = 0;
    for (uint32_t t = 0; t < SWEEP_MS && n < max; t += 50) {
        out[n].timestamp_ms = t;
        out[n].channel = channel;
        if (t <= early_stop_ms) {
            float phase = ((float)t - peak_ms) / (float)early_stop_ms * 2.0f * 3.14159265f;
            out[n].rssi = -70.0f + 12.0f * 0.5f * (1.0f + cosf(phase));
        } else {
            out[n].rssi = -70.0f + (float)(n % 3) * 0.4f;  // 停转后平台噪声
        }
        n++;
    }
    return n;
}

static void test_rotation_self_correction(void) {
    radar_sample_t samples[128];

    // 1) 名义 4s 完整旋转:不修正(时长保持 4000,样本数不变)
    unsigned n = build_pattern(samples, 128, SWEEP_MS, 2000.0f, 12.0f, 0);
    uint32_t dur = 0;
    unsigned m = radar_sweep_effective_duration_ms(samples, n, SWEEP_MS, &dur);
    CHECK(dur == SWEEP_MS, "nominal sweep should not be corrected, dur=%lu", (unsigned long)dur);
    CHECK(m == n, "nominal sweep should keep all samples, %u vs %u", m, n);

    // 2) 提前 3s 停转(真峰 1500ms → 实际旋转 180°):
    //    未修正映射 1500/4000 = 135°;修正后 1500/3000 = 180°。
    n = build_early_stop(samples, 128, 3000, 1500.0f, 0);
    uint32_t period = radar_sweep_estimate_period_ms(samples, n, SWEEP_MS);
    CHECK(period >= 2900 && period <= 3100,
          "estimated period %lu, expected ~3000", (unsigned long)period);

    m = radar_sweep_effective_duration_ms(samples, n, SWEEP_MS, &dur);
    CHECK(dur <= 3100 && m < n,
          "early stop should trim samples: m=%u n=%u dur=%lu", m, n, (unsigned long)dur);

    float ang_uncorrected = radar_sweep_harmonic_angle(samples, n, SWEEP_MS, NULL);
    CHECK(radar_fusion_angle_diff(ang_uncorrected, 135.0f) < 15.0f,
          "uncorrected angle %.1f should be near 135 (biased)", ang_uncorrected);

    float amp = 0.0f;
    float ang_corrected = radar_sweep_harmonic_angle(samples, m, dur, &amp);
    CHECK(ang_corrected >= 0.0f, "corrected fit should succeed (amp %.1f)", amp);
    if (ang_corrected >= 0.0f) {
        CHECK(radar_fusion_angle_diff(ang_corrected, 180.0f) < 10.0f,
              "corrected angle %.1f should be near 180", ang_corrected);
    }

    // 3) 样本太少或平台无峰:不修正
    n = build_early_stop(samples, 128, 500, 250.0f, 0);  // 早停且无完整一圈
    period = radar_sweep_estimate_period_ms(samples, n, SWEEP_MS);
    CHECK(period == SWEEP_MS, "flat/degenerate samples should not be corrected, got %lu",
          (unsigned long)period);

    printf("  rotation_self_correction: PASS\n");
}

static void test_harmonic_fit_no_gate(void) {
    radar_sample_t samples[80];
    unsigned n = build_pattern(samples, 80, SWEEP_MS, 2000.0f, 12.0f, 0);

    float amp = 0.0f, res = -1.0f;
    float ang = radar_sweep_harmonic_fit(samples, n, SWEEP_MS, &amp, &res);
    CHECK(ang >= 0.0f && radar_fusion_angle_diff(ang, 180.0f) < 5.0f,
          "fit angle %.1f should be near 180", ang);
    CHECK(amp > 5.0f, "fit amplitude %.1f should be ~6 (half of 12dB depth)", amp);
    CHECK(res >= 0.0f && res < 1.0f, "fit residual %.2f should be small", res);

    // 样本不足
    CHECK(radar_sweep_harmonic_fit(samples, 3, SWEEP_MS, NULL, NULL) < 0.0f,
          "too few samples should fail");

    printf("  harmonic_fit_no_gate: PASS\n");
}

int main(void) {
    test_distance();
    test_peak_angle();
    test_peak_rssi();
    test_peak_angle_interp();
    test_peak_prominence();
    test_channel_schedule();
    test_channel_angles_vote();
    test_rotation_self_correction();
    test_harmonic_fit_no_gate();
    if (s_failures != 0) {
        fprintf(stderr, "FAILED: %d check(s)\n", s_failures);
        return 1;
    }
    printf("PASS: radar sweep algorithms verified\n");
    return 0;
}
