#pragma once

// Wi-Fi FTM(802.11mc RTT)测距模块。
// FTM 结果以带质量信息的快照对外提供；调用方不得直接访问 Wi-Fi 事件数据。

#include "esp_err.h"
#include "esp_wifi_types.h"
#include <stdbool.h>
#include <stdint.h>

#define RADAR_FTM_MAC_LEN 6

typedef struct {
    bool valid;
    wifi_ftm_status_t status;
    float distance_m;
    float spread_m;
    uint8_t valid_bursts;
    int rssi;
    int64_t timestamp_us;
    uint8_t target_mac[RADAR_FTM_MAC_LEN];
    uint8_t channel;
} radar_ftm_snapshot_t;

// 初始化:配置 SoftAP(FTM responder)并注册 FTM 事件。
// 需在 radar_espnow_init() 之后调用。
esp_err_t radar_ftm_init(void);

// 停止当前 session、注销事件并释放模块状态。可重复调用。
void radar_ftm_deinit(void);

// 扫盘期间暂停/恢复 FTM 发起(跳频会打断 session)。
void radar_ftm_pause(bool paused);

// 周期驱动(由 100ms 轮询调用;内部节流到每 2s 发起一次)。
void radar_ftm_tick(void);

// 获取完整快照；返回 false 表示无快照或快照已过期。
bool radar_ftm_get_snapshot(radar_ftm_snapshot_t *out);

// 兼容查询接口。
bool radar_ftm_valid(void);
float radar_ftm_distance_m(void);
int radar_ftm_rssi(void);
