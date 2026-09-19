// radar_ble_sim.c —— RADAR_SIM_BUILD 专用桩。
// QEMU-wasm 没有 BLE 控制器模型,nimble_port_init() 会卡死/失败;
// 用空实现替换真栈,配合 radar_espnow.c 的合成对端注入 BLE RSSI。
// 由 main/CMakeLists.txt 按 CONFIG_RADAR_SIM_BUILD 条件编入。

#include "radar_ble.h"

#include <string.h>

static int s_sim_rssi = -127;

esp_err_t radar_ble_init(void) { return ESP_OK; }
void radar_ble_deinit(void) {}
void radar_ble_pause(bool paused) { (void)paused; }
bool radar_ble_ready(void) { return true; }

int radar_ble_peer_rssi(void) { return s_sim_rssi; }

bool radar_ble_peer_alive(void) { return s_sim_rssi > -127; }

void radar_ble_set_expected_identity(const uint8_t identity[6]) {
    (void)identity;
}

bool radar_ble_peer_identity(uint8_t out_identity[6]) {
    if (!out_identity) return false;
    // 与 radar_espnow.c 合成对端使用的 identity 保持一致。
    memset(out_identity, 0xAB, 6);
    return true;
}
