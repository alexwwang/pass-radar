#include "radar_capture.h"

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "radar_diag_persist.h"

#include <math.h>
#include <string.h>

static const char *TAG = "radar_capture";

#define CAPTURE_COUNTDOWN_MS 3000u
#define CAPTURE_SETTLE_MS 5000u
#define CAPTURE_RUNNING_MS 30000u
#define CAPTURE_SAMPLE_PERIOD_MS 250u
#define CAPTURE_MAX_RECORDS 128u
#define CAPTURE_MAGIC 0x52434150u
#define CAPTURE_VERSION 1u
#define CAPTURE_PARTITION_LABEL "radar_capture"

typedef struct {
    uint32_t elapsed_ms;
    int8_t local_rssi;
    int8_t peer_rssi;
    uint8_t flags;
    int16_t ftm_status;
    float ftm_distance_m;
    float ftm_spread_m;
    uint16_t ftm_session_count;
    int8_t ble_rssi;
    int32_t ble_age_ms;
    uint32_t csi_frames;
    uint8_t env_level;
    uint8_t reserved[3];
} capture_record_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t record_size;
    float target_distance_m;
    uint32_t record_count;
    uint32_t valid;
    uint32_t capture_id;
    uint32_t boot_sequence;
    uint32_t reset_reason;
    char firmware_version[32];
    uint32_t crc32;
} capture_header_t;

const float radar_capture_targets_m[RADAR_CAPTURE_TARGET_COUNT] = {
    1.0f, 3.0f, 5.0f, 8.0f, 10.0f, 15.0f, 20.0f, 30.0f,
};

static capture_record_t s_records[CAPTURE_MAX_RECORDS];
static radar_capture_phase_t s_phase;
static float s_target_m;
static int64_t s_start_us;
static int64_t s_last_sample_us;
static unsigned s_record_count;
static bool s_data_valid;
static bool s_save_succeeded;
static uint32_t s_capture_id;

static uint32_t crc32_bytes(const void *data, size_t length) {
    const uint8_t *bytes = data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int)(crc & 1u));
        }
    }
    return ~crc;
}

static const char *phase_name(radar_capture_phase_t phase) {
    switch (phase) {
    case RADAR_CAPTURE_COUNTDOWN: return "COUNTDOWN";
    case RADAR_CAPTURE_SETTLE: return "SETTLE";
    case RADAR_CAPTURE_RUNNING: return "CAPTURE";
    case RADAR_CAPTURE_SUMMARY: return "SUMMARY";
    case RADAR_CAPTURE_INVALID: return "INVALID";
    default: return "IDLE";
    }
}

void radar_capture_init(void) {
    memset(s_records, 0, sizeof(s_records));
    s_phase = RADAR_CAPTURE_IDLE;
    s_target_m = 0.0f;
    s_start_us = 0;
    s_last_sample_us = 0;
    s_record_count = 0;
    s_data_valid = false;
    s_save_succeeded = false;
    s_capture_id = 0;
}

esp_err_t radar_capture_start(float target_distance_m) {
    if (!isfinite(target_distance_m) || target_distance_m <= 0.0f ||
        s_phase == RADAR_CAPTURE_COUNTDOWN || s_phase == RADAR_CAPTURE_SETTLE ||
        s_phase == RADAR_CAPTURE_RUNNING) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(s_records, 0, sizeof(s_records));
    s_target_m = target_distance_m;
    s_start_us = esp_timer_get_time();
    s_last_sample_us = 0;
    s_record_count = 0;
    s_data_valid = false;
    s_save_succeeded = false;
    s_capture_id++;
    s_phase = RADAR_CAPTURE_COUNTDOWN;
    ESP_LOGI(TAG, "CAPTURE_START target_m=%.1f phase=%s",
             (double)s_target_m, phase_name(s_phase));
    return ESP_OK;
}

void radar_capture_abort(void) {
    if (s_phase == RADAR_CAPTURE_COUNTDOWN || s_phase == RADAR_CAPTURE_SETTLE ||
        s_phase == RADAR_CAPTURE_RUNNING) {
        s_phase = RADAR_CAPTURE_INVALID;
        s_data_valid = false;
        s_save_succeeded = false;
        ESP_LOGW(TAG, "CAPTURE_ABORT target_m=%.1f", (double)s_target_m);
    }
}

void radar_capture_tick(const radar_capture_observation_t *observation) {
    if (s_phase == RADAR_CAPTURE_IDLE || s_phase == RADAR_CAPTURE_SUMMARY ||
        s_phase == RADAR_CAPTURE_INVALID) return;

    int64_t now = esp_timer_get_time();
    uint32_t elapsed_ms = (uint32_t)((now - s_start_us) / 1000);
    if (elapsed_ms < CAPTURE_COUNTDOWN_MS) {
        s_phase = RADAR_CAPTURE_COUNTDOWN;
        return;
    }
    if (elapsed_ms < CAPTURE_COUNTDOWN_MS + CAPTURE_SETTLE_MS) {
        s_phase = RADAR_CAPTURE_SETTLE;
        return;
    }

    uint32_t capture_elapsed = elapsed_ms - CAPTURE_COUNTDOWN_MS - CAPTURE_SETTLE_MS;
    if (capture_elapsed >= CAPTURE_RUNNING_MS) {
        s_phase = RADAR_CAPTURE_SUMMARY;
        s_data_valid = s_record_count > 0;
        ESP_LOGI(TAG, "CAPTURE_DONE target_m=%.1f records=%u valid=%u",
                 (double)s_target_m, s_record_count, s_data_valid);
        (void)radar_capture_save();
        return;
    }

    s_phase = RADAR_CAPTURE_RUNNING;
    if (!observation || s_record_count >= CAPTURE_MAX_RECORDS ||
        (s_last_sample_us != 0 && now - s_last_sample_us <
         (int64_t)CAPTURE_SAMPLE_PERIOD_MS * 1000)) return;

    capture_record_t *record = &s_records[s_record_count++];
    record->elapsed_ms = capture_elapsed;
    record->local_rssi = observation->local_rssi;
    record->peer_rssi = observation->peer_rssi;
    record->flags = (observation->peer_alive ? 1u : 0u) |
                    (observation->ftm_valid ? 2u : 0u) |
                    (observation->ble_ready ? 4u : 0u) |
                    (observation->ble_peer_alive ? 8u : 0u);
    record->ftm_status = observation->ftm_status;
    record->ftm_distance_m = observation->ftm_distance_m;
    record->ftm_spread_m = observation->ftm_spread_m;
    record->ftm_session_count = observation->ftm_session_count;
    record->ble_rssi = observation->ble_rssi;
    record->ble_age_ms = observation->ble_age_ms;
    record->csi_frames = observation->csi_frames;
    record->env_level = observation->environment_level;
    s_last_sample_us = now;
}

radar_capture_phase_t radar_capture_phase(void) { return s_phase; }
float radar_capture_target_m(void) { return s_target_m; }
uint32_t radar_capture_elapsed_ms(void) {
    if (s_start_us == 0) return 0;
    return (uint32_t)((esp_timer_get_time() - s_start_us) / 1000);
}
unsigned radar_capture_record_count(void) { return s_record_count; }
bool radar_capture_data_valid(void) { return s_data_valid; }
bool radar_capture_save_succeeded(void) { return s_save_succeeded; }

esp_err_t radar_capture_save(void) {
    s_save_succeeded = false;
    if (s_phase != RADAR_CAPTURE_SUMMARY || !s_data_valid) return ESP_ERR_INVALID_STATE;

    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, CAPTURE_PARTITION_LABEL);
    if (!partition) {
        ESP_LOGE(TAG, "CAPTURE_SAVE failed: partition missing");
        return ESP_ERR_NOT_FOUND;
    }

    capture_header_t header = {
        .magic = CAPTURE_MAGIC,
        .version = CAPTURE_VERSION,
        .record_size = sizeof(capture_record_t),
        .target_distance_m = s_target_m,
        .record_count = s_record_count,
        .valid = 1,
        .capture_id = s_capture_id,
        .boot_sequence = radar_diag_current_sequence(),
        .reset_reason = radar_diag_current_reset_reason(),
    };
    const esp_app_desc_t *app = esp_app_get_description();
    if (app) {
        strncpy(header.firmware_version, app->version,
                sizeof(header.firmware_version) - 1u);
        header.firmware_version[sizeof(header.firmware_version) - 1u] = '\\0';
    }
    header.crc32 = crc32_bytes(s_records, s_record_count * sizeof(s_records[0]));
    size_t total = sizeof(header) + s_record_count * sizeof(s_records[0]);
    if (total > partition->size) return ESP_ERR_NO_MEM;

    esp_err_t err = esp_partition_erase_range(partition, 0, partition->size);
    if (err == ESP_OK) err = esp_partition_write(partition, 0, &header, sizeof(header));
    if (err == ESP_OK) {
        err = esp_partition_write(partition, sizeof(header), s_records,
                                  s_record_count * sizeof(s_records[0]));
    }
    if (err != ESP_OK) {
        s_data_valid = false;
        ESP_LOGE(TAG, "CAPTURE_SAVE failed err=%s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "CAPTURE_META capture_id=%lu firmware=%s boot_sequence=%lu reset_reason=%lu target_m=%.1f records=%lu crc32=0x%08lx",
             (unsigned long)header.capture_id, header.firmware_version,
             (unsigned long)header.boot_sequence,
             (unsigned long)header.reset_reason, (double)header.target_distance_m,
             (unsigned long)header.record_count, (unsigned long)header.crc32);
    ESP_LOGI(TAG, "CAPTURE_CSV target_m,elapsed_ms,local_rssi,peer_rssi,peer_alive,ftm_valid,ftm_status,ftm_distance_m,ftm_spread_m,ftm_session_count,ble_ready,ble_peer_alive,ble_rssi,ble_age_ms,csi_frames,env_level");
    for (unsigned i = 0; i < s_record_count; ++i) {
        const capture_record_t *r = &s_records[i];
        ESP_LOGI(TAG, "CAPTURE_CSV %.1f,%u,%d,%d,%u,%u,%d,%.3f,%.3f,%u,%u,%u,%d,%ld,%lu,%u",
                 (double)s_target_m, r->elapsed_ms, r->local_rssi, r->peer_rssi,
                 (r->flags & 1u) != 0, (r->flags & 2u) != 0, r->ftm_status,
                 (double)r->ftm_distance_m, (double)r->ftm_spread_m,
                 r->ftm_session_count, (r->flags & 4u) != 0,
                 (r->flags & 8u) != 0, r->ble_rssi, (long)r->ble_age_ms,
                 (unsigned long)r->csi_frames, r->env_level);
    }
    s_save_succeeded = true;
    ESP_LOGI(TAG, "CAPTURE_SAVE OK target_m=%.1f records=%u crc32=0x%08lx",
             (double)s_target_m, s_record_count, (unsigned long)header.crc32);
    return ESP_OK;
}

esp_err_t radar_capture_export_saved(void) {
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, CAPTURE_PARTITION_LABEL);
    if (!partition) return ESP_ERR_NOT_FOUND;

    capture_header_t header;
    esp_err_t err = esp_partition_read(partition, 0, &header, sizeof(header));
    if (err != ESP_OK || header.magic != CAPTURE_MAGIC ||
        header.version != CAPTURE_VERSION || header.record_size != sizeof(capture_record_t) ||
        header.valid == 0 || header.record_count == 0 ||
        header.record_count > CAPTURE_MAX_RECORDS) {
        ESP_LOGW(TAG, "CAPTURE_EXPORT no valid saved record");
        return ESP_ERR_NOT_FOUND;
    }

    size_t payload_size = header.record_count * sizeof(capture_record_t);
    if (sizeof(header) + payload_size > partition->size) return ESP_ERR_INVALID_SIZE;
    err = esp_partition_read(partition, sizeof(header), s_records, payload_size);
    if (err != ESP_OK || crc32_bytes(s_records, payload_size) != header.crc32) {
        ESP_LOGE(TAG, "CAPTURE_EXPORT CRC or read failure");
        return ESP_ERR_INVALID_CRC;
    }

    ESP_LOGI(TAG, "CAPTURE_EXPORT_META capture_id=%lu firmware=%s boot_sequence=%lu reset_reason=%lu target_m=%.1f records=%lu crc32=0x%08lx",
             (unsigned long)header.capture_id, header.firmware_version,
             (unsigned long)header.boot_sequence, (unsigned long)header.reset_reason,
             (double)header.target_distance_m, (unsigned long)header.record_count,
             (unsigned long)header.crc32);
    ESP_LOGI(TAG, "CAPTURE_EXPORT_CSV target_m,elapsed_ms,local_rssi,peer_rssi,peer_alive,ftm_valid,ftm_status,ftm_distance_m,ftm_spread_m,ftm_session_count,ble_ready,ble_peer_alive,ble_rssi,ble_age_ms,csi_frames,env_level");
    for (unsigned i = 0; i < header.record_count; ++i) {
        const capture_record_t *r = &s_records[i];
        ESP_LOGI(TAG, "CAPTURE_CSV %.1f,%u,%d,%d,%u,%u,%d,%.3f,%.3f,%u,%u,%u,%d,%ld,%lu,%u",
                 (double)header.target_distance_m, r->elapsed_ms, r->local_rssi,
                 r->peer_rssi, (r->flags & 1u) != 0, (r->flags & 2u) != 0,
                 r->ftm_status, (double)r->ftm_distance_m, (double)r->ftm_spread_m,
                 r->ftm_session_count, (r->flags & 4u) != 0, (r->flags & 8u) != 0,
                 r->ble_rssi, (long)r->ble_age_ms, (unsigned long)r->csi_frames,
                 r->env_level);
    }
    ESP_LOGI(TAG, "CAPTURE_EXPORT OK capture_id=%lu records=%lu",
             (unsigned long)header.capture_id, (unsigned long)header.record_count);
    return ESP_OK;
}
