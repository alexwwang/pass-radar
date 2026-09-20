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
    bool calibrated;
    wifi_ftm_status_t status;
    float distance_m;       // 最近窗口 P25 距离
    float last_session_m;   // 本次 session 中值
    float spread_m;         // 本次 session IQR
    float hist_spread_m;     // 最近 session 距离范围
    uint8_t valid_bursts;
    uint8_t hist_count;
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

// 最近一次会话结束/发起失败的 wifi_ftm_status_t(0=无记录)。
// 无有效快照时用于诊断:3=NO_RESPONSE(对端未就位/被挤占)、4=REJECTED 等。
int radar_ftm_last_status(void);

// 已发起的 FTM session 数量、当前 session 是否进行中、最近快照年龄(ms)。
unsigned radar_ftm_session_count(void);
bool radar_ftm_session_busy(void);
int radar_ftm_snapshot_age_ms(void);

// 探针诊断计数（不计入常规 FTM 成功率）。
unsigned radar_ftm_probe_attempts(void);
unsigned radar_ftm_probe_successes(void);
void radar_ftm_set_offset_m(float offset_m);
float radar_ftm_offset_m(void);
void radar_ftm_set_calibrated(bool calibrated);
bool radar_ftm_is_calibrated(void);

// 最近常规 FTM session 的成功率；探针 session 不计入。
float radar_ftm_success_rate(void);

// 兼容查询接口。
bool radar_ftm_valid(void);
float radar_ftm_distance_m(void);
int radar_ftm_rssi(void);
