#pragma once

// 旋转扫盘峰值检测与距离估算 —— 纯数学模块,无 ESP-IDF 依赖,可 host test。

#include <stdint.h>

// 采样点结构(与 radar_espnow.c 共用)
typedef struct {
    uint32_t timestamp_ms;  // 相对扫盘起始时刻的毫秒偏移
    float    rssi;          // 卡尔曼滤波后的信号强度
} radar_sample_t;

// 信号阈值:低于此值视为信号微弱/超出范围,不报告方向。
#define RADAR_RSSI_MIN_VALID (-88.0f)

// 自由空间路径损耗模型参数(户外开阔地)
#define RADAR_TX_POWER_REF  (-42.0f)  // 1 米处参考 RSSI(dBm)
#define RADAR_PATH_LOSS_N   (2.2f)    // 户外衰减因子

// 由滤波后 RSSI 估算距离(米)。rssi 越小距离越远。
float radar_sweep_calc_distance(float rssi);

// 在采样数组中找 RSSI 峰值,映射为 0°~360° 角度(相对于扫盘起始方向)。
// 峰值时间点 / 总时长 × 360 = 检测角度。
// 无采样返回 -1。
int radar_sweep_peak_angle(const radar_sample_t *samples, unsigned count,
                           uint32_t sweep_duration_ms);

// 从采样数组中返回峰值 RSSI(供阈值判断用)。
float radar_sweep_peak_rssi(const radar_sample_t *samples, unsigned count);
