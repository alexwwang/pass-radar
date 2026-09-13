#pragma once

// 一维卡尔曼滤波器 —— 平滑 ESP-NOW RSSI 抖动。
// 纯 C 逻辑,无 ESP-IDF 依赖,可用 host test 验证。

typedef struct {
    float q;  // 过程噪声协方差(Process Noise)
    float r;  // 测量噪声协方差(Measurement Noise,建议 3.0~5.0)
    float x;  // 估计 RSSI(当前最优估计)
    float p;  // 估计误差协方差
    float k;  // 卡尔曼增益(每步重算)
} radar_kalman_t;

// 初始化滤波器。init_rssi 通常取 -70.0(室外初始估计)。
void radar_kalman_init(radar_kalman_t *kf, float init_rssi);

// 输入一次测量值,返回滤波后的估计值。
float radar_kalman_update(radar_kalman_t *kf, float measurement);
