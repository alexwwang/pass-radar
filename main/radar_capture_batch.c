#include "radar_capture_batch.h"
#include "radar_capture_condition.h"

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "mbedtls/sha256.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

static const char *TAG = "radar_capture_batch";
static const esp_partition_t *s_partition;
static size_t s_data_size;
static uint32_t s_expected_captures;

static esp_err_t read_bytes(size_t offset, void *data, size_t length);
static bool s_open;
static bool s_active;
static uint32_t s_batch_id;
static uint32_t s_capture_count;
static size_t s_next_offset;

uint32_t radar_capture_batch_crc32(const void *data, size_t length) {
    const uint8_t *bytes = data;
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^
                  (0xedb88320u & (uint32_t)-(int)(crc & 1u));
        }
    }
    return ~crc;
}

bool radar_capture_batch_header_valid_cap(
    const radar_capture_batch_header_t *header, size_t capacity) {
    if (!header || header->magic != RADAR_CAPTURE_BATCH_MAGIC ||
        header->version < RADAR_CAPTURE_BATCH_FORMAT_VERSION_MIN ||
        header->version > RADAR_CAPTURE_BATCH_FORMAT_VERSION ||
        header->header_size != sizeof(*header) ||
        header->expected_captures > RADAR_CAPTURE_BATCH_MAX_CAPTURES ||
        header->committed_captures > header->expected_captures ||
        header->next_offset < radar_capture_batch_data_offset() ||
        (header->next_offset & (RADAR_CAPTURE_BATCH_SECTOR_SIZE - 1u)) != 0 ||
        header->next_offset > capacity) return false;
    radar_capture_batch_header_t copy = *header;
    uint32_t expected = copy.header_crc32;
    copy.header_crc32 = 0;
    return expected == radar_capture_batch_crc32(&copy, sizeof(copy));
}

bool radar_capture_batch_header_valid(const radar_capture_batch_header_t *header) {
    // Legacy fixed-capacity form: bounded by the v1 ota_2 fallback region.
    return radar_capture_batch_header_valid_cap(
        header, RADAR_CAPTURE_BATCH_FLASH_SIZE);
}

uint32_t radar_capture_batch_record_crc32(
    const radar_capture_batch_capture_header_t *header,
    const uint8_t *payload, size_t payload_length) {
    if (!header || (!payload && payload_length != 0)) return 0;
    radar_capture_batch_capture_header_t copy = *header;
    copy.record_crc32 = 0;
    copy.true_bearing = 0;  // operator annotation: outside integrity CRC
    return radar_capture_batch_crc32(&copy, sizeof(copy)) ^
           radar_capture_batch_crc32(payload, payload_length);
}

static bool capture_context_valid(uint8_t room_geometry, uint8_t room_size,
                                  uint8_t material, uint8_t furniture_density,
                                  uint8_t boundary, uint8_t card_height,
                                  uint8_t card_pose) {
    return room_geometry <= RADAR_CAPTURE_ROOM_GEOMETRY_OUTDOOR_OPEN &&
           room_size <= RADAR_CAPTURE_ROOM_SIZE_NOT_APPLICABLE &&
           material <= RADAR_CAPTURE_MATERIAL_METAL_GLASS &&
           furniture_density <= RADAR_CAPTURE_FURNITURE_HIGH &&
           boundary <= RADAR_CAPTURE_BOUNDARY_PARTITION &&
           card_height <= RADAR_CAPTURE_CARD_HEIGHT_OVERHEAD &&
           card_pose <= RADAR_CAPTURE_CARD_POSE_MIXED;
}

static bool scene_dimensions_valid(uint16_t scene_id, uint8_t environment,
                                   uint8_t link, uint8_t obstruction,
                                   uint8_t propagation) {
    if (environment > RADAR_CAPTURE_ENV_INDOOR ||
        link > RADAR_CAPTURE_LINK_MULTIPATH ||
        obstruction > RADAR_CAPTURE_OBSTRUCTION_METAL ||
        propagation > RADAR_CAPTURE_PROPAGATION_MULTIPATH) return false;
    uint16_t encoded = (uint16_t)((environment << 8) | (link << 4));
    return scene_id == encoded &&
           propagation == (uint8_t)radar_capture_condition_propagation(
               (radar_capture_link_condition_id_t)link);
}

bool radar_capture_batch_capture_header_valid(
    const radar_capture_batch_capture_header_t *header) {
    if (!header || header->magic != RADAR_CAPTURE_BATCH_CAPTURE_MAGIC ||
        header->version < RADAR_CAPTURE_BATCH_FORMAT_VERSION_MIN ||
        header->version > RADAR_CAPTURE_BATCH_FORMAT_VERSION ||
        header->header_size != sizeof(*header)) return false;
    if (header->version >= RADAR_CAPTURE_BATCH_FORMAT_VERSION) {
        // v4 原始 session 聚合字段范围校验。
        if (!isfinite(header->ftm_raw_median_m) ||
            header->ftm_raw_median_m < 0.0f ||
            header->ftm_raw_median_m > 1000.0f ||
            !isfinite(header->ftm_raw_iqr_m) ||
            header->ftm_raw_iqr_m < 0.0f ||
            (header->ftm_raw_sessions == 0u &&
             (header->ftm_raw_bursts != 0u || header->ftm_raw_median_m != 0.0f)) ||        (header->ftm_raw_sessions > 0u &&
         (header->ftm_raw_bursts == 0u ||
          header->ftm_raw_median_m <= 0.0f))) return false;
    }
    // v5: bearing annotation may be a valid sector or UNSET (v4 = 0xFF).
    if (header->true_bearing > RADAR_CAPTURE_BEARING_UNSET) return false;
    return header->batch_id != 0 && header->run_id != 0 &&
           header->batch_id != 0 && header->run_id != 0 &&
           scene_dimensions_valid(header->scene_id, header->condition_environment,
                                  header->condition_link,
                                  header->condition_obstruction,
                                  header->condition_propagation) &&
           header->condition_schema_version == RADAR_CAPTURE_CONDITION_SCHEMA_VERSION &&
           radar_capture_context_option_available(
               header->condition_environment, header->condition_link,
               RADAR_CAPTURE_CONTEXT_OBSTACLE, header->condition_obstruction) &&
           radar_capture_context_option_available(
               header->condition_environment, header->condition_link,
               RADAR_CAPTURE_CONTEXT_SPACE, header->room_geometry) &&
           radar_capture_context_option_available_for_space(
               header->condition_environment, header->condition_link,
               RADAR_CAPTURE_CONTEXT_ROOM_SIZE, header->room_size,
               (radar_capture_room_geometry_t)header->room_geometry) &&
           radar_capture_context_option_available(
               header->condition_environment, header->condition_link,
               RADAR_CAPTURE_CONTEXT_FURNITURE, header->furniture_density) &&
           header->observed_level <= RADAR_CAPTURE_OBS_LEVEL_L3_LOST &&
           header->observed_obstruction <= RADAR_CAPTURE_OBS_CLASS_UNKNOWN &&
           capture_context_valid(header->room_geometry, header->room_size,
                                header->material, header->furniture_density,
                                header->boundary, header->card_height,
                                header->card_pose) &&
           isfinite(header->target_distance_m) &&
           header->target_distance_m > 0.0f &&
           header->target_distance_m <= 655.34f &&
           isfinite(header->measured_distance_m) &&
           (header->measured_distance_m < 0.0f ||
            header->measured_distance_m <= 655.34f) &&
           header->record_count > 0 &&
           header->record_count <= RADAR_CAPTURE_BATCH_MAX_RECORDS &&
           header->payload_length > 0 &&
           header->payload_length <= RADAR_CAPTURE_BATCH_MAX_PAYLOAD;
}

static bool is_erased(const uint8_t *data, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        if (data[i] != 0xffu) return false;
    }
    return true;
}

static esp_err_t discard_uncommitted_tail(size_t offset) {
    if (!s_partition || offset > s_data_size ||
        (offset & (RADAR_CAPTURE_BATCH_SECTOR_SIZE - 1u)) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return esp_partition_erase_range(s_partition, offset, s_data_size - offset);
}

static esp_err_t scan_committed_records(void) {
    size_t offset = radar_capture_batch_data_offset();
    uint32_t count = 0;
    radar_capture_batch_capture_header_t capture;
    radar_capture_batch_commit_t commit;
    uint8_t *payload = malloc(RADAR_CAPTURE_BATCH_MAX_PAYLOAD);
    if (!payload) return ESP_ERR_NO_MEM;

    while (count < s_expected_captures &&
           offset + sizeof(capture) + sizeof(commit) <= s_data_size) {
        esp_err_t err = read_bytes(offset, &capture, sizeof(capture));
        if (err != ESP_OK || is_erased((const uint8_t *)&capture, sizeof(capture))) {
            break;
        }
        if (!radar_capture_batch_capture_header_valid(&capture) ||
            capture.batch_id != s_batch_id || capture.run_id != count + 1u ||
            offset + radar_capture_batch_capture_span(capture.payload_length) >
                s_data_size) {
            err = discard_uncommitted_tail(offset);
            s_capture_count = count;
            s_next_offset = offset;
            free(payload);
            return err;
        }
        size_t commit_offset = radar_capture_batch_commit_offset(
            offset, capture.payload_length);
        err = read_bytes(commit_offset, &commit, sizeof(commit));
        if (err != ESP_OK || commit.magic != RADAR_CAPTURE_BATCH_COMMIT_MAGIC ||
            commit.batch_id != capture.batch_id || commit.run_id != capture.run_id) {
            err = discard_uncommitted_tail(offset);
            s_capture_count = count;
            s_next_offset = offset;
            free(payload);
            return err;
        }
        if (commit.capture_crc32 != capture.record_crc32) {
            free(payload);
            return ESP_ERR_INVALID_CRC;
        }
        err = read_bytes(offset + sizeof(capture), payload,
                         capture.payload_length);
        if (err != ESP_OK ||
            radar_capture_batch_crc32(payload, capture.payload_length) !=
                capture.payload_crc32 ||
            radar_capture_batch_record_crc32(&capture, payload,
                                              capture.payload_length) !=
                capture.record_crc32) {
            free(payload);
            return ESP_ERR_INVALID_CRC;
        }
        offset += radar_capture_batch_capture_span(capture.payload_length);
        ++count;
    }
    free(payload);
    s_capture_count = count;
    s_next_offset = offset;
    return ESP_OK;
}

static bool partition_data_is_erased(const esp_partition_t *partition,
                                      size_t data_size) {
    // This function runs during radar_capture_init() on the small ESP-IDF
    // main task stack. Never place a full erase sector on that stack.
    static uint8_t probe[RADAR_CAPTURE_BATCH_SECTOR_SIZE];
    if (!partition || data_size < sizeof(probe) || data_size > partition->size ||
        (data_size & (RADAR_CAPTURE_BATCH_SECTOR_SIZE - 1u)) != 0) {
        return false;
    }
    for (size_t offset = 0; offset < data_size; offset += sizeof(probe)) {
        if (esp_partition_read(partition, offset, probe, sizeof(probe)) != ESP_OK) {
            return false;
        }
        for (size_t i = 0; i < sizeof(probe); ++i) {
            if (probe[i] != 0xffu) return false;
        }
    }
    return true;
}

// Accept either an erased region or one already initialized by this batch
// format. Any other content is treated as an occupied partition and is never
// erased or appended over.
static esp_err_t accept_data_partition(const esp_partition_t *partition,
                                       size_t data_size) {
    if (!partition_data_is_erased(partition, data_size)) {
        radar_capture_batch_header_t existing;
        if (esp_partition_read(partition, 0, &existing, sizeof(existing)) != ESP_OK ||
            !radar_capture_batch_header_valid_cap(&existing, data_size)) {
            ESP_LOGE(TAG, "BATCH data unavailable: partition is occupied");
            return ESP_ERR_INVALID_STATE;
        }
    }
    return ESP_OK;
}

static esp_err_t find_partition(void) {
    if (s_partition) return ESP_OK;

    // 1. meta-pass v2 (dynslot): the launcher materializes the child-declared
    // DATA carve under label "capture" when booting this firmware. The full
    // extent is usable; capacity is the carve size.
    const esp_partition_t *data = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, RADAR_CAPTURE_BATCH_DATA_SUBTYPE,
        RADAR_CAPTURE_BATCH_DATA_LABEL);
    if (data) {
        // Usable floor: batch header sector plus one full worst-case capture
        // span (padded header + payload + commit sectors).
        size_t floor = radar_capture_batch_data_offset() +
                       radar_capture_batch_capture_span(
                           RADAR_CAPTURE_BATCH_MAX_PAYLOAD);
        if (data->size < floor) {
            ESP_LOGE(TAG, "BATCH data unavailable: capture partition too small");
            return ESP_ERR_INVALID_STATE;
        }
        esp_err_t err = accept_data_partition(data, data->size);
        if (err != ESP_OK) return err;
        s_partition = data;
        s_data_size = data->size;
        ESP_LOGI(TAG, "BATCH data partition=%s address=0x%lx size=%u",
                 RADAR_CAPTURE_BATCH_DATA_LABEL,
                 (unsigned long)s_partition->address, (unsigned)s_data_size);
        return ESP_OK;
    }

    // 2. Legacy meta-pass v1: an empty, non-running ota_2 slot. Its final
    // 4 KiB holds the slot display-name blob and stays untouched.
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *slot2 = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_2, NULL);
    if (!slot2 || slot2->size <= RADAR_CAPTURE_BATCH_NAME_SECTOR_SIZE ||
        (running && slot2->address == running->address)) {
        ESP_LOGE(TAG, "BATCH data unavailable: slot2 is missing or running");
        return slot2 ? ESP_ERR_INVALID_STATE : ESP_ERR_NOT_FOUND;
    }
    size_t data_size = slot2->size - RADAR_CAPTURE_BATCH_NAME_SECTOR_SIZE;
    esp_err_t err = accept_data_partition(slot2, data_size);
    if (err != ESP_OK) return err;
    s_partition = slot2;
    s_data_size = data_size;
    if (s_data_size > RADAR_CAPTURE_BATCH_FLASH_SIZE) {
        s_data_size = RADAR_CAPTURE_BATCH_FLASH_SIZE;
    }
    ESP_LOGI(TAG, "BATCH data partition=ota_2 address=0x%lx size=%u",
             (unsigned long)s_partition->address, (unsigned)s_data_size);
    return ESP_OK;
}

static esp_err_t write_aligned(size_t offset, const void *data, size_t length) {
    if (!data || !s_partition || offset > s_data_size ||
        length > s_data_size - offset || (offset & 3u) != 0 ||
        (length & 3u) != 0) return ESP_ERR_INVALID_ARG;
    return esp_partition_write(s_partition, offset, data, length);
}

static esp_err_t read_bytes(size_t offset, void *data, size_t length) {
    if (!data || !s_partition || offset > s_data_size ||
        length > s_data_size - offset) {
        return ESP_ERR_INVALID_ARG;
    }
    return esp_partition_read(s_partition, offset, data, length);
}

// Padded writes fill the alignment padding with 0xFF, never 0x00: NOR flash
// can only clear bits (1 -> 0). The header block [offset, offset+4096) and
// the payload block [offset+sizeof(header), offset+sizeof(header)+4096)
// overlap when sizeof(header) < 4096, so a zero-filled header padding would
// poison the flash bytes the payload needs to write afterwards; AND-write
// semantics would then corrupt the payload silently and its CRC check would
// fail on export (observed on device: append OK, export INVALID_CRC).
// Padding with 0xFF keeps every byte writable because 0xFF & x == x.
static esp_err_t write_padded(size_t offset, const void *data, size_t length) {
    size_t padded = radar_capture_batch_align(length);
    uint8_t *buffer = malloc(padded);
    if (!buffer) return ESP_ERR_NO_MEM;
    memset(buffer, 0xff, padded);
    memcpy(buffer, data, length);
    esp_err_t err = write_aligned(offset, buffer, padded);
    free(buffer);
    return err;
}

esp_err_t radar_capture_device_hash(uint8_t out[16]) {
    if (!out) return ESP_ERR_INVALID_ARG;
    uint8_t id[16];
    // label + SUBTYPE_ANY:独立安装(partitions.csv subtype=nvs)与 meta-pass
    // 子固件(carve 物化 subtype=0x02)同 label 不同 subtype,都能命中。
    const esp_partition_t *cardid = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "cardid");
    if (!cardid || esp_partition_read(cardid, 0, id, sizeof(id)) != ESP_OK) {
        memset(id, 0xFF, sizeof(id));   // 未 provisioned 开发板:确定性"未身份"输入
    }
    uint8_t digest[32];
    if (mbedtls_sha256(id, sizeof(id), digest, 0) != 0) return ESP_FAIL;
    memcpy(out, digest, 16);
    return ESP_OK;
}

size_t radar_capture_batch_data_size(void) {
    if (find_partition() != ESP_OK) return 0;
    return s_data_size;
}

esp_err_t radar_capture_batch_data_read(size_t offset, void *dst, size_t length) {
    if (!dst && length) return ESP_ERR_INVALID_ARG;
    if (find_partition() != ESP_OK) return ESP_ERR_NOT_FOUND;
    if (offset > s_data_size || length > s_data_size - offset) {
        return ESP_ERR_INVALID_SIZE;
    }
    return read_bytes(offset, dst, length);
}

esp_err_t radar_capture_batch_open(void) {
    esp_err_t err = find_partition();
    if (err != ESP_OK) return err;
    s_open = true;
    radar_capture_batch_header_t header;
    err = read_bytes(0, &header, sizeof(header));
    if (err != ESP_OK ||
        !radar_capture_batch_header_valid_cap(&header, s_data_size)) {
        s_active = false;
        s_batch_id = 0;
        s_expected_captures = 0;
        s_capture_count = 0;
        s_next_offset = radar_capture_batch_data_offset();
        return ESP_OK;
    }
    s_active = true;
    s_batch_id = header.batch_id;
    s_expected_captures = header.expected_captures;
    err = scan_committed_records();
    if (err != ESP_OK) {
        s_active = false;
        ESP_LOGE(TAG, "batch recovery rejected err=%s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "batch open id=%lu committed=%lu next=%u",
             (unsigned long)s_batch_id, (unsigned long)s_capture_count,
             (unsigned)s_next_offset);
    return ESP_OK;
}

esp_err_t radar_capture_batch_begin(uint32_t batch_id, uint32_t expected_captures) {
    if (!s_open) {
        esp_err_t err = radar_capture_batch_open();
        if (err != ESP_OK) return err;
    }
    if (batch_id == 0 || expected_captures == 0 ||
        expected_captures > RADAR_CAPTURE_BATCH_MAX_CAPTURES) {
        return ESP_ERR_INVALID_ARG;
    }
    // Capacity is partition-derived (dynslot carve size or legacy ota_2). Every
    // capture costs 3 erase sectors even for small payloads (padded header,
    // payload, commit), so a small carve cannot honor the requested count.
    // Clamp instead of failing: capture stays usable, the batch just ends
    // earlier. Callers pass RADAR_CAPTURE_BATCH_MAX_CAPTURES unconditionally.
    size_t span_max =
        radar_capture_batch_capture_span(RADAR_CAPTURE_BATCH_MAX_PAYLOAD);
    uint32_t capacity_captures =
        span_max != 0 ? (uint32_t)((s_data_size -
                                    radar_capture_batch_data_offset()) /
                                   span_max)
                      : 0u;
    if (capacity_captures == 0) return ESP_ERR_NO_MEM;
    if (expected_captures > capacity_captures) {
        ESP_LOGW(TAG, "batch id=%lu expected=%lu clamped to %lu (capacity=%u)",
                 (unsigned long)batch_id, (unsigned long)expected_captures,
                 (unsigned long)capacity_captures, (unsigned)s_data_size);
        expected_captures = capacity_captures;
    }
    esp_err_t err = esp_partition_erase_range(s_partition, 0,
                                               s_data_size);
    if (err != ESP_OK) return err;

    radar_capture_batch_header_t header = {
        .magic = RADAR_CAPTURE_BATCH_MAGIC,
        .version = RADAR_CAPTURE_BATCH_FORMAT_VERSION,
        .header_size = sizeof(header),
        .batch_id = batch_id,
        .expected_captures = expected_captures,
        .committed_captures = 0,
        .next_offset = radar_capture_batch_data_offset(),
    };
    // v6+: 批次头盖设备身份哈希(cardid 明文不出设备)。失败不阻塞采集,
    // 头部记全零 = "未身份" 批次。
    if (radar_capture_device_hash(header.device_hash) != ESP_OK) {
        memset(header.device_hash, 0, sizeof(header.device_hash));
    }
    header.header_crc32 = radar_capture_batch_crc32(&header, sizeof(header));
    // Recompute with the checksum field zeroed, then store the checksum.
    header.header_crc32 = 0;
    header.header_crc32 = radar_capture_batch_crc32(&header, sizeof(header));
    err = write_aligned(0, &header, sizeof(header));
    if (err != ESP_OK) return err;
    s_active = true;
    s_batch_id = batch_id;
    s_expected_captures = expected_captures;
    s_capture_count = 0;
    s_next_offset = radar_capture_batch_data_offset();
    ESP_LOGI(TAG, "batch begin id=%lu expected=%lu",
             (unsigned long)batch_id, (unsigned long)expected_captures);
    return ESP_OK;
}

bool radar_capture_batch_can_append(size_t max_payload_length) {
    if (!s_open || !s_active || max_payload_length == 0 ||
        s_capture_count >= s_expected_captures ||
        s_capture_count >= RADAR_CAPTURE_BATCH_MAX_CAPTURES ||
        s_next_offset > s_data_size) return false;
    size_t span = radar_capture_batch_capture_span(max_payload_length);
    return span != 0 && span <= s_data_size - s_next_offset;
}

esp_err_t radar_capture_batch_append(const radar_capture_batch_capture_meta_t *meta,
                                     const uint8_t *payload, size_t payload_length,
                                     uint32_t payload_crc32) {
    if (!s_open || !s_active || !meta || !payload ||
        meta->batch_id != s_batch_id || meta->run_id != s_capture_count + 1u ||
        !scene_dimensions_valid(meta->scene_id, meta->condition_environment,
                                meta->condition_link,
                                meta->condition_obstruction,
                                meta->condition_propagation) ||
        meta->condition_schema_version != RADAR_CAPTURE_CONDITION_SCHEMA_VERSION ||
        !radar_capture_context_option_available(
            meta->condition_environment, meta->condition_link,
            RADAR_CAPTURE_CONTEXT_OBSTACLE, meta->condition_obstruction) ||
        !radar_capture_context_option_available(
            meta->condition_environment, meta->condition_link,
            RADAR_CAPTURE_CONTEXT_SPACE, meta->room_geometry) ||
        !radar_capture_context_option_available_for_space(
            meta->condition_environment, meta->condition_link,
            RADAR_CAPTURE_CONTEXT_ROOM_SIZE, meta->room_size,
            (radar_capture_room_geometry_t)meta->room_geometry) ||
        !radar_capture_context_option_available(
            meta->condition_environment, meta->condition_link,
            RADAR_CAPTURE_CONTEXT_FURNITURE, meta->furniture_density) ||
        meta->observed_level > RADAR_CAPTURE_OBS_LEVEL_L3_LOST ||
        meta->observed_obstruction > RADAR_CAPTURE_OBS_CLASS_UNKNOWN ||
        !capture_context_valid(meta->room_geometry, meta->room_size,
                               meta->material, meta->furniture_density,
                               meta->boundary, meta->card_height,
                               meta->card_pose) ||
        !isfinite(meta->target_distance_m) ||
 meta->target_distance_m <= 0.0f ||
        meta->target_distance_m > 655.34f ||
        !isfinite(meta->measured_distance_m) ||
        (meta->measured_distance_m >= 0.0f && meta->measured_distance_m > 655.34f) ||
        meta->record_count == 0 ||
        meta->record_count > RADAR_CAPTURE_BATCH_MAX_RECORDS ||
        payload_length == 0 || payload_length > RADAR_CAPTURE_BATCH_MAX_PAYLOAD ||
        payload_crc32 != radar_capture_batch_crc32(payload, payload_length) ||
        s_capture_count >= s_expected_captures ||
        s_capture_count >= RADAR_CAPTURE_BATCH_MAX_CAPTURES ||
        s_next_offset > s_data_size ||
        radar_capture_batch_capture_span(payload_length) == 0 ||
        radar_capture_batch_capture_span(payload_length) >
            s_data_size - s_next_offset) return ESP_ERR_INVALID_STATE;

    radar_capture_batch_capture_header_t header = {
        .magic = RADAR_CAPTURE_BATCH_CAPTURE_MAGIC,
        .version = RADAR_CAPTURE_BATCH_FORMAT_VERSION,
        .header_size = sizeof(header),
        .batch_id = meta->batch_id,
        .run_id = meta->run_id,
        .capture_id = meta->capture_id,
        .scene_id = meta->scene_id,
        .condition_environment = meta->condition_environment,
        .condition_link = meta->condition_link,
        .condition_obstruction = meta->condition_obstruction,
        .condition_schema_version = meta->condition_schema_version,
        .condition_propagation = meta->condition_propagation,
        .observed_level = meta->observed_level,
        .observed_obstruction = meta->observed_obstruction,
        .room_geometry = meta->room_geometry,
        .room_size = meta->room_size,
        .material = meta->material,
        .furniture_density = meta->furniture_density,
        .boundary = meta->boundary,
        .card_height = meta->card_height,
        .card_pose = meta->card_pose,
        .true_bearing = meta->true_bearing,
        .target_distance_m = meta->target_distance_m,
        .measured_distance_m = meta->measured_distance_m,
        .record_count = meta->record_count,
        .payload_length = payload_length,
        .payload_crc32 = payload_crc32,
        .firmware_hash32 = meta->firmware_hash32,
        .boot_sequence = meta->boot_sequence,
        .reset_reason = meta->reset_reason,
        .ftm_raw_median_m = meta->ftm_raw_median_m,
        .ftm_raw_iqr_m = meta->ftm_raw_iqr_m,
        .ftm_raw_sessions = meta->ftm_raw_sessions,
        .ftm_raw_bursts = meta->ftm_raw_bursts,
    };
    header.record_crc32 = radar_capture_batch_record_crc32(
        &header, payload, payload_length);

    size_t offset = s_next_offset;
    esp_err_t err = write_padded(offset, &header, sizeof(header));
    if (err == ESP_OK) err = write_padded(offset + sizeof(header), payload,
                                           payload_length);
    if (err != ESP_OK) return err;

    radar_capture_batch_commit_t commit = {
        .magic = RADAR_CAPTURE_BATCH_COMMIT_MAGIC,
        .batch_id = meta->batch_id,
        .run_id = meta->run_id,
        .capture_crc32 = header.record_crc32,
    };
    size_t commit_offset = radar_capture_batch_commit_offset(
        offset, payload_length);
    err = write_padded(commit_offset, &commit, sizeof(commit));
    if (err != ESP_OK) return err;

    s_capture_count++;
    s_next_offset += radar_capture_batch_capture_span(payload_length);
    size_t free_bytes = s_data_size - s_next_offset;
    if (free_bytes * 10u <= s_data_size) {
        ESP_LOGW(TAG, "BATCH_SPACE_WARN free=%u total=%u percent=%u",
                 (unsigned)free_bytes, (unsigned)s_data_size,
                 (unsigned)(free_bytes * 100u / s_data_size));
    }
    ESP_LOGI(TAG, "batch append id=%lu run=%lu count=%lu bytes=%u",
             (unsigned long)s_batch_id, (unsigned long)meta->run_id,
             (unsigned long)s_capture_count, (unsigned)s_next_offset);
    return ESP_OK;
}

esp_err_t radar_capture_batch_export(radar_capture_batch_record_cb callback,
                                      void *context) {
    if (!s_open || !s_active || !callback) return ESP_ERR_INVALID_ARG;

    size_t offset = radar_capture_batch_data_offset();
    uint32_t exported = 0;
    radar_capture_batch_capture_header_t header;
    radar_capture_batch_commit_t commit;
    uint8_t *payload = NULL;
    size_t payload_capacity = 0;

    while (exported < s_expected_captures &&
           offset + sizeof(header) + sizeof(commit) <= s_data_size) {
        esp_err_t err = read_bytes(offset, &header, sizeof(header));
        if (err != ESP_OK || !radar_capture_batch_capture_header_valid(&header) ||
            header.batch_id != s_batch_id || header.run_id != exported + 1u) break;
        size_t commit_offset = radar_capture_batch_commit_offset(
            offset, header.payload_length);
        if (commit_offset + sizeof(commit) > s_data_size) break;
        if (header.payload_length > payload_capacity) {
            uint8_t *next = realloc(payload, header.payload_length);
            if (!next) {
                free(payload);
                return ESP_ERR_NO_MEM;
            }
            payload = next;
            payload_capacity = header.payload_length;
        }
        err = read_bytes(offset + sizeof(header), payload, header.payload_length);
        if (err != ESP_OK ||
            radar_capture_batch_crc32(payload, header.payload_length) !=
                header.payload_crc32) break;
        err = read_bytes(commit_offset, &commit, sizeof(commit));
        if (err != ESP_OK ||
            radar_capture_batch_record_crc32(&header, payload,
                                              header.payload_length) !=
                header.record_crc32 ||
            commit.magic != RADAR_CAPTURE_BATCH_COMMIT_MAGIC ||
            commit.batch_id != header.batch_id || commit.run_id != header.run_id ||
            commit.capture_crc32 != header.record_crc32) break;
        err = callback(&header, payload, header.payload_length, context);
        if (err != ESP_OK) {
            free(payload);
            return err;
        }
        ++exported;
        offset += radar_capture_batch_capture_span(header.payload_length);
    }
    free(payload);
    ESP_LOGI(TAG, "BATCH_EXPORT id=%lu captures=%lu used=%u",
             (unsigned long)s_batch_id, (unsigned long)exported,
             (unsigned)offset);
    return exported == s_capture_count ? ESP_OK : ESP_ERR_INVALID_CRC;
}

esp_err_t radar_capture_batch_erase(void) {
    if (!s_open) {
        esp_err_t err = radar_capture_batch_open();
        if (err != ESP_OK) return err;
    }
    esp_err_t err = esp_partition_erase_range(s_partition, 0,
                                               s_data_size);
    if (err == ESP_OK) {
        s_active = false;
        s_batch_id = 0;
        s_expected_captures = 0;
        s_capture_count = 0;
        s_next_offset = radar_capture_batch_data_offset();
    }
    return err;
}

const char *radar_capture_batch_space_status(void) {
    static char status[32];
    if (!s_open || !s_active) return "BATCH N/A";
    size_t free_bytes = s_data_size - s_next_offset;
    unsigned percent = (unsigned)(free_bytes * 100u / s_data_size);
    snprintf(status, sizeof(status), "%s %u%%",
             percent <= 10u ? "BATCH LOW" : "BATCH", percent);
    return status;
}

uint32_t radar_capture_batch_id(void) { return s_batch_id; }
uint32_t radar_capture_batch_count(void) { return s_capture_count; }

uint16_t radar_capture_scene_menu_id(unsigned index) {
    static const uint16_t ids[RADAR_CAPTURE_BATCH_MENU_SCENE_MAX] = {
        RADAR_CAPTURE_SCENE_I_LOS, RADAR_CAPTURE_SCENE_I_STATIC,
        RADAR_CAPTURE_SCENE_I_DYNAMIC, RADAR_CAPTURE_SCENE_I_MULTIPATH,
        RADAR_CAPTURE_SCENE_O_LOS, RADAR_CAPTURE_SCENE_O_STATIC,
        RADAR_CAPTURE_SCENE_O_DYNAMIC, RADAR_CAPTURE_SCENE_O_MULTIPATH,
    };
    return index < RADAR_CAPTURE_BATCH_MENU_SCENE_MAX ? ids[index]
                                                       : ids[0];
}

unsigned radar_capture_scene_menu_count(void) {
    return RADAR_CAPTURE_BATCH_MENU_SCENE_MAX;
}

const char *radar_capture_environment_label(uint8_t environment) {
    return environment == RADAR_CAPTURE_ENV_INDOOR ? "INDOOR" :
           environment == RADAR_CAPTURE_ENV_OUTDOOR ? "OUTDOOR" : "UNKNOWN";
}

const char *radar_capture_bearing_label(uint8_t bearing) {
    switch (bearing) {
    case RADAR_CAPTURE_BEARING_FRONT: return "FRONT";
    case RADAR_CAPTURE_BEARING_FRONT_RIGHT: return "FRONT-RIGHT";
    case RADAR_CAPTURE_BEARING_RIGHT: return "RIGHT";
    case RADAR_CAPTURE_BEARING_REAR_RIGHT: return "REAR-RIGHT";
    case RADAR_CAPTURE_BEARING_REAR: return "REAR";
    case RADAR_CAPTURE_BEARING_REAR_LEFT: return "REAR-LEFT";
    case RADAR_CAPTURE_BEARING_LEFT: return "LEFT";
    case RADAR_CAPTURE_BEARING_FRONT_LEFT: return "FRONT-LEFT";
    default: return "UNSET";
    }
}

const char *radar_capture_link_label(uint8_t link) {
    switch (link) {
    case RADAR_CAPTURE_LINK_LOS: return "LOS";
    case RADAR_CAPTURE_LINK_STATIC_ATTENUATION: return "STATIC_ATTEN";
    case RADAR_CAPTURE_LINK_DYNAMIC_ATTENUATION: return "DYNAMIC_ATTEN";
    case RADAR_CAPTURE_LINK_MULTIPATH: return "MULTIPATH";
    default: return "UNKNOWN";
    }
}

const char *radar_capture_obstacle_label(uint8_t obstacle) {
    switch (obstacle) {
    case RADAR_CAPTURE_OBSTRUCTION_NONE: return "NONE";
    case RADAR_CAPTURE_OBSTRUCTION_HUMAN_STATIC: return "HUMAN_STATIC";
    case RADAR_CAPTURE_OBSTRUCTION_HUMAN_DYNAMIC: return "HUMAN_DYNAMIC";
    case RADAR_CAPTURE_OBSTRUCTION_FOLIAGE: return "FOLIAGE";
    case RADAR_CAPTURE_OBSTRUCTION_WALL: return "WALL";
    case RADAR_CAPTURE_OBSTRUCTION_METAL: return "METAL";
    default: return "UNKNOWN";
    }
}

const char *radar_capture_material_label(uint8_t material) {
    switch (material) {
    case RADAR_CAPTURE_MATERIAL_SOFT: return "SOFT";
    case RADAR_CAPTURE_MATERIAL_MIXED: return "MIXED";
    case RADAR_CAPTURE_MATERIAL_HARD: return "HARD";
    case RADAR_CAPTURE_MATERIAL_METAL_GLASS: return "METAL_GLASS";
    default: return "UNKNOWN";
    }
}

const char *radar_capture_boundary_label(uint8_t boundary) {
    switch (boundary) {
    case RADAR_CAPTURE_BOUNDARY_OPEN: return "OPEN >2m";
    case RADAR_CAPTURE_BOUNDARY_WALL_NEAR: return "WALL <=2m";
    case RADAR_CAPTURE_BOUNDARY_CORNER: return "CORNER <=2m";
    case RADAR_CAPTURE_BOUNDARY_DOORWAY: return "DOORWAY";
    case RADAR_CAPTURE_BOUNDARY_PARTITION: return "PARTITION";
    default: return "UNKNOWN";
    }
}

const char *radar_capture_room_size_label(uint8_t size) {
    switch (size) {
    case RADAR_CAPTURE_ROOM_SIZE_SMALL: return "SMALL";
    case RADAR_CAPTURE_ROOM_SIZE_MEDIUM: return "MEDIUM";
    case RADAR_CAPTURE_ROOM_SIZE_LARGE: return "LARGE";
    default: return "UNKNOWN";
    }
}

const char *radar_capture_room_size_label_for_space(uint8_t space, uint8_t size) {
    static const char *const room[] = {
        "SMALL <20m2", "MED 20-60m2", "LARGE >60m2"};
    static const char *const hall[] = {
        "SMALL <50m2", "MED 50-150m2", "LARGE >150m2"};
    static const char *const corridor[] = {
        "SMALL <10m", "MED 10-30m", "LARGE >30m"};
    static const char *const corner[] = {
        "SMALL <3m", "MED 3-6m", "LARGE >6m"};
    static const char *const stair[] = {
        "SMALL <5m", "MED 5-15m", "LARGE >15m"};
    const char *const *labels = room;
    switch ((radar_capture_room_geometry_t)space) {
    case RADAR_CAPTURE_ROOM_GEOMETRY_OPEN_HALL: labels = hall; break;
    case RADAR_CAPTURE_ROOM_GEOMETRY_CORRIDOR: labels = corridor; break;
    case RADAR_CAPTURE_ROOM_GEOMETRY_CORNER: labels = corner; break;
    case RADAR_CAPTURE_ROOM_GEOMETRY_STAIR: labels = stair; break;
    case RADAR_CAPTURE_ROOM_GEOMETRY_OUTDOOR_OPEN: return "N/A";
    default: break;
    }
    return size <= RADAR_CAPTURE_ROOM_SIZE_NOT_APPLICABLE
        ? (size == RADAR_CAPTURE_ROOM_SIZE_NOT_APPLICABLE ? "N/A" : labels[size])
        : "UNKNOWN";
}

const char *radar_capture_furniture_label(uint8_t density) {
    switch (density) {
    case RADAR_CAPTURE_FURNITURE_LOW: return "LOW <20%";
    case RADAR_CAPTURE_FURNITURE_MEDIUM: return "MED 20-50%";
    case RADAR_CAPTURE_FURNITURE_HIGH: return "HIGH >50%";
    default: return "UNKNOWN";
    }
}

const char *radar_capture_card_height_label(uint8_t height) {
    switch (height) {
    case RADAR_CAPTURE_CARD_HEIGHT_LOW: return "LOW<1.0m";
    case RADAR_CAPTURE_CARD_HEIGHT_CHEST: return "CHEST 1.0-1.8m";
    case RADAR_CAPTURE_CARD_HEIGHT_OVERHEAD: return "OVERHEAD>=1.8m";
    case RADAR_CAPTURE_CARD_HEIGHT_UNKNOWN: return "UNKNOWN";
    default: return "UNKNOWN";
    }
}

const char *radar_capture_card_pose_label(uint8_t pose) {
    switch (pose) {
    case RADAR_CAPTURE_CARD_POSE_UPRIGHT: return "UPRIGHT";
    case RADAR_CAPTURE_CARD_POSE_FLAT: return "FLAT";
    case RADAR_CAPTURE_CARD_POSE_MIXED: return "MIXED";
    default: return "UNKNOWN";
    }
}

radar_capture_card_height_t radar_capture_card_height_from_m(float height_m) {
    return radar_capture_condition_card_height_from_m(height_m);
}

const char *radar_capture_scene_label(uint16_t scene_id) {
    switch (scene_id) {
    case RADAR_CAPTURE_SCENE_I_LOS: return "I-LOS";
    case RADAR_CAPTURE_SCENE_I_STATIC: return "I-STATIC";
    case RADAR_CAPTURE_SCENE_I_DYNAMIC: return "I-DYNAMIC";
    case RADAR_CAPTURE_SCENE_I_MULTIPATH: return "I-MULTIPATH";
    case RADAR_CAPTURE_SCENE_O_LOS: return "O-LOS";
    case RADAR_CAPTURE_SCENE_O_STATIC: return "O-STATIC";
    case RADAR_CAPTURE_SCENE_O_DYNAMIC: return "O-DYNAMIC";
    case RADAR_CAPTURE_SCENE_O_MULTIPATH: return "O-MULTIPATH";
    default: return "CUSTOM";
    }
}

radar_capture_environment_id_t radar_capture_scene_environment(uint16_t scene_id) {
    return (radar_capture_environment_id_t)((scene_id >> 8) & 0xffu);
}

radar_capture_obstruction_id_t radar_capture_scene_obstruction(uint16_t scene_id) {
    (void)scene_id;
    return RADAR_CAPTURE_OBSTRUCTION_NONE;
}

radar_capture_propagation_id_t radar_capture_scene_propagation(uint16_t scene_id) {
    return radar_capture_condition_propagation(radar_capture_scene_link(scene_id));
}

radar_capture_link_condition_id_t radar_capture_scene_link(uint16_t scene_id) {
    return (radar_capture_link_condition_id_t)((scene_id >> 4) & 0x0fu);
}

size_t radar_capture_batch_used_bytes(void) { return s_next_offset; }
