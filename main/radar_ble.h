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

// 上传模式级挂起/恢复:整个 BT 控制器下电(nimble_port_deinit 内部
// esp_bt_controller_disable/deinit),Wi-Fi 扫描/关联不再受共存调度挤压
// (真机踩雷:控制器开着时驱动强制回退默认扫描参数,全信道扫描实际只在
// 主信道有有效驻留,贴脸的路由都扫不到)。恢复 = 重新 init + host 重新
// sync。每会话一次,代价约 1s,可接受。
void radar_ble_suspend(void);
esp_err_t radar_ble_resume(void);

// 暂停/恢复哨兵扫描(保留广播):扫盘跳频与标定期间让出射频时间片,
// 避免 Coded PHY 长窗口扫描挤压 ESP-NOW/FTM 收包。幂等。
void radar_ble_pause(bool paused);

// NimBLE sync callback 已完成且广播/扫描已成功提交。
bool radar_ble_ready(void);

// 最近一次仍在 10s 有效窗口内的目标 BLE 广播 RSSI(dBm)。无当前对端返回 -127。
int radar_ble_peer_rssi(void);

// 最近一次收到的目标 BLE 广播 RSSI(dBm)，即使已经超出有效窗口也保留。
int radar_ble_peer_last_rssi(void);

// 最近 10s 内收到过目标 BLE 广播。
bool radar_ble_peer_alive(void);

// 最近一次有效对端广播距现在的毫秒数；从未收到或当前不可用返回 -1。
int radar_ble_peer_age_ms(void);

// 低频输出 BLE ready/peer/RSSI/age 状态变化，供 USB 现场诊断使用。
void radar_ble_diag_tick(void);

// 设置由 ESP-NOW v2 验证过的目标 identity；未绑定前不接受 BLE sentinel。
void radar_ble_set_expected_identity(const uint8_t identity[6]);

// 最近目标广播携带的 BLE identity。未知时返回 false。
bool radar_ble_peer_identity(uint8_t out_identity[6]);
