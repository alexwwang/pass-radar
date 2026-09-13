// tests/test_radar_sweep.c —— 扫盘峰值检测与距离估算 host test。
#include "../main/radar_sweep.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define SWEEP_MS 4000

static int test_distance(void) {
    // 1m 参考点
    float d1 = radar_sweep_calc_distance(-42.0f);
    if (fabsf(d1 - 1.0f) > 0.2f) {
        fprintf(stderr, "FAIL: distance at -42dBm = %.2f, expected ~1.0\n", d1);
        return 1;
    }

    // 距离应随 RSSI 降低而增大
    float d2 = radar_sweep_calc_distance(-64.0f);
    if (d2 < 5.0f || d2 > 15.0f) {
        fprintf(stderr, "FAIL: distance at -64dBm = %.2f, expected ~8.5\n", d2);
        return 1;
    }

    // 信号越弱距离越远
    float d3 = radar_sweep_calc_distance(-80.0f);
    if (d3 <= d2) {
        fprintf(stderr, "FAIL: distance should increase with weaker signal\n");
        return 1;
    }

    printf("  distance: PASS (-42→%.1fm, -64→%.1fm, -80→%.1fm)\n", d1, d2, d3);
    return 0;
}

static int test_peak_angle(void) {
    // 构造 80 个采样,峰值在 t=2000ms(扫盘中点)
    radar_sample_t samples[80];
    for (int i = 0; i < 80; i++) {
        samples[i].timestamp_ms = (uint32_t)(i * 50);  // 50ms 间隔
        samples[i].rssi = -80.0f + (float)(rand() % 5);  // 基础噪声
    }
    // 在 t=2000ms 处插入峰值(第 40 个采样)
    samples[40].rssi = -55.0f;

    int angle = radar_sweep_peak_angle(samples, 80, SWEEP_MS);
    // t=2000 / 4000 = 0.5 → 180°
    if (angle < 175 || angle > 185) {
        fprintf(stderr, "FAIL: peak at t=2000/4000 → angle %d, expected ~180\n", angle);
        return 1;
    }

    // 峰值在起点
    radar_sample_t start_peak[4] = {
        { .timestamp_ms = 0,    .rssi = -50.0f },
        { .timestamp_ms = 1000, .rssi = -80.0f },
        { .timestamp_ms = 2000, .rssi = -80.0f },
        { .timestamp_ms = 3000, .rssi = -80.0f },
    };
    angle = radar_sweep_peak_angle(start_peak, 4, SWEEP_MS);
    if (angle != 0) {
        fprintf(stderr, "FAIL: peak at t=0 → angle %d, expected 0\n", angle);
        return 1;
    }

    // 无采样 → -1
    angle = radar_sweep_peak_angle(NULL, 0, SWEEP_MS);
    if (angle != -1) {
        fprintf(stderr, "FAIL: zero samples → angle %d, expected -1\n", angle);
        return 1;
    }

    printf("  peak_angle: PASS (mid→%d°, start→%d°, empty→%d)\n", 180, 0, -1);
    return 0;
}

static int test_peak_rssi(void) {
    radar_sample_t samples[3] = {
        { .timestamp_ms = 0,    .rssi = -70.0f },
        { .timestamp_ms = 1000, .rssi = -55.0f },
        { .timestamp_ms = 2000, .rssi = -80.0f },
    };
    float peak = radar_sweep_peak_rssi(samples, 3);
    if (fabsf(peak - (-55.0f)) > 0.1f) {
        fprintf(stderr, "FAIL: peak_rssi = %.1f, expected -55.0\n", peak);
        return 1;
    }

    // 空数组
    peak = radar_sweep_peak_rssi(NULL, 0);
    if (peak != -999.0f) {
        fprintf(stderr, "FAIL: empty peak_rssi = %.1f, expected -999\n", peak);
        return 1;
    }

    printf("  peak_rssi: PASS (max=%.1f, empty=%.1f)\n", -55.0f, -999.0f);
    return 0;
}

int main(void) {
    if (test_distance() != 0) return 1;
    if (test_peak_angle() != 0) return 1;
    if (test_peak_rssi() != 0) return 1;
    printf("PASS: radar sweep algorithms verified\n");
    return 0;
}
