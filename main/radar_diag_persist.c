#include "radar_diag_persist.h"

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <stddef.h>
#include <string.h>

#define RADAR_DIAG_NVS_NS "radar_diag"
#define RADAR_DIAG_NVS_KEY "last"
#define RADAR_DIAG_RTC_MAGIC 0x52444342u /* RDCB */
#define RADAR_DIAG_RTC_VERSION 1u

static const char *TAG = "radar_diag";

// RTC fast memory survives software panic/watchdog resets and is updated before
// the corresponding NVS write. It is intentionally tiny and lock-free: a
// panic/brownout path must never attempt NVS, heap allocation, or logging.
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t stage;
    uint16_t flags;
    int16_t peer_rssi;
    uint16_t csi_frames;
    uint32_t sequence;
    uint32_t uptime_ms;
    uint32_t inverse_magic;
} radar_diag_rtc_breadcrumb_t;

RTC_DATA_ATTR static radar_diag_rtc_breadcrumb_t s_rtc_breadcrumb;
static radar_diag_record_t s_current;
static radar_diag_record_t s_previous;
static bool s_previous_valid;
static bool s_initialized;

static const char *stage_name(uint16_t stage) {
    switch ((radar_diag_stage_t)stage) {
    case RADAR_DIAG_STAGE_BOOT: return "BOOT";
    case RADAR_DIAG_STAGE_DISPLAY_READY: return "DISPLAY_READY";
    case RADAR_DIAG_STAGE_RADIO_INIT: return "RADIO_INIT";
    case RADAR_DIAG_STAGE_RADIO_READY: return "RADIO_READY";
    case RADAR_DIAG_STAGE_CSI_READY: return "CSI_READY";
    case RADAR_DIAG_STAGE_PEER_SEEN: return "PEER_SEEN";
    case RADAR_DIAG_STAGE_RUNNING: return "RUNNING";
    default: return "UNKNOWN";
    }
}

static bool rtc_breadcrumb_valid(void) {
    return s_rtc_breadcrumb.magic == RADAR_DIAG_RTC_MAGIC &&
           s_rtc_breadcrumb.version == RADAR_DIAG_RTC_VERSION &&
           s_rtc_breadcrumb.inverse_magic == ~RADAR_DIAG_RTC_MAGIC;
}

static void rtc_breadcrumb_update(radar_diag_stage_t stage, uint16_t flags,
                                  int peer_rssi, unsigned csi_frames,
                                  uint32_t sequence) {
    if (csi_frames > UINT16_MAX) csi_frames = UINT16_MAX;
    if (peer_rssi < -127) peer_rssi = -127;
    if (peer_rssi > 20) peer_rssi = 20;

    // Write the validity marker last so a reset during an update cannot turn a
    // partially written breadcrumb into a valid one.
    s_rtc_breadcrumb.magic = 0;
    s_rtc_breadcrumb.version = RADAR_DIAG_RTC_VERSION;
    s_rtc_breadcrumb.stage = (uint16_t)stage;
    s_rtc_breadcrumb.flags = flags;
    s_rtc_breadcrumb.peer_rssi = (int16_t)peer_rssi;
    s_rtc_breadcrumb.csi_frames = (uint16_t)csi_frames;
    s_rtc_breadcrumb.sequence = sequence;
    s_rtc_breadcrumb.uptime_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_rtc_breadcrumb.inverse_magic = ~RADAR_DIAG_RTC_MAGIC;
    s_rtc_breadcrumb.magic = RADAR_DIAG_RTC_MAGIC;
}

uint32_t radar_diag_record_crc(const radar_diag_record_t *record) {
    if (!record) return 0;
    const uint8_t *bytes = (const uint8_t *)record;
    uint32_t crc = 0xFFFFFFFFu;
    for (unsigned i = 0; i < offsetof(radar_diag_record_t, crc32); ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int)(crc & 1u));
        }
    }
    return ~crc;
}

bool radar_diag_record_valid(const radar_diag_record_t *record) {
    return record && record->magic == RADAR_DIAG_RECORD_MAGIC &&
           record->version == RADAR_DIAG_RECORD_VERSION &&
           record->size == sizeof(*record) &&
           record->crc32 == radar_diag_record_crc(record);
}

static bool load_record(radar_diag_record_t *out) {
    nvs_handle_t handle;
    if (!out || nvs_open(RADAR_DIAG_NVS_NS, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    size_t size = sizeof(*out);
    esp_err_t err = nvs_get_blob(handle, RADAR_DIAG_NVS_KEY, out, &size);
    nvs_close(handle);
    return err == ESP_OK && size == sizeof(*out) && radar_diag_record_valid(out);
}

static void store_current(void) {
    nvs_handle_t handle;
    if (nvs_open(RADAR_DIAG_NVS_NS, NVS_READWRITE, &handle) != ESP_OK) return;
    s_current.crc32 = radar_diag_record_crc(&s_current);
    if (nvs_set_blob(handle, RADAR_DIAG_NVS_KEY, &s_current, sizeof(s_current)) == ESP_OK) {
        (void)nvs_commit(handle);
    }
    nvs_close(handle);
}

static void report_previous(uint32_t reset_reason) {
    if (s_previous_valid) {
        ESP_LOGW(TAG, "previous NVS: seq=%lu reset=%lu stage=%s(%u) flags=0x%04x "
                      "rssi=%d csi=%u uptime=%lums",
                 (unsigned long)s_previous.sequence,
                 (unsigned long)s_previous.reset_reason,
                 stage_name(s_previous.stage), s_previous.stage,
                 s_previous.flags, s_previous.peer_rssi,
                 s_previous.csi_frames, (unsigned long)s_previous.uptime_ms);
    } else {
        ESP_LOGW(TAG, "previous NVS: unavailable");
    }

    if (rtc_breadcrumb_valid()) {
        ESP_LOGW(TAG, "previous RTC breadcrumb: seq=%lu reset_now=%lu stage=%s(%u) "
                      "flags=0x%04x rssi=%d csi=%u uptime=%lums",
                 (unsigned long)s_rtc_breadcrumb.sequence,
                 (unsigned long)reset_reason,
                 stage_name(s_rtc_breadcrumb.stage), s_rtc_breadcrumb.stage,
                 s_rtc_breadcrumb.flags, s_rtc_breadcrumb.peer_rssi,
                 s_rtc_breadcrumb.csi_frames,
                 (unsigned long)s_rtc_breadcrumb.uptime_ms);
    } else {
        ESP_LOGW(TAG, "previous RTC breadcrumb: unavailable (power-on or invalid)");
    }
}

bool radar_diag_persist_init(void) {
    if (s_initialized) return true;

    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) return false;

    s_previous_valid = load_record(&s_previous);
    uint32_t reset_reason = (uint32_t)esp_reset_reason();
    report_previous(reset_reason);

    memset(&s_current, 0, sizeof(s_current));
    s_current.magic = RADAR_DIAG_RECORD_MAGIC;
    s_current.version = RADAR_DIAG_RECORD_VERSION;
    s_current.size = sizeof(s_current);
    s_current.sequence = s_previous_valid ? s_previous.sequence + 1u : 1u;
    s_current.reset_reason = reset_reason;
    s_current.stage = RADAR_DIAG_STAGE_BOOT;
    s_current.peer_rssi = -127;
    rtc_breadcrumb_update(RADAR_DIAG_STAGE_BOOT, 0, -127, 0,
                          s_current.sequence);
    store_current();
    s_initialized = true;
    return true;
}

void radar_diag_persist_stage(radar_diag_stage_t stage, uint16_t flags,
                              int peer_rssi, unsigned csi_frames) {
    if (!s_initialized) return;
    if (csi_frames > UINT16_MAX) csi_frames = UINT16_MAX;
    if (peer_rssi < -127) peer_rssi = -127;
    if (peer_rssi > 20) peer_rssi = 20;

    uint32_t uptime_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (s_current.stage == (uint16_t)stage && s_current.flags == flags &&
        s_current.peer_rssi == peer_rssi && s_current.csi_frames == csi_frames) {
        return;
    }
    s_current.stage = (uint16_t)stage;
    s_current.flags = flags;
    s_current.peer_rssi = (int16_t)peer_rssi;
    s_current.csi_frames = (uint16_t)csi_frames;
    s_current.uptime_ms = uptime_ms;
    rtc_breadcrumb_update(stage, flags, peer_rssi, csi_frames,
                          s_current.sequence);
    store_current();
}

bool radar_diag_persist_previous(radar_diag_record_t *out) {
    if (!out || !s_previous_valid) return false;
    *out = s_previous;
    return true;
}

uint32_t radar_diag_current_sequence(void) {
    return s_initialized ? s_current.sequence : 0;
}

uint32_t radar_diag_current_reset_reason(void) {
    return s_initialized ? s_current.reset_reason : 0;
}
