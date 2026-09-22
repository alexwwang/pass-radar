#include "radar_capture.h"

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "radar_capture_codec.h"
#include "radar_capture_budget.h"
#include "radar_capture_batch.h"
#include "radar_diag_persist.h"

#include <esp_system.h>
#include <esp_random.h>

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
#define CAPTURE_BUFFER_MAX (sizeof(capture_storage_header_t) + RADAR_CAPTURE_CODEC_MAX_PAYLOAD)
#define CAPTURE_MEMORY_SAFETY_MARGIN RADAR_CAPTURE_BUDGET_SAFETY_MARGIN
#define CAPTURE_HEAP_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define CAPTURE_RECORD_BYTES \
    (RADAR_CAPTURE_MAX_RECORDS * sizeof(radar_capture_record_t))
#define CAPTURE_ALLOC_BYTES (CAPTURE_RECORD_BYTES + CAPTURE_BUFFER_MAX)

_Static_assert(sizeof(radar_capture_record_t) ==
                   RADAR_CAPTURE_BUDGET_RAW_RECORD_BYTES,
               "capture raw record budget changed");
_Static_assert(sizeof(capture_storage_header_t) ==
                   RADAR_CAPTURE_BUDGET_STORAGE_HEADER_BYTES,
               "capture storage header budget changed");

static void log_memory_budget(const char *stage, size_t required) {
    size_t free_bytes = heap_caps_get_free_size(CAPTURE_HEAP_CAPS);
    size_t largest = heap_caps_get_largest_free_block(CAPTURE_HEAP_CAPS);
    size_t minimum = heap_caps_get_minimum_free_size(CAPTURE_HEAP_CAPS);
    ESP_LOGI(TAG, "CAPTURE_MEM stage=%s free=%u largest=%u min=%u required=%u margin=%u",
             stage, (unsigned)free_bytes, (unsigned)largest, (unsigned)minimum,
             (unsigned)required, (unsigned)CAPTURE_MEMORY_SAFETY_MARGIN);
}

// Capture storage is allocated on demand, after radio/BLE initialization.
// Keeping these buffers out of BSS preserves the startup heap required by
// NimBLE; capture/export paths run in the LVGL task and are not concurrent.
static uint8_t *s_storage;
static radar_capture_record_t *s_records;
static uint8_t *s_scratch;

static bool ensure_storage(void) {
    if (!s_storage) {
        log_memory_budget("storage-before", CAPTURE_ALLOC_BYTES);
        size_t largest = heap_caps_get_largest_free_block(CAPTURE_HEAP_CAPS);
        if (largest < CAPTURE_ALLOC_BYTES + CAPTURE_MEMORY_SAFETY_MARGIN) {
            ESP_LOGE(TAG,
                     "Capture storage rejected: largest=%u required=%u margin=%u",
                     (unsigned)largest, (unsigned)CAPTURE_ALLOC_BYTES,
                     (unsigned)CAPTURE_MEMORY_SAFETY_MARGIN);
            return false;
        }
        s_storage = calloc(1, CAPTURE_ALLOC_BYTES);
        if (s_storage) {
            s_records = (radar_capture_record_t *)s_storage;
            s_scratch = s_storage + CAPTURE_RECORD_BYTES;
        }
    }
    if (!s_storage || !s_records || !s_scratch) {
        log_memory_budget("storage-failed", CAPTURE_ALLOC_BYTES);
        ESP_LOGE(TAG, "Capture storage allocation failed bytes=%u",
                 (unsigned)CAPTURE_ALLOC_BYTES);
        return false;
    }
    log_memory_budget("storage-after", 0);
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
static uint16_t s_scene_id = RADAR_CAPTURE_SCENE_O_LOS;
static uint8_t s_condition_obstruction = RADAR_CAPTURE_OBSTRUCTION_NONE;
static uint8_t s_room_geometry = RADAR_CAPTURE_ROOM_GEOMETRY_OUTDOOR_OPEN;
static uint8_t s_room_size = RADAR_CAPTURE_ROOM_SIZE_LARGE;
static uint8_t s_material = RADAR_CAPTURE_MATERIAL_MIXED;
static uint8_t s_furniture_density = RADAR_CAPTURE_FURNITURE_LOW;
static uint8_t s_boundary = RADAR_CAPTURE_BOUNDARY_OPEN;
static uint8_t s_card_height = RADAR_CAPTURE_CARD_HEIGHT_CHEST;
static uint8_t s_card_pose = RADAR_CAPTURE_CARD_POSE_UPRIGHT;
static uint8_t s_observed_level = RADAR_CAPTURE_OBS_LEVEL_L3_LOST;
static uint8_t s_observed_obstruction = RADAR_CAPTURE_OBS_CLASS_UNKNOWN;
// Screen feedback for the DOWN long-press re-export: "" = never attempted,
// otherwise a short status string cleared on the next Debug-page entry.
static char s_export_status[16];
static int64_t s_export_status_until_us;

const float radar_capture_targets_m[RADAR_CAPTURE_TARGET_COUNT] = {
    1.0f, 3.0f, 5.0f, 8.0f, 10.0f, 15.0f, 20.0f, 30.0f,
};

static uint16_t capture_cm(float value) {
    if (!(value >= 0.0f) || value > 655.34f) return RADAR_CAPTURE_INVALID_U16;
    return (uint16_t)(value * 100.0f + 0.5f);
}

static float capture_m(uint16_t value) {
    return value == RADAR_CAPTURE_INVALID_U16 ? -1.0f : (float)value / 100.0f;
}

static uint16_t capture_age(int32_t value) {
    if (value < 0 || value > 65534) return RADAR_CAPTURE_INVALID_U16;
    return (uint16_t)value;
}

static int32_t capture_age_ms(uint16_t value) {
    return value == RADAR_CAPTURE_INVALID_U16 ? -1 : (int32_t)value;
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

    size_t blob_size = CAPTURE_BUFFER_MAX;
    esp_err_t err = nvs_get_blob(handle, key, s_scratch, &blob_size);
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CAPTURE_READ key=%s nvs_err=%s", key, esp_err_to_name(err));
        return false;
    }
    if (blob_size < sizeof(*header) || blob_size > CAPTURE_BUFFER_MAX) {
        ESP_LOGW(TAG, "CAPTURE_READ key=%s blob=%u invalid_bounds", key,
                 (unsigned)blob_size);
        return false;
    }

    memcpy(header, s_scratch, sizeof(*header));
    if (!valid_header(header)) {
        ESP_LOGW(TAG, "CAPTURE_READ key=%s invalid_header codec=%u records=%lu raw=%lu",
                 key, (unsigned)header->codec_id,
                 (unsigned long)header->record_count,
                 (unsigned long)header->uncompressed_length);
        return false;
    }
    if (sizeof(*header) + header->compressed_length != blob_size ||
        header->compressed_length > payload_capacity) {
        ESP_LOGW(TAG, "CAPTURE_READ key=%s length_mismatch blob=%u compressed=%lu",
                 key, (unsigned)blob_size,
                 (unsigned long)header->compressed_length);
        return false;
    }
    memcpy(payload, s_scratch + sizeof(*header), header->compressed_length);
    uint32_t crc = radar_capture_codec_crc32(payload, header->compressed_length);
    if (crc != header->payload_crc32) {
        ESP_LOGW(TAG, "CAPTURE_READ key=%s crc actual=0x%08lx expected=0x%08lx",
                 key, (unsigned long)crc, (unsigned long)header->payload_crc32);
        return false;
    }
    return true;
}

static bool read_key_header(const char *key, capture_storage_header_t *header) {
    nvs_handle_t handle;
    if (!key || !header ||
        nvs_open(CAPTURE_NVS_NS, NVS_READONLY, &handle) != ESP_OK) return false;

    size_t blob_size = CAPTURE_BUFFER_MAX;
    esp_err_t err = nvs_get_blob(handle, key, s_scratch, &blob_size);
    nvs_close(handle);
    if (err != ESP_OK || blob_size < sizeof(*header) ||
        blob_size > CAPTURE_BUFFER_MAX) return false;

    memcpy(header, s_scratch, sizeof(*header));
    if (!valid_header(header) ||
        sizeof(*header) + header->compressed_length != blob_size) {
        ESP_LOGW(TAG, "CAPTURE_READ key=%s invalid_header codec=%u records=%lu raw=%lu",
                 key, (unsigned)header->codec_id,
                 (unsigned long)header->record_count,
                 (unsigned long)header->uncompressed_length);
        return false;
    }
    return true;
}

static bool read_latest(capture_storage_header_t *header, uint8_t *payload,
                        size_t payload_capacity) {
    capture_storage_header_t candidate;
    const char *best_key = NULL;
    uint32_t best_generation = 0;

    // Inspect headers first. A complete read of a second key would overwrite
    // s_scratch and could corrupt the payload belonging to the first key.
    for (unsigned i = 0; i < 2; ++i) {
        const char *key = i == 0 ? CAPTURE_NVS_KEY_A : CAPTURE_NVS_KEY_B;
        if (!read_key_header(key, &candidate)) continue;
        if (!best_key || candidate.generation > best_generation) {
            best_key = key;
            best_generation = candidate.generation;
            *header = candidate;
        }
    }
    return best_key && read_key(best_key, header, payload, payload_capacity);
}

static uint32_t latest_generation(void) {
    capture_storage_header_t header;
    return read_latest(&header, s_scratch + sizeof(capture_storage_header_t),
                       CAPTURE_BUFFER_MAX - sizeof(capture_storage_header_t))
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
                 record->ftm_status, (double)capture_m(record->ftm_distance_cm),
                 (double)capture_m(record->ftm_spread_cm), record->ftm_session_count,
                 (record->flags & 4u) != 0, (record->flags & 8u) != 0,
                 record->ble_rssi, (long)capture_age_ms(record->ble_age_ms),
                 (unsigned long)record->csi_frames, record->env_level);
    }
}

typedef struct {
    unsigned count;
} batch_export_context_t;

static void log_candidate_summary(void) {
    if (s_record_count == 0) return;
    int32_t rssi_sum = 0;
    int32_t ftm_sum_cm = 0;
    unsigned ftm_ok = 0;
    for (unsigned i = 0; i < s_record_count; ++i) {
        rssi_sum += s_records[i].peer_rssi;
        if (s_records[i].flags & 2u) {
            ftm_ok++;
            if (s_records[i].ftm_distance_cm != RADAR_CAPTURE_INVALID_U16) {
                ftm_sum_cm += s_records[i].ftm_distance_cm;
            }
        }
    }
    ESP_LOGI(TAG,
             "MODEL_CANDIDATE batch=%lu run=%lu scene=%s target_m=%.1f records=%u "
             "ftm_ok=%u ftm_rate_pct=%u rssi_mean=%d ftm_mean_cm=%ld",
             (unsigned long)radar_capture_batch_id(),
             (unsigned long)s_capture_id, radar_capture_scene_name(),
             (double)s_target_m, s_record_count,
             ftm_ok, (unsigned)(ftm_ok * 100u / s_record_count),
             (int)(rssi_sum / (int32_t)s_record_count),
             ftm_ok ? (long)(ftm_sum_cm / (int32_t)ftm_ok) : -1L);
}

static esp_err_t export_batch_record(
    const radar_capture_batch_capture_header_t *batch_header,
    const uint8_t *payload, size_t payload_length, void *context) {
    batch_export_context_t *export_context = context;
    if (!batch_header || !payload || !export_context ||
        !radar_capture_codec_decode(payload, payload_length, s_records,
                                    RADAR_CAPTURE_MAX_RECORDS,
                                    batch_header->record_count)) {
        ESP_LOGE(TAG, "BATCH_EXPORT decode failed");
        return ESP_ERR_INVALID_CRC;
    }

    capture_storage_header_t header = {
        .magic = CAPTURE_MAGIC,
        .version = CAPTURE_STORAGE_VERSION,
        .codec_id = RADAR_CAPTURE_CODEC_ID,
        .generation = batch_header->run_id,
        .target_distance_m = batch_header->target_distance_m,
        .record_count = batch_header->record_count,
        .uncompressed_length = batch_header->record_count *
                               sizeof(radar_capture_record_t),
        .compressed_length = payload_length,
        .capture_id = batch_header->capture_id,
        .boot_sequence = batch_header->boot_sequence,
        .reset_reason = batch_header->reset_reason,
        .payload_crc32 = batch_header->payload_crc32,
    };
    strncpy(header.firmware_version, "slot2-batch",
            sizeof(header.firmware_version) - 1u);
    ESP_LOGI(TAG,
             "BATCH_CAPTURE_CONDITION batch=%lu run=%lu scene=%s env=%u link=%u obs=%u "
             "prop=%u room=%u size=%u material=%u furniture=%u boundary=%u "
             "height=%u pose=%u observed_level=%u observed_obstruction=%u",
             (unsigned long)batch_header->batch_id,
             (unsigned long)batch_header->run_id,
             radar_capture_scene_label(batch_header->scene_id),
             (unsigned)batch_header->condition_environment,
             (unsigned)batch_header->condition_link,
             (unsigned)batch_header->condition_obstruction,
             (unsigned)batch_header->condition_propagation,
             (unsigned)batch_header->room_geometry,
             (unsigned)batch_header->room_size,
             (unsigned)batch_header->material,
             (unsigned)batch_header->furniture_density,
             (unsigned)batch_header->boundary,
             (unsigned)batch_header->card_height,
             (unsigned)batch_header->card_pose,
             (unsigned)batch_header->observed_level,
             (unsigned)batch_header->observed_obstruction);
    emit_csv("BATCH_CAPTURE_META", "BATCH_CAPTURE_CSV", &header, s_records);
    export_context->count++;
    return ESP_OK;
}

void radar_capture_init(void) {
    (void)radar_capture_batch_open();
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
    if (!ensure_storage()) {
        log_memory_budget("start-failed", CAPTURE_ALLOC_BYTES);
        ESP_LOGE(TAG, "CAPTURE_START FAIL RAM");
        return ESP_ERR_NO_MEM;
    }
    if (radar_capture_batch_id() == 0) {
        uint32_t batch_id = esp_random();
        if (batch_id == 0) batch_id = (uint32_t)esp_timer_get_time();
        esp_err_t batch_err = radar_capture_batch_begin(
            batch_id, RADAR_CAPTURE_BATCH_MAX_CAPTURES);
        if (batch_err != ESP_OK) {
            if (batch_err == ESP_ERR_NOT_FOUND) {
                ESP_LOGW(TAG, "BATCH unavailable; using NVS fallback");
            } else {
                ESP_LOGE(TAG, "CAPTURE_START FAIL BATCH err=%s",
                         esp_err_to_name(batch_err));
                return batch_err;
            }
        }
    }
    if (radar_capture_batch_id() != 0 &&
        !radar_capture_batch_can_append(RADAR_CAPTURE_CODEC_MAX_PAYLOAD)) {
        ESP_LOGE(TAG, "CAPTURE_START FAIL BATCH_SPACE next_capture_max=%u",
                 (unsigned)RADAR_CAPTURE_CODEC_MAX_PAYLOAD);
        return ESP_ERR_NO_MEM;
    }
    memset(s_records, 0,
           RADAR_CAPTURE_MAX_RECORDS * sizeof(*s_records));
    s_target_m = target_distance_m;
    s_start_us = esp_timer_get_time();
    s_last_sample_us = 0;
    s_record_count = 0;
    s_data_valid = false;
    s_save_succeeded = false;
    s_observed_level = RADAR_CAPTURE_OBS_LEVEL_L3_LOST;
    s_observed_obstruction = RADAR_CAPTURE_OBS_CLASS_UNKNOWN;
    // run_id must remain unique within the active batch across reboots.
    // Aborted runs are not committed, so the next start may reuse the next
    // committed ordinal without creating a visible gap.
    if (radar_capture_batch_id() != 0) {
        s_capture_id = radar_capture_batch_count() + 1u;
    } else {
        s_capture_id++;
    }
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
    record->elapsed_ms = capture_elapsed > UINT16_MAX
        ? UINT16_MAX : (uint16_t)capture_elapsed;
    record->local_rssi = observation->local_rssi;
    record->peer_rssi = observation->peer_rssi;
    record->flags = (observation->peer_alive ? 1u : 0u) |
                    (observation->ftm_valid ? 2u : 0u) |
                    (observation->ble_ready ? 4u : 0u) |
                    (observation->ble_peer_alive ? 8u : 0u);
    record->ftm_status = observation->ftm_status;
    record->ftm_distance_cm = capture_cm(observation->ftm_distance_m);
    record->ftm_spread_cm = capture_cm(observation->ftm_spread_m);
    record->ftm_session_count = observation->ftm_session_count;
    record->ble_rssi = observation->ble_rssi;
    record->ble_age_ms = capture_age(observation->ble_age_ms);
    record->csi_frames = observation->csi_frames > UINT16_MAX
        ? UINT16_MAX : (uint16_t)observation->csi_frames;
    record->env_level = observation->environment_level;
    s_observed_level = observation->observed_level;
    s_observed_obstruction = observation->observed_obstruction;
    s_last_sample_us = now;
}

radar_capture_phase_t radar_capture_phase(void) { return s_phase; }
float radar_capture_target_m(void) { return s_target_m; }

esp_err_t radar_capture_begin_new_batch(void) {
    uint32_t batch_id = esp_random();
    if (batch_id == 0) batch_id = (uint32_t)esp_timer_get_time();
    esp_err_t err = radar_capture_batch_erase();
    if (err == ESP_ERR_NOT_FOUND) return err;
    if (err != ESP_OK) return err;
    return radar_capture_batch_begin(batch_id, RADAR_CAPTURE_BATCH_MAX_CAPTURES);
}

void radar_capture_set_scene(uint16_t scene_id) {
    uint8_t environment = (uint8_t)radar_capture_scene_environment(scene_id);
    uint8_t link = (uint8_t)radar_capture_scene_link(scene_id);
    if (environment <= RADAR_CAPTURE_ENV_INDOOR &&
        link <= RADAR_CAPTURE_LINK_MULTIPATH &&
        scene_id == (uint16_t)((environment << 8) | (link << 4))) {
        s_scene_id = scene_id;
        switch (link) {
        case RADAR_CAPTURE_LINK_STATIC_ATTENUATION:
            s_condition_obstruction = RADAR_CAPTURE_OBSTRUCTION_WALL;
            break;
        case RADAR_CAPTURE_LINK_DYNAMIC_ATTENUATION:
            s_condition_obstruction = RADAR_CAPTURE_OBSTRUCTION_HUMAN_DYNAMIC;
            break;
        case RADAR_CAPTURE_LINK_MULTIPATH:
            s_condition_obstruction = RADAR_CAPTURE_OBSTRUCTION_METAL;
            break;
        default:
            s_condition_obstruction = RADAR_CAPTURE_OBSTRUCTION_NONE;
            break;
        }
        s_room_geometry = environment == RADAR_CAPTURE_ENV_INDOOR
            ? RADAR_CAPTURE_ROOM_GEOMETRY_ROOM
            : RADAR_CAPTURE_ROOM_GEOMETRY_OUTDOOR_OPEN;
        s_boundary = RADAR_CAPTURE_BOUNDARY_OPEN;
        s_material = RADAR_CAPTURE_MATERIAL_MIXED;
        s_furniture_density = environment == RADAR_CAPTURE_ENV_INDOOR
            ? RADAR_CAPTURE_FURNITURE_MEDIUM : RADAR_CAPTURE_FURNITURE_LOW;
    }
}

uint16_t radar_capture_scene(void) { return s_scene_id; }
const char *radar_capture_scene_name(void) {
    return radar_capture_scene_label(s_scene_id);
}

const char *radar_capture_context_name(void) {
    if (s_room_geometry == RADAR_CAPTURE_ROOM_GEOMETRY_CORRIDOR) return "CORRIDOR";
    if (s_room_geometry == RADAR_CAPTURE_ROOM_GEOMETRY_OPEN_HALL) return "OPEN_HALL";
    if (s_room_geometry == RADAR_CAPTURE_ROOM_GEOMETRY_CORNER) return "CORNER";
    if (s_material == RADAR_CAPTURE_MATERIAL_METAL_GLASS) return "METAL_GLASS";
    return s_room_geometry == RADAR_CAPTURE_ROOM_GEOMETRY_OUTDOOR_OPEN
        ? "OUTDOOR_OPEN" : "ROOM";
}

void radar_capture_set_obstacle(uint8_t obstacle) {
    (void)radar_capture_set_context_field(RADAR_CAPTURE_CONTEXT_OBSTACLE,
                                          obstacle);
}

bool radar_capture_set_context_field(radar_capture_context_field_t field,
                                     uint8_t value) {
    radar_capture_environment_id_t environment =
        radar_capture_scene_environment(s_scene_id);
    radar_capture_link_condition_id_t link = radar_capture_scene_link(s_scene_id);
    if (!radar_capture_context_option_available(environment, link, field, value)) {
        return false;
    }
    switch (field) {
    case RADAR_CAPTURE_CONTEXT_SPACE: s_room_geometry = value; break;
    case RADAR_CAPTURE_CONTEXT_ROOM_SIZE: s_room_size = value; break;
    case RADAR_CAPTURE_CONTEXT_OBSTACLE: s_condition_obstruction = value; break;
    case RADAR_CAPTURE_CONTEXT_MATERIAL: s_material = value; break;
    case RADAR_CAPTURE_CONTEXT_FURNITURE: s_furniture_density = value; break;
    case RADAR_CAPTURE_CONTEXT_BOUNDARY: s_boundary = value; break;
    case RADAR_CAPTURE_CONTEXT_CARD_HEIGHT: s_card_height = value; break;
    case RADAR_CAPTURE_CONTEXT_CARD_POSE: s_card_pose = value; break;
    default: return false;
    }
    return true;
}

uint8_t radar_capture_context_value(radar_capture_context_field_t field) {
    switch (field) {
    case RADAR_CAPTURE_CONTEXT_SPACE: return s_room_geometry;
    case RADAR_CAPTURE_CONTEXT_ROOM_SIZE: return s_room_size;
    case RADAR_CAPTURE_CONTEXT_OBSTACLE: return s_condition_obstruction;
    case RADAR_CAPTURE_CONTEXT_MATERIAL: return s_material;
    case RADAR_CAPTURE_CONTEXT_FURNITURE: return s_furniture_density;
    case RADAR_CAPTURE_CONTEXT_BOUNDARY: return s_boundary;
    case RADAR_CAPTURE_CONTEXT_CARD_HEIGHT: return s_card_height;
    case RADAR_CAPTURE_CONTEXT_CARD_POSE: return s_card_pose;
    default: return UINT8_MAX;
    }
}

void radar_capture_set_context(uint8_t room_geometry, uint8_t room_size,
                               uint8_t material, uint8_t furniture_density,
                               uint8_t boundary, uint8_t card_height,
                               uint8_t card_pose) {
    const uint8_t values[] = {room_geometry, room_size, material,
                              furniture_density, boundary, card_height,
                              card_pose};
    const radar_capture_context_field_t fields[] = {
        RADAR_CAPTURE_CONTEXT_SPACE, RADAR_CAPTURE_CONTEXT_ROOM_SIZE,
        RADAR_CAPTURE_CONTEXT_MATERIAL, RADAR_CAPTURE_CONTEXT_FURNITURE,
        RADAR_CAPTURE_CONTEXT_BOUNDARY, RADAR_CAPTURE_CONTEXT_CARD_HEIGHT,
        RADAR_CAPTURE_CONTEXT_CARD_POSE};
    radar_capture_environment_id_t environment =
        radar_capture_scene_environment(s_scene_id);
    radar_capture_link_condition_id_t link = radar_capture_scene_link(s_scene_id);
    for (unsigned i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        if (!radar_capture_context_option_available(environment, link,
                                                    fields[i], values[i])) {
            return;
        }
    }
    s_room_geometry = room_geometry;
    s_room_size = room_size;
    s_material = material;
    s_furniture_density = furniture_density;
    s_boundary = boundary;
    s_card_height = card_height;
    s_card_pose = card_pose;
}

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

    // Read the generation before encoding. latest_generation() uses s_scratch
    // as its NVS read buffer; calling it after encoding would overwrite the
    // payload while leaving the new header's lengths unchanged.
    uint32_t generation = latest_generation() + 1u;

    size_t payload_length = radar_capture_codec_encode(
        s_records, s_record_count,
        s_scratch + sizeof(capture_storage_header_t),
        CAPTURE_BUFFER_MAX - sizeof(capture_storage_header_t));
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
    if (blob_length > CAPTURE_BUFFER_MAX) {
        ESP_LOGE(TAG, "CAPTURE_SAVE FAIL NVS_SPACE blob=%u max=%u",
                 (unsigned)blob_length, (unsigned)CAPTURE_BUFFER_MAX);
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
                  CAPTURE_BUFFER_MAX - sizeof(capture_storage_header_t))) {
        ESP_LOGE(TAG, "CAPTURE_SAVE FAIL VERIFY READBACK");
        return ESP_ERR_INVALID_CRC;
    }
    if (verified.generation != generation) {
        ESP_LOGE(TAG, "CAPTURE_SAVE FAIL VERIFY GENERATION expected=%lu actual=%lu",
                 (unsigned long)generation, (unsigned long)verified.generation);
        return ESP_ERR_INVALID_CRC;
    }
    if (!radar_capture_codec_decode(s_scratch + sizeof(capture_storage_header_t),
                                    verified.compressed_length,
                                    s_records, RADAR_CAPTURE_MAX_RECORDS,
                                    verified.record_count)) {
        ESP_LOGE(TAG, "CAPTURE_SAVE FAIL VERIFY DECODE records=%lu compressed=%lu",
                 (unsigned long)verified.record_count,
                 (unsigned long)verified.compressed_length);
        return ESP_ERR_INVALID_CRC;
    }

    if (radar_capture_batch_id() == 0) {
        ESP_LOGW(TAG, "BATCH unavailable; CAPTURE_SAVE using NVS fallback");
        s_record_count = verified.record_count;
        s_target_m = verified.target_distance_m;
        log_candidate_summary();
        s_save_succeeded = true;
        emit_csv("CAPTURE_META", "CAPTURE_CSV", &verified, s_records);
        ESP_LOGI(TAG, "CAPTURE_SAVE OK NVS_FALLBACK target_m=%.1f records=%u",
                 (double)s_target_m, s_record_count);
        return ESP_OK;
    }

    radar_capture_batch_capture_meta_t batch_meta = {
        .batch_id = radar_capture_batch_id(),
        .run_id = s_capture_id,
        .capture_id = s_capture_id,
        .scene_id = s_scene_id,
        .condition_environment = (uint8_t)radar_capture_scene_environment(s_scene_id),
        .condition_link = (uint8_t)radar_capture_scene_link(s_scene_id),
        .condition_obstruction = s_condition_obstruction,
        .condition_propagation = (uint8_t)(radar_capture_scene_link(s_scene_id) ==
                                           RADAR_CAPTURE_LINK_LOS
                                               ? RADAR_CAPTURE_PROPAGATION_LOS
                                               : radar_capture_scene_link(s_scene_id) ==
                                                         RADAR_CAPTURE_LINK_MULTIPATH
                                                     ? RADAR_CAPTURE_PROPAGATION_MULTIPATH
                                                     : RADAR_CAPTURE_PROPAGATION_NLOS),
        .observed_level = s_observed_level,
        .observed_obstruction = s_observed_obstruction,
        .room_geometry = s_room_geometry,
        .room_size = s_room_size,
        .material = s_material,
        .furniture_density = s_furniture_density,
        .boundary = s_boundary,
        .card_height = s_card_height,
        .card_pose = s_card_pose,
        .target_distance_m = verified.target_distance_m,
        .measured_distance_m = -1.0f,
        .record_count = verified.record_count,
        .boot_sequence = verified.boot_sequence,
        .reset_reason = verified.reset_reason,
        .firmware_hash32 = 0,
    };
    esp_err_t batch_err = radar_capture_batch_append(
        &batch_meta, s_scratch + sizeof(capture_storage_header_t),
        verified.compressed_length, verified.payload_crc32);
    if (batch_err != ESP_OK) {
        ESP_LOGE(TAG, "CAPTURE_SAVE FAIL BATCH err=%s",
                 esp_err_to_name(batch_err));
        return batch_err;
    }

    s_record_count = verified.record_count;
    s_target_m = verified.target_distance_m;
    log_candidate_summary();
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
    if (radar_capture_batch_count() > 0) {
        batch_export_context_t export_context = {0};
        esp_err_t batch_err = radar_capture_batch_export(export_batch_record,
                                                          &export_context);
        if (batch_err != ESP_OK) {
            ESP_LOGW(TAG, "CAPTURE_EXPORT batch failed err=%s",
                     esp_err_to_name(batch_err));
            snprintf(s_export_status, sizeof(s_export_status), "EXPORT FAIL");
            s_export_status_until_us = esp_timer_get_time() + 5000000;
            return batch_err;
        }
        ESP_LOGI(TAG, "CAPTURE_EXPORT OK batch=%lu captures=%u",
                 (unsigned long)radar_capture_batch_id(), export_context.count);
        snprintf(s_export_status, sizeof(s_export_status), "EXPORT OK %u",
                 export_context.count);
        s_export_status_until_us = esp_timer_get_time() + 5000000;
        return ESP_OK;
    }
    if (!read_latest(&header, s_scratch + sizeof(capture_storage_header_t),
                     CAPTURE_BUFFER_MAX - sizeof(capture_storage_header_t))) {
        ESP_LOGW(TAG, "CAPTURE_EXPORT no valid saved record");
        snprintf(s_export_status, sizeof(s_export_status), "EXPORT NOREC");
        s_export_status_until_us = esp_timer_get_time() + 5000000;
        return ESP_ERR_NOT_FOUND;
    }
    if (!radar_capture_codec_decode(s_scratch + sizeof(capture_storage_header_t),
                                    header.compressed_length,
                                    s_records, RADAR_CAPTURE_MAX_RECORDS,
                                    header.record_count)) {
        ESP_LOGW(TAG, "CAPTURE_EXPORT decode failed records=%lu compressed=%lu",
                 (unsigned long)header.record_count,
                 (unsigned long)header.compressed_length);
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
