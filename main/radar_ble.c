// radar_ble.c —— BLE Coded PHY 长距哨兵链路(NimBLE)。
#include "radar_ble.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_hs_id.h"
#include "host/ble_gap.h"
#include "host/util/util.h"
#include "os/os_mbuf.h"

#include <string.h>

static const char *TAG = "radar_ble";

#define BLE_ADV_NAME          "passradar"
#define BLE_ADV_INTERVAL_MS   1000
#define BLE_SCAN_ITVL_MS      300
#define BLE_SCAN_WIN_MS       30
#define BLE_PEER_HOLD_US      (10 * 1000 * 1000)
#define BLE_IDENTITY_LEN      6
#define BLE_SENTINEL_COMPANY  0xFFFF
#define BLE_SENTINEL_VERSION  1

// Manufacturer payload: company(LE), product, version, identity.
#define BLE_SENTINEL_DATA_LEN (2 + 1 + 1 + BLE_IDENTITY_LEN)

static volatile int      s_ble_rssi = -127;
static volatile int64_t  s_ble_seen_us;
static uint8_t           s_own_addr_type;
static uint8_t           s_own_identity[BLE_IDENTITY_LEN];
static uint8_t           s_peer_identity[BLE_IDENTITY_LEN];
static uint8_t           s_expected_identity[BLE_IDENTITY_LEN];
static volatile bool     s_expected_identity_known;
static volatile bool     s_peer_identity_known;
static volatile bool     s_ready;
static volatile bool     s_sync_seen;
static volatile bool     s_scan_paused;
static volatile bool     s_adv_started;
static volatile bool     s_scan_started;
static bool              s_initialized;
static bool              s_diag_logged;
static bool              s_diag_last_ready;
static bool              s_diag_last_peer;
static int               s_diag_last_rssi = -127;
static int               s_diag_last_age_s = -1;
static bool              s_diag_last_expected;
static int64_t           s_diag_last_candidate_us;

static bool is_valid_rssi(int8_t rssi) {
    // NimBLE uses 127 when RSSI is unavailable.
    return rssi != 127 && rssi >= -127 && rssi <= 20;
}

static bool identity_is_self(const uint8_t *identity) {
    return memcmp(identity, s_own_identity, BLE_IDENTITY_LEN) == 0;
}

// ---------- 广播 ----------

static int adv_event_cb(struct ble_gap_event *event, void *arg) {
    (void)event;
    (void)arg;
    return 0;
}

static int start_advertising(void) {
    struct ble_gap_ext_adv_params params = {0};
    params.own_addr_type = s_own_addr_type;
    params.primary_phy = BLE_HCI_LE_PHY_CODED;
    params.secondary_phy = BLE_HCI_LE_PHY_CODED;
    params.sid = 1;
    params.itvl_min = BLE_GAP_ADV_ITVL_MS(BLE_ADV_INTERVAL_MS);
    params.itvl_max = BLE_GAP_ADV_ITVL_MS(BLE_ADV_INTERVAL_MS + 200);
    params.filter_policy = BLE_HCI_ADV_FILT_NONE;

    int rc = ble_gap_ext_adv_configure(0, &params, NULL, adv_event_cb, NULL);
    if (rc != 0) return rc;

    uint8_t mfg[BLE_SENTINEL_DATA_LEN] = {
        (uint8_t)(BLE_SENTINEL_COMPANY & 0xff),
        (uint8_t)(BLE_SENTINEL_COMPANY >> 8),
        'R',
        BLE_SENTINEL_VERSION,
    };
    memcpy(&mfg[4], s_own_identity, BLE_IDENTITY_LEN);

    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (const uint8_t *)BLE_ADV_NAME;
    fields.name_len = sizeof(BLE_ADV_NAME) - 1;
    fields.name_is_complete = 1;
    fields.mfg_data = mfg;
    fields.mfg_data_len = sizeof(mfg);

    struct os_mbuf *data = os_msys_get_pkthdr(0, 0);
    if (!data) return BLE_HS_ENOMEM;

    rc = ble_hs_adv_set_fields_mbuf(&fields, data);
    if (rc == 0) rc = ble_gap_ext_adv_set_data(0, data);
    if (rc != 0) {
        os_mbuf_free_chain(data);
        (void)ble_gap_ext_adv_stop(0);
        return rc;
    }

    rc = ble_gap_ext_adv_start(0, 0, 0);
    if (rc != 0) {
        (void)ble_gap_ext_adv_stop(0);
        return rc;
    }
    s_adv_started = true;
    return 0;
}

// ---------- 扫描 ----------

static int scan_event_cb(struct ble_gap_event *event, void *arg) {
    (void)arg;
    if (event->type != BLE_GAP_EVENT_EXT_DISC) return 0;

    const struct ble_gap_ext_disc_desc *desc = &event->ext_disc;
    if (desc->data_status != BLE_GAP_EXT_ADV_DATA_STATUS_COMPLETE ||
        desc->length_data == 0 || !desc->data || !is_valid_rssi(desc->rssi)) {
        return 0;
    }
    if (desc->prim_phy != BLE_HCI_LE_PHY_CODED ||
        desc->sec_phy != BLE_HCI_LE_PHY_CODED) {
        return 0;
    }

    struct ble_hs_adv_fields fields = {0};
    if (ble_hs_adv_parse_fields(&fields, desc->data, desc->length_data) != 0) {
        return 0;
    }
    if (!fields.name_is_complete || fields.name_len != sizeof(BLE_ADV_NAME) - 1 ||
        memcmp(fields.name, BLE_ADV_NAME, sizeof(BLE_ADV_NAME) - 1) != 0 ||
        fields.mfg_data_len != BLE_SENTINEL_DATA_LEN || !fields.mfg_data) {
        return 0;
    }

    const uint8_t *mfg = fields.mfg_data;
    if (mfg[0] != (uint8_t)(BLE_SENTINEL_COMPANY & 0xff) ||
        mfg[1] != (uint8_t)(BLE_SENTINEL_COMPANY >> 8) ||
        mfg[2] != 'R' || mfg[3] != BLE_SENTINEL_VERSION ||
        identity_is_self(&mfg[4])) {
        return 0;
    }
    if (!s_expected_identity_known ||
        memcmp(&mfg[4], s_expected_identity, BLE_IDENTITY_LEN) != 0) {
        int64_t now = esp_timer_get_time();
        if (now - s_diag_last_candidate_us > 2000000) {
            ESP_LOGI(TAG, "BLE candidate rejected expected=%u id=%02x:%02x:%02x:%02x:%02x:%02x",
                     s_expected_identity_known, mfg[4], mfg[5], mfg[6],
                     mfg[7], mfg[8], mfg[9]);
            s_diag_last_candidate_us = now;
        }
        return 0;
    }

    bool was_alive = s_peer_identity_known && s_ble_seen_us != 0 &&
                     (esp_timer_get_time() - s_ble_seen_us) < BLE_PEER_HOLD_US;
    s_ble_rssi = desc->rssi;
    memcpy(s_peer_identity, &mfg[4], BLE_IDENTITY_LEN);
    s_peer_identity_known = true;
    s_ble_seen_us = esp_timer_get_time();
    if (!was_alive) {
        ESP_LOGI(TAG, "BLE peer matched rssi=%d", (int)desc->rssi);
    }
    return 0;
}

static int start_scanning(void) {
    struct ble_gap_ext_disc_params uncoded = {0};
    uncoded.passive = 1;
    uncoded.itvl = BLE_GAP_SCAN_ITVL_MS(BLE_SCAN_ITVL_MS);
    uncoded.window = BLE_GAP_SCAN_WIN_MS(BLE_SCAN_WIN_MS);

    struct ble_gap_ext_disc_params coded = uncoded;
    int rc = ble_gap_ext_disc(s_own_addr_type, 0, 0, 0,
                              BLE_HCI_SCAN_FILT_NO_WL, 0,
                              &uncoded, &coded, scan_event_cb, NULL);
    if (rc == 0) s_scan_started = true;
    return rc;
}

// ---------- NimBLE 生命周期 ----------

static void on_sync(void) {
    if (ble_hs_util_ensure_addr(0) != 0 ||
        ble_hs_id_infer_auto(0, &s_own_addr_type) != 0 ||
        esp_wifi_get_mac(WIFI_IF_AP, s_own_identity) != ESP_OK) {
        ESP_LOGE(TAG, "BLE identity setup failed");
        return;
    }

    int adv_rc = start_advertising();
    int scan_rc = start_scanning();
    if (adv_rc != 0 || scan_rc != 0) {
        ESP_LOGE(TAG, "BLE start failed adv=%d scan=%d", adv_rc, scan_rc);
        if (s_scan_started) (void)ble_gap_disc_cancel();
        if (s_adv_started) (void)ble_gap_ext_adv_stop(0);
        s_scan_started = false;
        s_adv_started = false;
        return;
    }
    s_sync_seen = true;
    s_ready = true;
    ESP_LOGI(TAG, "BLE sentinel ready (coded PHY)");
}

static void on_reset(int reason) {
    s_ready = false;
    s_sync_seen = false;
    ESP_LOGW(TAG, "BLE host reset: %d", reason);
}

static void ble_host_task(void *param) {
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t radar_ble_init(void) {
    if (s_initialized) return ESP_OK;

    s_ready = false;
    s_sync_seen = false;
    s_adv_started = false;
    s_scan_started = false;
    s_peer_identity_known = false;
    s_diag_logged = false;
    s_diag_last_ready = false;
    s_diag_last_peer = false;
    s_diag_last_rssi = -127;
    s_diag_last_age_s = -1;
    s_diag_last_expected = false;
    s_diag_last_candidate_us = 0;
    s_expected_identity_known = false;
    s_ble_seen_us = 0;

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) return err;

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    nimble_port_freertos_init(ble_host_task);
    s_initialized = true;
    return ESP_OK;
}

void radar_ble_deinit(void) {
    if (!s_initialized) return;

    if (s_scan_started) {
        int rc = ble_gap_disc_cancel();
        if (rc != 0 && rc != BLE_HS_EALREADY) {
            ESP_LOGW(TAG, "BLE scan cancel rc=%d", rc);
        }
    }
    if (s_adv_started) {
        int rc = ble_gap_ext_adv_stop(0);
        if (rc != 0 && rc != BLE_HS_EALREADY) {
            ESP_LOGW(TAG, "BLE adv stop rc=%d", rc);
        }
    }
    s_ready = false;
    s_scan_started = false;
    s_adv_started = false;
    (void)nimble_port_stop();
    (void)nimble_port_deinit();
    // The host task calls nimble_port_freertos_deinit() after nimble_port_run()
    // returns; do not delete it twice from this caller.
    s_initialized = false;
}

bool radar_ble_ready(void) { return s_ready && s_sync_seen; }

void radar_ble_pause(bool paused) {
    if (paused == s_scan_paused) return;
    s_scan_paused = paused;
    if (paused) {
        if (s_scan_started) {
            int rc = ble_gap_disc_cancel();
            if (rc == 0 || rc == BLE_HS_EALREADY) s_scan_started = false;
            else ESP_LOGW(TAG, "BLE scan pause rc=%d", rc);
        }
    } else if (s_ready && !s_scan_started) {
        // 恢复扫描;失败不致命,下一轮 on_reset/on_sync 之外无自动重试,
        // 记录日志供诊断。
        int rc = start_scanning();
        if (rc != 0) ESP_LOGW(TAG, "BLE scan resume rc=%d", rc);
    }
}

void radar_ble_set_expected_identity(const uint8_t identity[6]) {
    if (!identity) return;
    if (!s_expected_identity_known ||
        memcmp(s_expected_identity, identity, BLE_IDENTITY_LEN) != 0) {
        memcpy(s_expected_identity, identity, BLE_IDENTITY_LEN);
        s_expected_identity_known = true;
        ESP_LOGI(TAG, "BLE identity bound id=%02x:%02x:%02x:%02x:%02x:%02x",
                 identity[0], identity[1], identity[2], identity[3],
                 identity[4], identity[5]);
    }
}

int radar_ble_peer_rssi(void) {
    return radar_ble_peer_alive() ? s_ble_rssi : -127;
}

int radar_ble_peer_last_rssi(void) {
    return s_peer_identity_known ? s_ble_rssi : -127;
}

bool radar_ble_peer_alive(void) {
    return s_peer_identity_known && s_ble_seen_us != 0 &&
           (esp_timer_get_time() - s_ble_seen_us) < BLE_PEER_HOLD_US;
}

int radar_ble_peer_age_ms(void) {
    if (!s_peer_identity_known || s_ble_seen_us == 0) return -1;
    int64_t age_us = esp_timer_get_time() - s_ble_seen_us;
    if (age_us < 0) return -1;
    return (int)(age_us / 1000);
}

void radar_ble_diag_tick(void) {
    bool ready = radar_ble_ready();
    bool peer = radar_ble_peer_alive();
    int rssi = radar_ble_peer_last_rssi();
    int age_ms = radar_ble_peer_age_ms();
    int age_s = age_ms >= 0 ? age_ms / 1000 : -1;
    if (!s_diag_logged || ready != s_diag_last_ready || peer != s_diag_last_peer ||
        rssi != s_diag_last_rssi || age_s != s_diag_last_age_s ||
        s_expected_identity_known != s_diag_last_expected) {
        ESP_LOGI(TAG, "BLE_STATE ready=%u peer=%u expected=%u rssi=%d age_ms=%d scan=%u adv=%u",
                 ready, peer, s_expected_identity_known, rssi, age_ms,
                 s_scan_started, s_adv_started);
        s_diag_logged = true;
        s_diag_last_ready = ready;
        s_diag_last_peer = peer;
        s_diag_last_rssi = rssi;
        s_diag_last_age_s = age_s;
        s_diag_last_expected = s_expected_identity_known;
    }
}

bool radar_ble_peer_identity(uint8_t out_identity[6]) {
    if (!out_identity || !radar_ble_peer_alive()) return false;
    memcpy(out_identity, s_peer_identity, BLE_IDENTITY_LEN);
    return true;
}
