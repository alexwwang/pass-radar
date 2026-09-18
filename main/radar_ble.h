#pragma once

// BLE 长距哨兵链路(NimBLE + BLE 5.0 Coded PHY)。
//
// 角色:辅助链路,非主测距源。
//   - Wi-Fi/ESP-NOW 判超界后,BLE Coded PHY 链路仍可达,提供远距存在性提示;
//   - 广播 payload 携带产品标识、协议版本和 BLE identity,避免仅按名称认定目标。

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

// 初始化 NimBLE、启动广播与扫描。失败返回错误码(调用方降级为无哨兵)。
// 需在 radar_espnow_init()(NVS/event loop 就绪)之后调用。
esp_err_t radar_ble_init(void);

// 停止扫描/广播并释放 NimBLE host/controller 资源。可重复调用。
void radar_ble_deinit(void);

// 暂停/恢复哨兵扫描(保留广播):扫盘跳频与标定期间让出射频时间片,
// 避免 Coded PHY 长窗口扫描挤压 ESP-NOW/FTM 收包。幂等。
void radar_ble_pause(bool paused);

// NimBLE sync callback 已完成且广播/扫描已成功提交。
bool radar_ble_ready(void);

// 最近一次收到的目标 BLE 广播 RSSI(dBm)。无数据返回 -127。
int radar_ble_peer_rssi(void);

// 最近 10s 内收到过目标 BLE 广播。
bool radar_ble_peer_alive(void);

// 设置由 ESP-NOW v2 验证过的目标 identity；未绑定前不接受 BLE sentinel。
void radar_ble_set_expected_identity(const uint8_t identity[6]);

// 最近目标广播携带的 BLE identity。未知时返回 false。
bool radar_ble_peer_identity(uint8_t out_identity[6]);
