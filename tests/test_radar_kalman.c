// tests/test_radar_kalman.c —— 卡尔曼滤波器 host test。
// 验证:收敛性(输出跟踪真实值)与降噪(输出方差 < 输入方差)。
#include "../main/radar_kalman.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define TEST_SAMPLES 100
#define TRUE_RSSI    (-70.0f)
#define NOISE_AMP    5.0f

static float pseudo_random_noise(int seed) {
    // 简单 LCG 伪随机数,产生 [-NOISE_AMP, +NOISE_AMP] 均匀分布
    static unsigned long state = 0;
    state = state * 1103515245UL + 12345UL + (unsigned long)seed;
    return ((float)(state % 2000) / 1000.0f - 1.0f) * NOISE_AMP;
}

int main(void) {
    radar_kalman_t kf;
    radar_kalman_init(&kf, TRUE_RSSI);

    float raw_sum = 0.0f, raw_sq = 0.0f;
    float filt_sum = 0.0f, filt_sq = 0.0f;
    float raw[TEST_SAMPLES], filt[TEST_SAMPLES];

    // 生成含噪声的 RSSI 序列,经过滤波器
    for (int i = 0; i < TEST_SAMPLES; i++) {
        float noise = pseudo_random_noise(i);
        raw[i] = TRUE_RSSI + noise;
        filt[i] = radar_kalman_update(&kf, raw[i]);
        raw_sum += raw[i];    raw_sq += raw[i] * raw[i];
        filt_sum += filt[i];  filt_sq += filt[i] * filt[i];
    }

    float raw_mean = raw_sum / TEST_SAMPLES;
    float filt_mean = filt_sum / TEST_SAMPLES;
    float raw_var = raw_sq / TEST_SAMPLES - raw_mean * raw_mean;
    float filt_var = filt_sq / TEST_SAMPLES - filt_mean * filt_mean;

    printf("raw:  mean=%.2f  var=%.2f\n", raw_mean, raw_var);
    printf("filt: mean=%.2f  var=%.2f\n", filt_mean, filt_var);

    // 验证 1:滤波后均值应在真值 ±2 以内
    if (fabsf(filt_mean - TRUE_RSSI) > 2.0f) {
        fprintf(stderr, "FAIL: filtered mean %.2f too far from true %.2f\n",
                filt_mean, TRUE_RSSI);
        return 1;
    }

    // 验证 2:滤波后方差应小于原始方差(降噪)
    if (filt_var >= raw_var) {
        fprintf(stderr, "FAIL: filtered variance %.2f >= raw %.2f (no noise reduction)\n",
                filt_var, raw_var);
        return 1;
    }

    // 验证 3:最终估计值应在真值 ±3 以内
    float final_est = kf.x;
    if (fabsf(final_est - TRUE_RSSI) > 3.0f) {
        fprintf(stderr, "FAIL: final estimate %.2f too far from true %.2f\n",
                final_est, TRUE_RSSI);
        return 1;
    }

    printf("PASS: kalman filter converges and reduces noise\n");
    return 0;
}
