#pragma once

// ESP-NOW 广播收发模块 —— 为雷达提供 RSSI 采集与对端状态交换。
// 接收回调运行于 Wi-Fi 任务上下文,不得触碰 LVGL。

#include "esp_err.h"
#include "radar_kalman.h"
#include "radar_protocol.h"
#include "radar_sweep.h"
#include <stdbool.h>
#include <stdint.h>

#define RADAR_MAX_SAMPLES 100
#define RADAR_RAW_RING 256
#define RADAR_SWEEP_DURATION_MS 4000

esp_err_t radar_espnow_init(void);
esp_err_t radar_espnow_start_broadcast(const char *device_name);
void radar_espnow_stop_broadcast(void);
float radar_espnow_filtered_rssi(void);
int radar_espnow_raw_rssi(void);
// Copy recent unfiltered RSSI samples in chronological order. This API is
// intended for the LVGL polling task; the Wi-Fi callback performs lock-free writes.
unsigned radar_espnow_raw_history(float *out_dbm, uint32_t *out_ts_ms, unsigned max);

bool radar_espnow_peer_alive(void);
int radar_espnow_peer_rssi(void);
float radar_espnow_peer_angle(void);
float radar_espnow_peer_distance(void);
void radar_espnow_report_result(float angle_deg, float dist_m);

// 接收源 MAC 仅表示 ESP-NOW 发送接口；FTM 必须使用已验证的 AP MAC。
bool radar_espnow_peer_mac(uint8_t out_mac[RADAR_MAC_LEN]);
bool radar_espnow_peer_ap_mac(uint8_t out_mac[RADAR_MAC_LEN]);
bool radar_espnow_peer_ble_identity(uint8_t out_identity[RADAR_BLE_ID_LEN]);
bool radar_espnow_is_ftm_initiator(void);
bool radar_espnow_is_hopping(void);

void radar_espnow_start_sweep(void);
unsigned radar_espnow_stop_sweep(void);
unsigned radar_espnow_get_samples(radar_sample_t *out, unsigned max_count);
bool radar_espnow_is_sweeping(void);

// 对端扫盘状态(由协议 v2 SWEEPING 标记驱动,短暂保持)。
// 返回 true 时写出对端 sweep_id 与已进行毫秒数,供跟随启动判断。
bool radar_espnow_peer_sweep(uint32_t *out_sweep_id, uint32_t *out_elapsed_ms);

// 对端请求保持静止（HOMING/扫盘期间，短时有效）。
void radar_espnow_set_homing(bool active);
bool radar_espnow_peer_wants_still(void);

esp_err_t radar_espnow_calib_save(float tx_ref_sym);
esp_err_t radar_espnow_calib_load(float *tx_ref_sym);
esp_err_t radar_espnow_ftm_offset_save(float offset_m);
esp_err_t radar_espnow_ftm_offset_load(float *offset_m);
void radar_espnow_deinit(void);
