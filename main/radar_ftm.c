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
#define FTM_HISTORY 5
#define FTM_SUCCESS_WINDOW 10
#define FTM_MIN_PEER_RSSI (-72)
#define FTM_PROBE_MIN_RSSI (-82)
#define FTM_PROBE_PERIOD_US (5 * 1000 * 1000)
// ESP-IDF accepts 0, 8, 16, 24, 32 or 64 frames; 8 is the smallest
// useful probe and avoids the invalid 4-frame request used previously.
#define FTM_PROBE_FRAMES 8

static volatile bool s_paused;
static volatile bool s_session_busy;
static bool s_initialized;
static bool s_handler_registered;
// ESP32-C3 firmware may expose FTM responder but not initiator support.
// Once the driver reports NOT_SUPPORTED, stop retrying for this boot and
// let the fusion layer use RSSI without repeatedly disturbing BLE coexistence.
static bool s_initiator_unsupported;
static int64_t s_last_session_us;
static int64_t s_session_deadline_us;
static int64_t s_backoff_until_us;
static radar_ftm_snapshot_t s_snapshot;
static int s_coex_kick_count;
static bool s_coex_active;
static int64_t s_coex_start_us;
static volatile int s_last_status;
static bool s_probe_session;
static int64_t s_last_probe_us;
static unsigned s_session_count;
static unsigned s_probe_attempts;
static unsigned s_probe_successes;
static float s_offset_m;
static bool s_calibrated;
static float s_history[FTM_HISTORY];
static unsigned s_history_count;
static unsigned s_history_head;
static bool s_success_history[FTM_SUCCESS_WINDOW];
static unsigned s_success_count;
static unsigned s_success_head;

// WIFI_EVENT_FTM_REPORT runs on ESP-IDF's sys_evt task. Keep the report
// buffers out of that task's stack: 64 entries are already about 2.5 KiB on
// ESP32-C3, before the raw-distance buffer and event-handler call chain.
static wifi_ftm_report_entry_t s_report_entries[FTM_MAX_REPORT];
static float s_raw_distances[FTM_MAX_REPORT];
static float s_history_scratch[FTM_HISTORY];

static void record_regular_session(bool success) {
    s_success_history[s_success_head] = success;
    s_success_head = (s_success_head + 1u) % FTM_SUCCESS_WINDOW;
    if (s_success_count < FTM_SUCCESS_WINDOW) s_success_count++;
}

static void record_distance(float distance_m) {
    s_history[s_history_head] = distance_m;
    s_history_head = (s_history_head + 1u) % FTM_HISTORY;
    if (s_history_count < FTM_HISTORY) s_history_count++;
}

int radar_ftm_last_status(void) { return s_last_status; }
unsigned radar_ftm_session_count(void) { return s_session_count; }
bool radar_ftm_session_busy(void) { return s_session_busy; }
int radar_ftm_snapshot_age_ms(void) {
    if (s_snapshot.timestamp_us == 0) return -1;
    int64_t age_us = esp_timer_get_time() - s_snapshot.timestamp_us;
    return age_us < 0 ? -1 : (int)(age_us / 1000);
}
unsigned radar_ftm_probe_attempts(void) { return s_probe_attempts; }
unsigned radar_ftm_probe_successes(void) { return s_probe_successes; }
void radar_ftm_set_offset_m(float offset_m) {
    if (isfinite(offset_m) && offset_m >= -2.0f && offset_m <= 15.0f) s_offset_m = offset_m;
}
float radar_ftm_offset_m(void) { return s_offset_m; }

void radar_ftm_set_calibrated(bool calibrated) { s_calibrated = calibrated; }
bool radar_ftm_is_calibrated(void) { return s_calibrated; }

float radar_ftm_success_rate(void) {
    if (s_success_count == 0) return NAN;
    unsigned ok = 0;
    for (unsigned i = 0; i < s_success_count; ++i) {
        if (s_success_history[i]) ok++;
    }
    return (float)ok / (float)s_success_count;
}


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
    radar_espnow_ftm_session_finished();

    radar_ftm_snapshot_t next = {0};
    next.status = report->status;
    next.rssi = -127;
    next.channel = FTM_AP_CHANNEL;
    memcpy(next.target_mac, report->peer_mac, sizeof(next.target_mac));

    if (report->status == FTM_STATUS_SUCCESS &&
        report->ftm_report_num_entries > 0) {
        uint8_t count = report->ftm_report_num_entries;
        if (count > FTM_MAX_REPORT) count = FTM_MAX_REPORT;
        esp_err_t err = esp_wifi_ftm_get_report(s_report_entries, count);
        if (err == ESP_OK) {
            unsigned valid = 0;
            int rssi_sum = 0;
            for (unsigned i = 0; i < count; ++i) {
                float distance = (float)s_report_entries[i].rtt * FTM_PS_TO_M;
                if (!isfinite(distance) || distance <= 0.0f || distance > 1000.0f) {
                    continue;
                }
                s_raw_distances[valid++] = distance;
                rssi_sum += s_report_entries[i].rssi;
            }
            if (valid > 0) {
                next.distance_m = radar_fusion_ftm_filter(s_raw_distances, valid, &next.spread_m);
                next.distance_m = radar_fusion_ftm_apply_offset(next.distance_m, s_offset_m);
                next.valid_bursts = (uint8_t)valid;
                next.rssi = rssi_sum / (int)valid;
                next.valid = next.distance_m > 0.0f && isfinite(next.distance_m);
                next.last_session_m = next.distance_m;
                next.calibrated = s_calibrated;
                next.timestamp_us = esp_timer_get_time();
                if (s_probe_session) {
                    s_probe_successes++;
                } else {
                    record_regular_session(next.valid);
                }
                if (next.valid) {
                    record_distance(next.distance_m);
                    unsigned history_count = s_history_count;
                    unsigned start = (s_history_head + FTM_HISTORY - history_count) % FTM_HISTORY;
                    for (unsigned j = 0; j < history_count; ++j) {
                        s_history_scratch[j] = s_history[(start + j) % FTM_HISTORY];
                    }
                    next.distance_m = radar_fusion_ftm_low_quantile(s_history_scratch,
                                                                    history_count);
                    next.hist_count = (uint8_t)history_count;
                    float min_d = s_history_scratch[0], max_d = s_history_scratch[0];
                    for (unsigned j = 1; j < history_count; ++j) {
                        if (s_history_scratch[j] < min_d) min_d = s_history_scratch[j];
                        if (s_history_scratch[j] > max_d) max_d = s_history_scratch[j];
                    }
                    next.hist_spread_m = max_d - min_d;
                    s_last_status = (int)FTM_STATUS_SUCCESS;
                    s_snapshot = next;
                    s_probe_session = false;
                    ESP_LOGI(TAG, "FTM_SESSION end=%u result=OK distance=%.2f spread=%.2f bursts=%u rssi=%d",
                             s_session_count, (double)next.distance_m, (double)next.spread_m,
                             next.valid_bursts, next.rssi);
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
        if (!s_probe_session) record_regular_session(false);
        s_probe_session = false;
        clear_snapshot(FTM_STATUS_NO_VALID_MSMT);
        s_snapshot.timestamp_us = esp_timer_get_time();
        ESP_LOGW(TAG, "FTM_SESSION end=%u result=FAIL status=%d valid=0",
                 s_session_count, (int)FTM_STATUS_NO_VALID_MSMT);
        return;
    }

    // The report API owns the internal report. Release it through the IDF API
    // even when the session did not produce usable measurements; never free
    // event-owned memory directly.
    (void)esp_wifi_ftm_get_report(NULL, 0);
    if (!s_probe_session) record_regular_session(false);
    clear_snapshot(report->status);
    s_probe_session = false;
    s_snapshot.timestamp_us = esp_timer_get_time();
    s_backoff_until_us = s_snapshot.timestamp_us + FTM_BACKOFF_US;
    ESP_LOGW(TAG, "FTM_SESSION end=%u result=FAIL status=%d",
             s_session_count, (int)report->status);
    ESP_LOGW(TAG, "FTM session status=%d", (int)report->status);
}

esp_err_t radar_ftm_init(void) {
    if (s_initialized) return ESP_OK;
    clear_snapshot(FTM_STATUS_FAIL);
    s_last_status = 0;
    s_paused = false;
    s_session_busy = false;
    s_last_session_us = 0;
    s_session_deadline_us = 0;
    s_backoff_until_us = 0;
    s_session_count = 0;
    s_coex_kick_count = 0;
    s_coex_active = false;
    s_initiator_unsupported = false;
    s_coex_start_us = 0;
    s_calibrated = false;
    s_history_count = 0;
    s_history_head = 0;
    s_success_count = 0;
    s_success_head = 0;
    memset(s_history, 0, sizeof(s_history));
    memset(s_success_history, 0, sizeof(s_success_history));

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
    s_probe_session = false;
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
    if (!s_initialized || s_paused || s_initiator_unsupported ||
        !radar_espnow_ftm_can_initiate()) return;
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
            radar_espnow_ftm_session_finished();
            s_backoff_until_us = now + FTM_BACKOFF_US;
            clear_snapshot(FTM_STATUS_USER_TERM);
            if (!s_probe_session) record_regular_session(false);
            s_probe_session = false;
            ESP_LOGW(TAG, "FTM_SESSION end=%u result=TIMEOUT status=%d",
                     s_session_count, (int)FTM_STATUS_USER_TERM);
        }
        return;
    }
    if (now < s_backoff_until_us || now - s_last_session_us < FTM_PERIOD_US ||
        radar_espnow_is_hopping()) {
        return;
    }

    uint8_t mac[RADAR_FTM_MAC_LEN];
    if (!radar_espnow_peer_ap_mac(mac) || !valid_mac(mac)) return;

    int peer_rssi = radar_espnow_peer_rssi();
    bool regular = peer_rssi >= FTM_MIN_PEER_RSSI;
    bool probe = !regular && peer_rssi >= FTM_PROBE_MIN_RSSI &&
                 now - s_last_probe_us >= FTM_PROBE_PERIOD_US;
    if (!regular && !probe) return;

    wifi_ftm_initiator_cfg_t cfg = {
        .frm_count = probe ? FTM_PROBE_FRAMES : FTM_FRAMES_PER_SESSION,
        .burst_period = 0,
        .channel = radar_espnow_peer_channel(),
        .use_get_report_api = true,
    };
    memcpy(cfg.resp_mac, mac, sizeof(cfg.resp_mac));

    esp_err_t err = esp_wifi_ftm_initiate_session(&cfg);
    if (err == ESP_OK) {
        s_session_count++;
        s_probe_session = probe;
        ESP_LOGI(TAG, "FTM_SESSION start=%u role=%s probe=%u rssi=%d channel=%u",
                 s_session_count,
                 radar_espnow_is_ftm_initiator() ? "INITIATOR" : "RESPONDER",
                 probe, peer_rssi, cfg.channel);
        if (probe) {
            s_last_probe_us = now;
            s_probe_attempts++;
        }
        s_last_session_us = now;
        s_session_deadline_us = now + FTM_SESSION_TIMEOUT_US;
        s_session_busy = true;
        radar_espnow_ftm_session_started();
        // 会话成功建立:恢复此前为建立会话而暂停的扫描。
        if (s_coex_active) {
            s_coex_active = false;
            radar_ble_pause(false);
            ESP_LOGI(TAG, "coex: BLE scan resumed");
        }
    } else {
        if (!probe) record_regular_session(false);
        s_probe_session = false;
        s_backoff_until_us = now + FTM_BACKOFF_US;
        s_last_status = -(int)err;  // 负值 = initiate 阶段 esp_err_t
        if (err == ESP_ERR_NOT_SUPPORTED) {
            // This is a capability result, not a transient radio failure.
            // Do not retry forever or repeatedly pause BLE scanning.
            s_initiator_unsupported = true;
            if (s_coex_active) {
                s_coex_active = false;
                radar_ble_pause(false);
            }
            ESP_LOGW(TAG, "FTM initiator unsupported; disabling FTM for this boot, using RSSI");
            return;
        }
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
