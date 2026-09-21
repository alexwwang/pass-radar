#include "radar_capture.h"

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "radar_capture_codec.h"
#include "radar_diag_persist.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "radar_capture";

#define CAPTURE_COUNTDOWN_MS 3000u
#define CAPTURE_SETTLE_MS 5000u
#define CAPTURE_RUNNING_MS 30000u
#define CAPTURE_SAMPLE_PERIOD_MS 250u
#define CAPTURE_MAGIC 0x52434132u /* RCA2 */
#define CAPTURE_STORAGE_VERSION 1u
#define CAPTURE_NVS_NS "radar_capture"
#define CAPTURE_NVS_KEY_A "latest_a"
#define CAPTURE_NVS_KEY_B "latest_b"
#define CAPTURE_STORAGE_MAX 12288u

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t codec_id;
    uint32_t generation;
    float target_distance_m;
    uint32_t record_count;
    uint32_t uncompressed_length;
    uint32_t compressed_length;
    uint32_t capture_id;
    uint32_t boot_sequence;
    uint32_t reset_reason;
    char firmware_version[32];
    uint32_t payload_crc32;
} capture_storage_header_t;

_Static_assert(sizeof(capture_storage_header_t) == 76,
               "capture storage header layout changed");

// Capture storage is allocated on demand, after radio/BLE initialization.
// Keeping these buffers out of BSS preserves the startup heap required by
// NimBLE; capture/export paths run in the LVGL task and are not concurrent.
static radar_capture_record_t *s_records;
static uint8_t *s_scratch;

static bool ensure_storage(void) {
    if (!s_records) {
        s_records = calloc(RADAR_CAPTURE_MAX_RECORDS, sizeof(*s_records));
    }
    if (!s_scratch) {
        s_scratch = malloc(CAPTURE_STORAGE_MAX);
    }
    if (!s_records || !s_scratch) {
        free(s_records);
        free(s_scratch);
        s_records = NULL;
        s_scratch = NULL;
        return false;
    }
    return true;
}
static radar_capture_phase_t s_phase;
static float s_target_m;
static int64_t s_start_us;
static int64_t s_last_sample_us;
static unsigned s_record_count;
static bool s_data_valid;
static bool s_save_succeeded;
static uint32_t s_capture_id;
// Screen feedback for the DOWN long-press re-export: "" = never attempted,
// otherwise a short status string cleared on the next Debug-page entry.
static char s_export_status[16];
static int64_t s_export_status_until_us;

const float radar_capture_targets_m[RADAR_CAPTURE_TARGET_COUNT] = {
    1.0f, 3.0f, 5.0f, 8.0f, 10.0f, 15.0f, 20.0f, 30.0f,
};

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

static const char *key_for_generation(uint32_t generation) {
    return (generation & 1u) ? CAPTURE_NVS_KEY_A : CAPTURE_NVS_KEY_B;
}

static bool valid_header(const capture_storage_header_t *header) {
    return header && header->magic == CAPTURE_MAGIC &&
           header->version == CAPTURE_STORAGE_VERSION &&
           header->codec_id == RADAR_CAPTURE_CODEC_ID &&
           header->record_count > 0 &&
           header->record_count <= RADAR_CAPTURE_MAX_RECORDS &&
           header->uncompressed_length ==
               header->record_count * sizeof(radar_capture_record_t) &&
           header->compressed_length > 0 &&
           header->compressed_length <= RADAR_CAPTURE_CODEC_MAX_PAYLOAD;
}

static bool read_key(const char *key, capture_storage_header_t *header,
                     uint8_t *payload, size_t payload_capacity) {
    nvs_handle_t handle;
    if (!key || !header || !payload ||
        nvs_open(CAPTURE_NVS_NS, NVS_READONLY, &handle) != ESP_OK) return false;

    size_t blob_size = CAPTURE_STORAGE_MAX;
    esp_err_t err = nvs_get_blob(handle, key, s_scratch, &blob_size);
    nvs_close(handle);
    if (err != ESP_OK || blob_size < sizeof(*header) ||
        blob_size > CAPTURE_STORAGE_MAX) return false;

    memcpy(header, s_scratch, sizeof(*header));
    if (!valid_header(header) ||
        sizeof(*header) + header->compressed_length != blob_size ||
        header->compressed_length > payload_capacity) return false;
    memcpy(payload, s_scratch + sizeof(*header), header->compressed_length);
    return radar_capture_codec_crc32(payload, header->compressed_length) ==
           header->payload_crc32;
}

static bool read_latest(capture_storage_header_t *header, uint8_t *payload,
                        size_t payload_capacity) {
    capture_storage_header_t candidate;
    bool found = false;
    uint32_t best_generation = 0;
    for (unsigned i = 0; i < 2; ++i) {
        const char *key = i == 0 ? CAPTURE_NVS_KEY_A : CAPTURE_NVS_KEY_B;
        if (!read_key(key, &candidate, payload, payload_capacity)) continue;
        if (!found || candidate.generation > best_generation) {
            found = true;
            best_generation = candidate.generation;
            *header = candidate;
        }
    }
    return found;
}

static uint32_t latest_generation(void) {
    capture_storage_header_t header;
    return read_latest(&header, s_scratch + sizeof(capture_storage_header_t),
                       CAPTURE_STORAGE_MAX - sizeof(capture_storage_header_t))
               ? header.generation : 0;
}

static void emit_csv(const char *meta_tag, const char *row_tag,
                     const capture_storage_header_t *header,
                     const radar_capture_record_t *records) {
    ESP_LOGI(TAG, "%s capture_id=%lu firmware=%s boot_sequence=%lu reset_reason=%lu target_m=%.1f records=%lu crc32=0x%08lx generation=%lu compressed=%lu/%lu",
             meta_tag, (unsigned long)header->capture_id, header->firmware_version,
             (unsigned long)header->boot_sequence,
             (unsigned long)header->reset_reason, (double)header->target_distance_m,
             (unsigned long)header->record_count,
             (unsigned long)header->payload_crc32,
             (unsigned long)header->generation,
             (unsigned long)header->compressed_length,
             (unsigned long)header->uncompressed_length);
    ESP_LOGI(TAG, "%s target_m,elapsed_ms,local_rssi,peer_rssi,peer_alive,ftm_valid,ftm_status,ftm_distance_m,ftm_spread_m,ftm_session_count,ble_ready,ble_peer_alive,ble_rssi,ble_age_ms,csi_frames,env_level",
             row_tag);
    for (uint32_t i = 0; i < header->record_count; ++i) {
        const radar_capture_record_t *record = &records[i];
        ESP_LOGI(TAG, "%s %.1f,%u,%d,%d,%u,%u,%d,%.3f,%.3f,%u,%u,%u,%d,%ld,%lu,%u",
                 row_tag, (double)header->target_distance_m, record->elapsed_ms,
                 record->local_rssi, record->peer_rssi,
                 (record->flags & 1u) != 0, (record->flags & 2u) != 0,
                 record->ftm_status, (double)record->ftm_distance_m,
                 (double)record->ftm_spread_m, record->ftm_session_count,
                 (record->flags & 4u) != 0, (record->flags & 8u) != 0,
                 record->ble_rssi, (long)record->ble_age_ms,
                 (unsigned long)record->csi_frames, record->env_level);
    }
}

void radar_capture_init(void) {
    if (s_records) memset(s_records, 0,
                           RADAR_CAPTURE_MAX_RECORDS * sizeof(*s_records));
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
        s_phase == RADAR_CAPTURE_RUNNING) return ESP_ERR_INVALID_STATE;
    if (!ensure_storage()) return ESP_ERR_NO_MEM;
    memset(s_records, 0,
           RADAR_CAPTURE_MAX_RECORDS * sizeof(*s_records));
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
    if (!observation || s_record_count >= RADAR_CAPTURE_MAX_RECORDS ||
        (s_last_sample_us != 0 && now - s_last_sample_us <
         (int64_t)CAPTURE_SAMPLE_PERIOD_MS * 1000)) return;
    radar_capture_record_t *record = &s_records[s_record_count++];
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
    if (!ensure_storage()) {
        ESP_LOGE(TAG, "CAPTURE_SAVE FAIL RAM");
        return ESP_ERR_NO_MEM;
    }

    size_t payload_length = radar_capture_codec_encode(
        s_records, s_record_count,
        s_scratch + sizeof(capture_storage_header_t),
        CAPTURE_STORAGE_MAX - sizeof(capture_storage_header_t));
    if (payload_length == 0 || payload_length > RADAR_CAPTURE_CODEC_MAX_PAYLOAD) {
        ESP_LOGE(TAG, "CAPTURE_SAVE FAIL ENCODE");
        return ESP_ERR_NO_MEM;
    }

    nvs_stats_t stats;
    if (nvs_get_stats(NULL, &stats) == ESP_OK) {
        ESP_LOGI(TAG, "CAPTURE_NVS used=%lu free=%lu total=%lu",
                 (unsigned long)stats.used_entries,
                 (unsigned long)stats.free_entries,
                 (unsigned long)stats.total_entries);
    }

    uint32_t generation = latest_generation() + 1u;
    capture_storage_header_t header = {
        .magic = CAPTURE_MAGIC,
        .version = CAPTURE_STORAGE_VERSION,
        .codec_id = RADAR_CAPTURE_CODEC_ID,
        .generation = generation,
        .target_distance_m = s_target_m,
        .record_count = s_record_count,
        .uncompressed_length = s_record_count * sizeof(s_records[0]),
        .compressed_length = payload_length,
        .capture_id = s_capture_id,
        .boot_sequence = radar_diag_current_sequence(),
        .reset_reason = radar_diag_current_reset_reason(),
        .payload_crc32 = radar_capture_codec_crc32(
            s_scratch + sizeof(capture_storage_header_t), payload_length),
    };
    const esp_app_desc_t *app = esp_app_get_description();
    if (app) {
        strncpy(header.firmware_version, app->version,
                sizeof(header.firmware_version) - 1u);
        header.firmware_version[sizeof(header.firmware_version) - 1u] = '\0';
    }
    size_t blob_length = sizeof(header) + payload_length;
    if (blob_length > CAPTURE_STORAGE_MAX) {
        ESP_LOGE(TAG, "CAPTURE_SAVE FAIL NVS_SPACE blob=%u max=%u",
                 (unsigned)blob_length, (unsigned)CAPTURE_STORAGE_MAX);
        return ESP_ERR_NO_MEM;
    }
    memcpy(s_scratch, &header, sizeof(header));

    nvs_handle_t handle;
    esp_err_t err = nvs_open(CAPTURE_NVS_NS, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, key_for_generation(generation), s_scratch, blob_length);
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    if (err != ESP_OK) {
        if (err == ESP_ERR_NVS_NOT_ENOUGH_SPACE || err == ESP_ERR_NVS_PAGE_FULL) {
            ESP_LOGE(TAG, "CAPTURE_SAVE FAIL NVS_SPACE err=%s", esp_err_to_name(err));
        } else {
            ESP_LOGE(TAG, "CAPTURE_SAVE FAIL NVS err=%s", esp_err_to_name(err));
        }
        return err;
    }

    capture_storage_header_t verified;
    if (!read_key(key_for_generation(generation), &verified,
                  s_scratch + sizeof(capture_storage_header_t),
                  CAPTURE_STORAGE_MAX - sizeof(capture_storage_header_t)) ||
        verified.generation != generation ||
        !radar_capture_codec_decode(s_scratch + sizeof(capture_storage_header_t),
                                    verified.compressed_length,
                                    s_records, RADAR_CAPTURE_MAX_RECORDS,
                                    verified.record_count)) {
        ESP_LOGE(TAG, "CAPTURE_SAVE FAIL VERIFY");
        return ESP_ERR_INVALID_CRC;
    }

    s_record_count = verified.record_count;
    s_target_m = verified.target_distance_m;
    s_save_succeeded = true;
    emit_csv("CAPTURE_META", "CAPTURE_CSV", &verified, s_records);
    ESP_LOGI(TAG, "CAPTURE_SAVE OK target_m=%.1f records=%u crc32=0x%08lx generation=%lu compressed=%lu/%lu",
             (double)s_target_m, s_record_count,
             (unsigned long)verified.payload_crc32,
             (unsigned long)verified.generation,
             (unsigned long)verified.compressed_length,
             (unsigned long)verified.uncompressed_length);
    return ESP_OK;
}

esp_err_t radar_capture_export_saved(void) {
    capture_storage_header_t header;
    if (!ensure_storage()) {
        ESP_LOGW(TAG, "CAPTURE_EXPORT FAIL RAM");
        snprintf(s_export_status, sizeof(s_export_status), "EXPORT RAM");
        s_export_status_until_us = esp_timer_get_time() + 5000000;
        return ESP_ERR_NO_MEM;
    }
    if (!read_latest(&header, s_scratch + sizeof(capture_storage_header_t),
                     CAPTURE_STORAGE_MAX - sizeof(capture_storage_header_t)) ||
        !radar_capture_codec_decode(s_scratch + sizeof(capture_storage_header_t),
                                    header.compressed_length,
                                    s_records, RADAR_CAPTURE_MAX_RECORDS,
                                    header.record_count)) {
        ESP_LOGW(TAG, "CAPTURE_EXPORT no valid saved record");
        snprintf(s_export_status, sizeof(s_export_status), "EXPORT NOREC");
        s_export_status_until_us = esp_timer_get_time() + 5000000;
        return ESP_ERR_NOT_FOUND;
    }
    emit_csv("CAPTURE_EXPORT_META", "CAPTURE_EXPORT_CSV", &header, s_records);
    ESP_LOGI(TAG, "CAPTURE_EXPORT OK capture_id=%lu records=%lu generation=%lu",
             (unsigned long)header.capture_id,
             (unsigned long)header.record_count,
             (unsigned long)header.generation);
    snprintf(s_export_status, sizeof(s_export_status), "EXPORT OK %lu",
             (unsigned long)header.record_count);
    s_export_status_until_us = esp_timer_get_time() + 5000000;
    return ESP_OK;
}

const char *radar_capture_export_status(void) {
    if (s_export_status[0] == '\0' ||
        esp_timer_get_time() > s_export_status_until_us) {
        return "";
    }
    return s_export_status;
}
