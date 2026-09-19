// radar_ftm.c —— Wi-Fi FTM(802.11mc) RTT 测距。
#include "radar_ftm.h"
#include "radar_ble.h"
#include "radar_espnow.h"
#include "radar_fusion.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "sdkconfig.h"

#include <math.h>
#include <string.h>

static const char *TAG = "radar_ftm";

#define FTM_AP_SSID "passradar"
#define FTM_AP_PASSWORD "passradar123"
#define FTM_AP_CHANNEL 1
#define FTM_FRAMES_PER_SESSION 16
#define FTM_PERIOD_US (2 * 1000 * 1000)
#define FTM_SESSION_TIMEOUT_US (1500 * 1000)
#define FTM_VALID_US (5 * 1000 * 1000)
#define FTM_BACKOFF_US (2 * 1000 * 1000)
#define FTM_PS_TO_M (1.49896e-4f)
#define FTM_MAX_REPORT 64

static volatile bool s_paused;
static volatile bool s_session_busy;
static bool s_initialized;
static bool s_handler_registered;
static int64_t s_last_session_us;
static int64_t s_session_deadline_us;
static int64_t s_backoff_until_us;
static radar_ftm_snapshot_t s_snapshot;
static int s_coex_kick_count;
static bool s_coex_active;
static int64_t s_coex_start_us;
static volatile int s_last_status;

int radar_ftm_last_status(void) { return s_last_status; }

#ifdef CONFIG_RADAR_SIM_BUILD
// ==== passport-sim 专用:合成 FTM 快照注入 ====
// QEMU 没有 FTM 硬件,真实会话永远失败;注入一个稳定快照,
// 让结果页/诊断页出现 "FTM ok"/"2way"/[HI] 文案,用于布局验证。
// 注:sim 快照绕过 timestamp_us 过期检查由 get_snapshot 统一处理
// (注入时打上当前时间戳,不会过期)。
static void sim_ftm_inject_snapshot(void) {
#if CONFIG_RADAR_SIM_SCENARIO != 0
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_snapshot.status = FTM_STATUS_NO_RESPONSE;
    s_snapshot.distance_m = RADAR_DIST_INVALID;
    s_last_status = FTM_STATUS_NO_RESPONSE;
    return;
#else
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_snapshot.valid = true;
    s_snapshot.calibrated = true;
    s_snapshot.status = FTM_STATUS_SUCCESS;
    s_snapshot.distance_m = 3.8f;
    s_snapshot.spread_m = 0.4f;
    s_snapshot.valid_bursts = 16;
    s_snapshot.rssi = -52;
    s_snapshot.timestamp_us = esp_timer_get_time();
    s_snapshot.channel = FTM_AP_CHANNEL;
    s_last_status = FTM_STATUS_SUCCESS;
#endif
}
#endif

// 与 BLE 扫描共存:Coded PHY 扫描窗口会挤压 FTM 会话帧。连续 initiate
// 失败时暂停扫描几秒让 FTM 建立,超时后恢复扫描(若仍失败会再次触发)。
#define FTM_COEX_KICK_LIMIT 3
#define FTM_COEX_HOLD_US (5 * 1000 * 1000)

static bool valid_mac(const uint8_t mac[6]) {
    uint8_t zero[6] = {0};
    return mac && memcmp(mac, zero, sizeof(zero)) != 0;
}

static void clear_snapshot(wifi_ftm_status_t status) {
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_snapshot.status = status;
    s_snapshot.distance_m = RADAR_DIST_INVALID;
    s_snapshot.rssi = -127;
    s_last_status = (int)status;
}

static void ftm_event_handler(void *arg, esp_event_base_t base,
                              int32_t id, void *data) {
    (void)arg;
    (void)base;
    if (id != WIFI_EVENT_FTM_REPORT || !data) return;

    const wifi_event_ftm_report_t *report = data;
    s_session_busy = false;
    s_session_deadline_us = 0;

    radar_ftm_snapshot_t next = {0};
    next.status = report->status;
    next.rssi = -127;
    next.channel = FTM_AP_CHANNEL;
    memcpy(next.target_mac, report->peer_mac, sizeof(next.target_mac));

    if (report->status == FTM_STATUS_SUCCESS &&
        report->ftm_report_num_entries > 0) {
        uint8_t count = report->ftm_report_num_entries;
        if (count > FTM_MAX_REPORT) count = FTM_MAX_REPORT;
        wifi_ftm_report_entry_t entries[FTM_MAX_REPORT];
        esp_err_t err = esp_wifi_ftm_get_report(entries, count);
        if (err == ESP_OK) {
            float raw[FTM_MAX_REPORT];
            unsigned valid = 0;
            int rssi_sum = 0;
            for (unsigned i = 0; i < count; ++i) {
                float distance = (float)entries[i].rtt * FTM_PS_TO_M;
                if (!isfinite(distance) || distance <= 0.0f || distance > 1000.0f) {
                    continue;
                }
                raw[valid++] = distance;
                rssi_sum += entries[i].rssi;
            }
            if (valid > 0) {
                next.distance_m = radar_fusion_ftm_filter(raw, valid, &next.spread_m);
                next.valid_bursts = (uint8_t)valid;
                next.rssi = rssi_sum / (int)valid;
                next.valid = next.distance_m > 0.0f && isfinite(next.distance_m);
                next.calibrated = false; // populated from NVS calibration state in the next phase
                next.timestamp_us = esp_timer_get_time();
                if (next.valid) {
                    s_snapshot = next;
                    ESP_LOGI(TAG, "FTM ok: %.2fm spread %.2f bursts %u rssi %d",
                             (double)next.distance_m, (double)next.spread_m,
                             next.valid_bursts, next.rssi);
                    return;
                }
            }
        } else {
            // The report API may leave an internal report on an error path;
            // release it through the IDF API, never with free().
            (void)esp_wifi_ftm_get_report(NULL, 0);
        }
        clear_snapshot(FTM_STATUS_NO_VALID_MSMT);
        s_snapshot.timestamp_us = esp_timer_get_time();
        return;
    }

    // The report API owns the internal report. Release it through the IDF API
    // even when the session did not produce usable measurements; never free
    // event-owned memory directly.
    (void)esp_wifi_ftm_get_report(NULL, 0);
    clear_snapshot(report->status);
    s_snapshot.timestamp_us = esp_timer_get_time();
    s_backoff_until_us = s_snapshot.timestamp_us + FTM_BACKOFF_US;
    ESP_LOGW(TAG, "FTM session status=%d", (int)report->status);
}

esp_err_t radar_ftm_init(void) {
#ifdef CONFIG_RADAR_SIM_BUILD
    // sim 下不配置 SoftAP/FTM responder(QEMU 无射频),
    // 仅标记就绪让 tick 注入合成快照。
    s_initialized = true;
    return ESP_OK;
#endif
    if (s_initialized) return ESP_OK;
    clear_snapshot(FTM_STATUS_FAIL);
    s_last_status = 0;
    s_paused = false;
    s_session_busy = false;
    s_last_session_us = 0;
    s_session_deadline_us = 0;
    s_backoff_until_us = 0;
    s_coex_kick_count = 0;
    s_coex_active = false;
    s_coex_start_us = 0;

    wifi_config_t ap = {
        .ap = {
            .ssid = FTM_AP_SSID,
            .ssid_len = sizeof(FTM_AP_SSID) - 1,
            .password = FTM_AP_PASSWORD,
            .channel = FTM_AP_CHANNEL,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .ssid_hidden = 1,
            .max_connection = 1,
            .ftm_responder = true,
        },
    };
    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (err != ESP_OK) return err;

    err = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_FTM_REPORT,
                                     ftm_event_handler, NULL);
    if (err != ESP_OK) return err;
    s_handler_registered = true;
    s_initialized = true;
    return ESP_OK;
}

void radar_ftm_deinit(void) {
    if (!s_initialized && !s_handler_registered) return;
    if (s_session_busy) (void)esp_wifi_ftm_end_session();
    if (s_handler_registered) {
        (void)esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_FTM_REPORT,
                                            ftm_event_handler);
    }
    s_handler_registered = false;
    s_session_busy = false;
    s_session_deadline_us = 0;
    s_initialized = false;
    clear_snapshot(FTM_STATUS_USER_TERM);
}

void radar_ftm_pause(bool paused) {
    s_paused = paused;
    if (paused && s_session_busy) {
        (void)esp_wifi_ftm_end_session();
        s_session_busy = false;
        s_session_deadline_us = 0;
    }
}

void radar_ftm_tick(void) {
#ifdef CONFIG_RADAR_SIM_BUILD
    sim_ftm_inject_snapshot();
    return;
#endif
    if (!s_initialized || s_paused || !radar_espnow_is_ftm_initiator()) return;
    int64_t now = esp_timer_get_time();

    // 共存保持超时:恢复扫描,允许下一轮按需再次触发。
    if (s_coex_active && now - s_coex_start_us > FTM_COEX_HOLD_US) {
        s_coex_active = false;
        radar_ble_pause(false);
        ESP_LOGI(TAG, "coex: BLE scan resumed (timeout)");
    }

    if (s_session_busy) {
        if (now >= s_session_deadline_us) {
            (void)esp_wifi_ftm_end_session();
            s_session_busy = false;
            s_backoff_until_us = now + FTM_BACKOFF_US;
            clear_snapshot(FTM_STATUS_USER_TERM);
        }
        return;
    }
    if (now < s_backoff_until_us || now - s_last_session_us < FTM_PERIOD_US ||
        radar_espnow_is_hopping()) {
        return;
    }

    uint8_t mac[RADAR_FTM_MAC_LEN];
    if (!radar_espnow_peer_ap_mac(mac) || !valid_mac(mac)) return;

    wifi_ftm_initiator_cfg_t cfg = {
        .frm_count = FTM_FRAMES_PER_SESSION,
        .burst_period = 0,
        .channel = FTM_AP_CHANNEL,
        .use_get_report_api = true,
    };
    memcpy(cfg.resp_mac, mac, sizeof(cfg.resp_mac));

    esp_err_t err = esp_wifi_ftm_initiate_session(&cfg);
    if (err == ESP_OK) {
        s_last_session_us = now;
        s_session_deadline_us = now + FTM_SESSION_TIMEOUT_US;
        s_session_busy = true;
        // 会话成功建立:恢复此前为建立会话而暂停的扫描。
        if (s_coex_active) {
            s_coex_active = false;
            radar_ble_pause(false);
            ESP_LOGI(TAG, "coex: BLE scan resumed");
        }
    } else {
        s_backoff_until_us = now + FTM_BACKOFF_US;
        s_last_status = -(int)err;  // 负值 = initiate 阶段 esp_err_t
        ESP_LOGW(TAG, "FTM initiate failed: %s", esp_err_to_name(err));
        if (++s_coex_kick_count >= FTM_COEX_KICK_LIMIT && !s_coex_active) {
            radar_ble_pause(true);
            s_coex_active = true;
            s_coex_start_us = now;
            s_coex_kick_count = 0;
            ESP_LOGW(TAG, "coex: pausing BLE scan to let FTM through");
        }
    }
}

bool radar_ftm_get_snapshot(radar_ftm_snapshot_t *out) {
    if (!out) return false;
    radar_ftm_snapshot_t copy = s_snapshot;
    if (!copy.valid || copy.timestamp_us == 0 ||
        esp_timer_get_time() - copy.timestamp_us > FTM_VALID_US) {
        copy.valid = false;
        *out = copy;
        return false;
    }
    *out = copy;
    return true;
}

bool radar_ftm_valid(void) {
    radar_ftm_snapshot_t snapshot;
    return radar_ftm_get_snapshot(&snapshot);
}

float radar_ftm_distance_m(void) {
    radar_ftm_snapshot_t snapshot;
    return radar_ftm_get_snapshot(&snapshot) ? snapshot.distance_m : RADAR_DIST_INVALID;
}

int radar_ftm_rssi(void) {
    radar_ftm_snapshot_t snapshot;
    return radar_ftm_get_snapshot(&snapshot) ? snapshot.rssi : -127;
}
