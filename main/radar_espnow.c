// radar_espnow.c —— ESP-NOW 广播接收 + RSSI 采集 + 对端状态交换(v2)。
#include "radar_espnow.h"
#include "radar_ble.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "nvs.h"

#include <stddef.h>
#include <string.h>

static const char *TAG = "radar_espnow";

#define RADAR_HOME_CHANNEL 1
#define PEER_TIMEOUT_US (1000 * 1000)
#define PEER_SWEEP_HOLD_US (300 * 1000)
#define RADAR_NVS_NS "radar"
#define RADAR_NVS_KEY "txref"

static volatile float s_filtered_rssi = -90.0f;
static volatile int s_raw_rssi = -90;
static volatile bool s_sweeping;
static volatile bool s_channel_fault;

static radar_sample_t s_samples[RADAR_MAX_SAMPLES];
static volatile unsigned s_sample_count;
static int64_t s_sweep_start_us;
static uint32_t s_sweep_id;

static volatile int s_peer_rssi = -127;
static volatile int64_t s_peer_seen_us;
static volatile int64_t s_peer_sweep_us;
static volatile float s_peer_angle = -1.0f;
static volatile float s_peer_dist = -1.0f;
static uint8_t s_peer_src_mac[RADAR_MAC_LEN];
static uint8_t s_peer_ap_mac[RADAR_MAC_LEN];
static uint8_t s_peer_ble_identity[RADAR_BLE_ID_LEN];
static volatile bool s_peer_mac_known;
static volatile bool s_peer_ap_mac_known;
static volatile bool s_peer_ble_identity_known;
static uint32_t s_peer_sequence;
static bool s_peer_sequence_known;
static uint32_t s_peer_sweep_id;
static uint32_t s_peer_sweep_elapsed_ms;
static uint8_t s_peer_hop_index;
static uint8_t s_peer_channel;
static bool s_peer_sweep_known;

static uint8_t s_sta_mac[RADAR_MAC_LEN];
static uint8_t s_ap_mac[RADAR_MAC_LEN];
static uint8_t s_ble_identity[RADAR_BLE_ID_LEN];
static bool s_ftm_initiator;
static bool s_role_known;

static volatile float s_report_angle = -1.0f;
static volatile float s_report_dist = -1.0f;
static volatile uint8_t s_cur_channel = RADAR_HOME_CHANNEL;
static int64_t s_hop_start_us;
static radar_kalman_t s_kalman;

static bool s_nvs_ready;
static bool s_netif_ready;
static bool s_event_loop_ready;
static bool s_initialized;

static esp_timer_handle_t s_broadcast_timer;
static radar_packet_t s_tx_packet;
static volatile bool s_broadcasting;
static const uint8_t BROADCAST_MAC[RADAR_MAC_LEN] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

static bool valid_mac(const uint8_t mac[RADAR_MAC_LEN]) {
    return radar_protocol_valid_mac(mac);
}

static bool seq_is_newer(uint32_t seq, uint32_t previous) {
    return radar_protocol_seq_newer(seq, previous);
}

static bool valid_rssi(int rssi) { return radar_protocol_valid_rssi(rssi); }

static bool valid_angle10(int16_t angle) {
    return radar_protocol_valid_angle10(angle);
}

static bool valid_distance_dm(int16_t distance) {
    return radar_protocol_valid_distance_dm(distance);
}

static void update_role(void) {
    if (!s_peer_ap_mac_known || !valid_mac(s_ap_mac)) {
        s_role_known = false;
        return;
    }
    // Stable deterministic role: lower SoftAP MAC is the sole initiator.
    s_ftm_initiator = memcmp(s_ap_mac, s_peer_ap_mac, RADAR_MAC_LEN) < 0;
    s_role_known = true;
}

static void parse_v2_packet(const radar_packet_t *pkt, int64_t now) {
    if (!radar_protocol_valid_v2(pkt, sizeof(*pkt))) {
        return;
    }
    if (s_peer_sequence_known && !seq_is_newer(pkt->sequence, s_peer_sequence)) {
        return;
    }
    if (s_peer_ap_mac_known &&
        memcmp(s_peer_ap_mac, pkt->ap_mac, RADAR_MAC_LEN) != 0) {
        // A stable peer must not silently change its FTM/BLE identity.
        return;
    }

    memcpy(s_peer_ap_mac, pkt->ap_mac, RADAR_MAC_LEN);
    s_peer_ap_mac_known = true;
    if ((pkt->flags & RADAR_PKT_FLAG_VALID_BLE_ID) != 0) {
        memcpy(s_peer_ble_identity, pkt->ble_identity, RADAR_BLE_ID_LEN);
        s_peer_ble_identity_known = true;
    }
    s_peer_rssi = pkt->rssi_to_peer;
    s_peer_angle = pkt->angle_deg10 >= 0 ? (float)pkt->angle_deg10 / 10.0f : -1.0f;
    s_peer_dist = pkt->dist_dm >= 0 ? (float)pkt->dist_dm / 10.0f : -1.0f;
    s_peer_sequence = pkt->sequence;
    s_peer_sequence_known = true;
    update_role();

    if ((pkt->flags & RADAR_PKT_FLAG_SWEEPING) != 0) {
        if (!s_peer_sweep_known || pkt->sweep_id != s_peer_sweep_id) {
            s_peer_sweep_id = pkt->sweep_id;
            s_peer_sweep_known = true;
        }
        s_peer_sweep_elapsed_ms = pkt->sweep_elapsed_ms;
        s_peer_hop_index = pkt->hop_index;
        s_peer_channel = pkt->channel;
        s_peer_sweep_us = now;
    } else if (s_peer_sweep_known && now - s_peer_sweep_us >= PEER_SWEEP_HOLD_US) {
        s_peer_sweep_known = false;
        s_peer_sweep_us = 0;
    }
}

static void espnow_recv_cb(const esp_now_recv_info_t *recv_info,
                           const uint8_t *data, int len) {
    if (!recv_info || !recv_info->rx_ctrl || !data || len <= 0) return;
    int raw = recv_info->rx_ctrl->rssi;
    if (!valid_rssi(raw)) return;
    s_raw_rssi = raw;
    s_filtered_rssi = radar_kalman_update(&s_kalman, (float)raw);
    int64_t now = esp_timer_get_time();

    if (s_sweeping && !s_channel_fault && s_sample_count < RADAR_MAX_SAMPLES) {
        s_samples[s_sample_count].timestamp_ms = (uint32_t)((now - s_sweep_start_us) / 1000);
        s_samples[s_sample_count].rssi = s_filtered_rssi;
        s_samples[s_sample_count].channel = s_cur_channel;
        s_sample_count++;
    }

    const int v1_prefix = (int)offsetof(radar_packet_t, sequence) + (int)sizeof(uint32_t);
    if (len < v1_prefix) return;

    // Source MAC is tracked separately from the advertised SoftAP MAC.
    if (!s_peer_mac_known) {
        memcpy(s_peer_src_mac, recv_info->src_addr, RADAR_MAC_LEN);
        s_peer_mac_known = valid_mac(s_peer_src_mac);
    } else if (memcmp(s_peer_src_mac, recv_info->src_addr, RADAR_MAC_LEN) != 0) {
        // Do not allow a new sender to replace an established peer silently.
        return;
    }

    if (len < (int)sizeof(radar_packet_t)) {
        // v1 is read-only compatibility: mark liveness only, do not use v1
        // RSSI/angle/distance as v2 peer data.
        s_peer_seen_us = now;
        return;
    }
    parse_v2_packet((const radar_packet_t *)data, now);
    if (s_peer_sequence_known) s_peer_seen_us = now;
}

static esp_err_t nvs_prepare(void) {
    if (s_nvs_ready) return ESP_OK;
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) return err;
    s_nvs_ready = true;
    return ESP_OK;
}

static esp_err_t network_prepare(void) {
    if (!s_netif_ready) {
        esp_err_t err = esp_netif_init();
        if (err != ESP_OK) return err;
        s_netif_ready = true;
    }
    if (!s_event_loop_ready) {
        esp_err_t err = esp_event_loop_create_default();
        if (err != ESP_OK) return err;
        s_event_loop_ready = true;
    }
    return ESP_OK;
}

esp_err_t radar_espnow_init(void) {
    if (s_initialized) return ESP_OK;
    esp_err_t err = nvs_prepare();
    if (err != ESP_OK) return err;
    err = network_prepare();
    if (err != ESP_OK) return err;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) return err;
    err = esp_wifi_start();
    if (err != ESP_OK) return err;
    err = esp_wifi_get_mac(WIFI_IF_STA, s_sta_mac);
    if (err != ESP_OK) return err;
    err = esp_wifi_get_mac(WIFI_IF_AP, s_ap_mac);
    if (err != ESP_OK) return err;
    memcpy(s_ble_identity, s_ap_mac, RADAR_BLE_ID_LEN);
    err = esp_wifi_set_channel(RADAR_HOME_CHANNEL, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) return err;

    err = esp_now_init();
    if (err != ESP_OK) return err;
    err = esp_now_register_recv_cb(espnow_recv_cb);
    if (err != ESP_OK) return err;
    radar_kalman_init(&s_kalman, -70.0f);
    s_cur_channel = RADAR_HOME_CHANNEL;
    s_initialized = true;
    ESP_LOGI(TAG, "ESP-NOW ready STA=%02x:%02x:%02x:%02x:%02x:%02x AP=%02x:%02x:%02x:%02x:%02x:%02x",
             s_sta_mac[0], s_sta_mac[1], s_sta_mac[2], s_sta_mac[3], s_sta_mac[4], s_sta_mac[5],
             s_ap_mac[0], s_ap_mac[1], s_ap_mac[2], s_ap_mac[3], s_ap_mac[4], s_ap_mac[5]);
    return ESP_OK;
}

static void hop_tick(int64_t now) {
    uint8_t desired = RADAR_HOME_CHANNEL;
    if (s_sweeping) {
        uint32_t elapsed = (uint32_t)((now - s_sweep_start_us) / 1000);
        desired = radar_sweep_channel_at(elapsed, RADAR_SWEEP_DURATION_MS);
    } else if (s_peer_sweep_known && now - s_peer_sweep_us < PEER_SWEEP_HOLD_US) {
        uint32_t elapsed = s_peer_sweep_elapsed_ms +
            (uint32_t)((now - s_peer_sweep_us) / 1000);
        if (elapsed < RADAR_SWEEP_DURATION_MS) {
            desired = radar_sweep_channel_at(elapsed, RADAR_SWEEP_DURATION_MS);
        } else {
            s_peer_sweep_known = false;
            s_peer_sweep_us = 0;
        }
    }
    if (desired != s_cur_channel) {
        esp_err_t err = esp_wifi_set_channel(desired, WIFI_SECOND_CHAN_NONE);
        if (err == ESP_OK) {
            s_cur_channel = desired;
            s_channel_fault = false;
        } else {
            s_channel_fault = true;
            ESP_LOGW(TAG, "channel switch %u->%u failed: %s", s_cur_channel, desired,
                     esp_err_to_name(err));
        }
    }
}

static void broadcast_tick(void *arg) {
    (void)arg;
    int64_t now = esp_timer_get_time();
    hop_tick(now);
    s_tx_packet.sequence++;
    s_tx_packet.proto_ver = RADAR_PROTO_VER;
    s_tx_packet.payload_len = sizeof(radar_packet_t);
    s_tx_packet.flags = RADAR_PKT_FLAG_VALID_AP_MAC | RADAR_PKT_FLAG_VALID_BLE_ID;
    if (s_sweeping) s_tx_packet.flags |= RADAR_PKT_FLAG_SWEEPING;
    if (s_ftm_initiator) s_tx_packet.flags |= RADAR_PKT_FLAG_FTM_INITIATOR;
    float fr = s_filtered_rssi;
    s_tx_packet.rssi_to_peer = fr < -127.0f ? -127 : (fr > 0.0f ? 0 : (int8_t)fr);
    float ra = s_report_angle;
    float rd = s_report_dist;
    s_tx_packet.angle_deg10 = ra >= 0.0f && ra <= 360.0f ? (int16_t)(ra * 10.0f + 0.5f) : -1;
    s_tx_packet.dist_dm = rd >= 0.0f && rd <= 1000.0f ? (int16_t)(rd * 10.0f + 0.5f) : -1;
    memcpy(s_tx_packet.ap_mac, s_ap_mac, RADAR_MAC_LEN);
    memcpy(s_tx_packet.ble_identity, s_ble_identity, RADAR_BLE_ID_LEN);
    s_tx_packet.sweep_id = s_sweep_id;
    s_tx_packet.sweep_elapsed_ms = s_sweeping ? (uint32_t)((now - s_sweep_start_us) / 1000) : 0;
    s_tx_packet.hop_index = (uint8_t)(s_tx_packet.sweep_elapsed_ms / RADAR_SWEEP_HOP_DWELL_MS);
    s_tx_packet.channel = s_cur_channel;
    (void)esp_now_send(BROADCAST_MAC, (const uint8_t *)&s_tx_packet, sizeof(s_tx_packet));
}

esp_err_t radar_espnow_start_broadcast(const char *device_name) {
    if (!s_initialized || !device_name) return ESP_ERR_INVALID_STATE;
    if (s_broadcasting) return ESP_OK;

    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, BROADCAST_MAC, RADAR_MAC_LEN);
    peer.channel = 0;
    peer.ifidx = WIFI_IF_AP;
    peer.encrypt = false;
    esp_err_t err = esp_now_add_peer(&peer);
    if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST) return err;

    memset(&s_tx_packet, 0, sizeof(s_tx_packet));
    strncpy(s_tx_packet.device_name, device_name, RADAR_PACKET_NAME_LEN - 1);
    s_tx_packet.proto_ver = RADAR_PROTO_VER;
    s_tx_packet.payload_len = sizeof(radar_packet_t);
    s_tx_packet.angle_deg10 = -1;
    s_tx_packet.dist_dm = -1;

    esp_timer_create_args_t timer_args = {.callback = broadcast_tick, .name = "radar_tx"};
    err = esp_timer_create(&timer_args, &s_broadcast_timer);
    if (err != ESP_OK) return err;
    err = esp_timer_start_periodic(s_broadcast_timer, 50000);
    if (err != ESP_OK) {
        (void)esp_timer_delete(s_broadcast_timer);
        s_broadcast_timer = NULL;
        return err;
    }
    s_broadcasting = true;
    return ESP_OK;
}

void radar_espnow_stop_broadcast(void) {
    if (!s_broadcasting) return;
    (void)esp_timer_stop(s_broadcast_timer);
    (void)esp_timer_delete(s_broadcast_timer);
    s_broadcast_timer = NULL;
    s_broadcasting = false;
    (void)esp_now_del_peer(BROADCAST_MAC);
}

void radar_espnow_deinit(void) {
    radar_espnow_stop_broadcast();
    if (!s_initialized) return;
    (void)esp_now_unregister_recv_cb();
    (void)esp_now_deinit();
    (void)esp_wifi_stop();
    (void)esp_wifi_deinit();
    s_initialized = false;
}

float radar_espnow_filtered_rssi(void) { return s_filtered_rssi; }
int radar_espnow_raw_rssi(void) { return s_raw_rssi; }
bool radar_espnow_is_sweeping(void) { return s_sweeping; }

bool radar_espnow_peer_alive(void) {
    return s_peer_seen_us != 0 && esp_timer_get_time() - s_peer_seen_us < PEER_TIMEOUT_US;
}

int radar_espnow_peer_rssi(void) { return radar_espnow_peer_alive() ? s_peer_rssi : -127; }
float radar_espnow_peer_angle(void) { return radar_espnow_peer_alive() ? s_peer_angle : -1.0f; }
float radar_espnow_peer_distance(void) { return radar_espnow_peer_alive() ? s_peer_dist : -1.0f; }

void radar_espnow_report_result(float angle_deg, float dist_m) {
    s_report_angle = angle_deg;
    s_report_dist = dist_m;
}

bool radar_espnow_peer_mac(uint8_t out_mac[RADAR_MAC_LEN]) {
    if (!out_mac || !s_peer_mac_known) return false;
    memcpy(out_mac, s_peer_src_mac, RADAR_MAC_LEN);
    return true;
}

bool radar_espnow_peer_ap_mac(uint8_t out_mac[RADAR_MAC_LEN]) {
    if (!out_mac || !s_peer_ap_mac_known || !radar_espnow_peer_alive()) return false;
    memcpy(out_mac, s_peer_ap_mac, RADAR_MAC_LEN);
    return true;
}

bool radar_espnow_peer_ble_identity(uint8_t out_identity[RADAR_BLE_ID_LEN]) {
    if (!out_identity || !s_peer_ble_identity_known || !radar_espnow_peer_alive()) return false;
    memcpy(out_identity, s_peer_ble_identity, RADAR_BLE_ID_LEN);
    return true;
}

bool radar_espnow_is_ftm_initiator(void) { return s_role_known && s_ftm_initiator; }

bool radar_espnow_is_hopping(void) {
    return s_sweeping || (s_peer_sweep_known &&
                          esp_timer_get_time() - s_peer_sweep_us < PEER_SWEEP_HOLD_US);
}

bool radar_espnow_peer_sweep(uint32_t *out_sweep_id, uint32_t *out_elapsed_ms) {
    if (!radar_espnow_peer_alive() || !s_peer_sweep_known) return false;
    if (out_sweep_id) *out_sweep_id = s_peer_sweep_id;
    if (out_elapsed_ms) *out_elapsed_ms = s_peer_sweep_elapsed_ms;
    return true;
}

void radar_espnow_start_sweep(void) {
    s_sample_count = 0;
    s_channel_fault = false;
    s_sweep_start_us = esp_timer_get_time();
    s_hop_start_us = s_sweep_start_us;
    s_sweep_id++;
    s_sweeping = true;
}

unsigned radar_espnow_stop_sweep(void) {
    s_sweeping = false;
    if (s_cur_channel != RADAR_HOME_CHANNEL) {
        esp_err_t err = esp_wifi_set_channel(RADAR_HOME_CHANNEL, WIFI_SECOND_CHAN_NONE);
        if (err == ESP_OK) {
            s_cur_channel = RADAR_HOME_CHANNEL;
            s_channel_fault = false;
        } else {
            s_channel_fault = true;
            ESP_LOGW(TAG, "home channel restore failed: %s", esp_err_to_name(err));
        }
    }
    return s_sample_count;
}

unsigned radar_espnow_get_samples(radar_sample_t *out, unsigned max_count) {
    if (!out || max_count == 0) return 0;
    unsigned n = s_sample_count;
    if (n > max_count) n = max_count;
    memcpy(out, s_samples, n * sizeof(radar_sample_t));
    return n;
}

esp_err_t radar_espnow_calib_save(float tx_ref_sym) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(RADAR_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, RADAR_NVS_KEY, &tx_ref_sym, sizeof(tx_ref_sym));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t radar_espnow_calib_load(float *tx_ref_sym) {
    if (!tx_ref_sym) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(RADAR_NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    size_t len = sizeof(*tx_ref_sym);
    err = nvs_get_blob(h, RADAR_NVS_KEY, tx_ref_sym, &len);
    nvs_close(h);
    return err;
}
