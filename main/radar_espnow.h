#pragma once

// ESP-NOW 广播收发模块 —— 为雷达提供 RSSI 采集。
// 接收回调运行于 Wi-Fi 任务上下文,不得触碰 LVGL。

#include "esp_err.h"
#include "radar_kalman.h"
#include "radar_sweep.h"
#include <stdbool.h>
#include <stdint.h>

#define RADAR_MAX_SAMPLES 100
#define RADAR_SWEEP_DURATION_MS 4000

// 数据包结构(发射端发送,接收端解析)
typedef struct __attribute__((packed)) {
    char     device_name[12];
    uint32_t sequence;
} radar_packet_t;

// ESP-NOW 初始化(NVS/netif/event-loop/Wi-Fi/ESP-NOW 全链路)。
// 必须在 app_main 中调用。
esp_err_t radar_espnow_init(void);

// 获取最新滤波后 RSSI(volatile,线程安全读取)。
float radar_espnow_filtered_rssi(void);

// 获取最新原始 RSSI。
int radar_espnow_raw_rssi(void);

// 进入扫盘采样模式。清空采样缓冲区,记录起始时刻。
void radar_espnow_start_sweep(void);

// 退出扫盘采样模式。返回采样数量。
unsigned radar_espnow_stop_sweep(void);

// 拷贝采样数据到调用者缓冲区。返回实际拷贝数量。
unsigned radar_espnow_get_samples(radar_sample_t *out, unsigned max_count);

// 是否在扫盘采样中。
bool radar_espnow_is_sweeping(void);

// 释放 ESP-NOW 与 Wi-Fi 资源。
void radar_espnow_deinit(void);
