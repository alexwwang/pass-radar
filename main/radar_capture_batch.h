#pragma once

#ifdef ESP_PLATFORM
#include "esp_err.h"
#else
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_INVALID_CRC 0x109
#define ESP_ERR_NO_MEM 0x101
#endif
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// meta-pass ota_2 is 0x560000..0x7fe000. The final 4 KiB is reserved
// for the slot display-name blob, so an empty ota_2 provides the full
// preceding region for Capture data. The physical base is selected at
// runtime; all record offsets are relative to the selected data region.
#define RADAR_CAPTURE_BATCH_SLOT2_SIZE 0x29E000u
#define RADAR_CAPTURE_BATCH_NAME_SECTOR_SIZE 0x1000u
#define RADAR_CAPTURE_BATCH_FLASH_SIZE \
    (RADAR_CAPTURE_BATCH_SLOT2_SIZE - RADAR_CAPTURE_BATCH_NAME_SECTOR_SIZE)
#define RADAR_CAPTURE_BATCH_SECTOR_SIZE 0x1000u
#define RADAR_CAPTURE_BATCH_MAX_CAPTURES 160u
#define RADAR_CAPTURE_BATCH_MENU_SCENE_MAX 8u
#define RADAR_CAPTURE_BATCH_MAX_RECORDS 128u
#define RADAR_CAPTURE_BATCH_MAX_PAYLOAD 8192u
#define RADAR_CAPTURE_BATCH_MAGIC 0x52434231u /* RCB1 */
#define RADAR_CAPTURE_BATCH_CAPTURE_MAGIC 0x52434331u /* RCC1 */
#define RADAR_CAPTURE_BATCH_COMMIT_MAGIC 0x52434f4bu /* RCOK */
#define RADAR_CAPTURE_BATCH_FORMAT_VERSION 3u
#define RADAR_CAPTURE_CONDITION_SCHEMA_VERSION 1u
#define RADAR_CAPTURE_BATCH_SCENE_MAX 0x1000u

typedef enum {
    RADAR_CAPTURE_ENV_OUTDOOR = 0,
    RADAR_CAPTURE_ENV_INDOOR = 1,
} radar_capture_environment_id_t;

typedef enum {
    RADAR_CAPTURE_OBSTRUCTION_NONE = 0,
    RADAR_CAPTURE_OBSTRUCTION_HUMAN_STATIC = 1,
    RADAR_CAPTURE_OBSTRUCTION_HUMAN_DYNAMIC = 2,
    RADAR_CAPTURE_OBSTRUCTION_FOLIAGE = 3,
    RADAR_CAPTURE_OBSTRUCTION_WALL = 4,
    RADAR_CAPTURE_OBSTRUCTION_METAL = 5,
} radar_capture_obstruction_id_t;

typedef enum {
    RADAR_CAPTURE_PROPAGATION_LOS = 0,
    RADAR_CAPTURE_PROPAGATION_NLOS = 1,
    RADAR_CAPTURE_PROPAGATION_MULTIPATH = 2,
} radar_capture_propagation_id_t;

typedef enum {
    RADAR_CAPTURE_LINK_LOS = 0,
    RADAR_CAPTURE_LINK_STATIC_ATTENUATION = 1,
    RADAR_CAPTURE_LINK_DYNAMIC_ATTENUATION = 2,
    RADAR_CAPTURE_LINK_MULTIPATH = 3,
} radar_capture_link_condition_id_t;

typedef enum {
    RADAR_CAPTURE_SCENE_I_LOS = (RADAR_CAPTURE_ENV_INDOOR << 8) |
                                (RADAR_CAPTURE_LINK_LOS << 4),
    RADAR_CAPTURE_SCENE_I_STATIC = (RADAR_CAPTURE_ENV_INDOOR << 8) |
                                   (RADAR_CAPTURE_LINK_STATIC_ATTENUATION << 4),
    RADAR_CAPTURE_SCENE_I_DYNAMIC = (RADAR_CAPTURE_ENV_INDOOR << 8) |
                                    (RADAR_CAPTURE_LINK_DYNAMIC_ATTENUATION << 4),
    RADAR_CAPTURE_SCENE_I_MULTIPATH = (RADAR_CAPTURE_ENV_INDOOR << 8) |
                                      (RADAR_CAPTURE_LINK_MULTIPATH << 4),
    RADAR_CAPTURE_SCENE_O_LOS = (RADAR_CAPTURE_ENV_OUTDOOR << 8) |
                                (RADAR_CAPTURE_LINK_LOS << 4),
    RADAR_CAPTURE_SCENE_O_STATIC = (RADAR_CAPTURE_ENV_OUTDOOR << 8) |
                                   (RADAR_CAPTURE_LINK_STATIC_ATTENUATION << 4),
    RADAR_CAPTURE_SCENE_O_DYNAMIC = (RADAR_CAPTURE_ENV_OUTDOOR << 8) |
                                    (RADAR_CAPTURE_LINK_DYNAMIC_ATTENUATION << 4),
    RADAR_CAPTURE_SCENE_O_MULTIPATH = (RADAR_CAPTURE_ENV_OUTDOOR << 8) |
                                      (RADAR_CAPTURE_LINK_MULTIPATH << 4),
} radar_capture_scene_id_t;

typedef enum {
    RADAR_CAPTURE_CONTEXT_SPACE = 0,
    RADAR_CAPTURE_CONTEXT_ROOM_SIZE,
    RADAR_CAPTURE_CONTEXT_OBSTACLE,
    RADAR_CAPTURE_CONTEXT_MATERIAL,
    RADAR_CAPTURE_CONTEXT_FURNITURE,
    RADAR_CAPTURE_CONTEXT_BOUNDARY,
    RADAR_CAPTURE_CONTEXT_CARD_HEIGHT,
    RADAR_CAPTURE_CONTEXT_CARD_POSE,
} radar_capture_context_field_t;

typedef enum {
    RADAR_CAPTURE_ROOM_GEOMETRY_ROOM = 0,
    RADAR_CAPTURE_ROOM_GEOMETRY_CORRIDOR = 1,
    RADAR_CAPTURE_ROOM_GEOMETRY_OPEN_HALL = 2,
    RADAR_CAPTURE_ROOM_GEOMETRY_CORNER = 3,
    RADAR_CAPTURE_ROOM_GEOMETRY_STAIR = 4,
    RADAR_CAPTURE_ROOM_GEOMETRY_OUTDOOR_OPEN = 5,
} radar_capture_room_geometry_t;

typedef enum {
    RADAR_CAPTURE_ROOM_SIZE_SMALL = 0,
    RADAR_CAPTURE_ROOM_SIZE_MEDIUM = 1,
    RADAR_CAPTURE_ROOM_SIZE_LARGE = 2,
} radar_capture_room_size_t;

typedef enum {
    RADAR_CAPTURE_MATERIAL_SOFT = 0,
    RADAR_CAPTURE_MATERIAL_MIXED = 1,
    RADAR_CAPTURE_MATERIAL_HARD = 2,
    RADAR_CAPTURE_MATERIAL_METAL_GLASS = 3,
} radar_capture_material_t;

typedef enum {
    RADAR_CAPTURE_FURNITURE_LOW = 0,
    RADAR_CAPTURE_FURNITURE_MEDIUM = 1,
    RADAR_CAPTURE_FURNITURE_HIGH = 2,
} radar_capture_furniture_density_t;

typedef enum {
    RADAR_CAPTURE_BOUNDARY_OPEN = 0,
    RADAR_CAPTURE_BOUNDARY_WALL_NEAR = 1,
    RADAR_CAPTURE_BOUNDARY_CORNER = 2,
    RADAR_CAPTURE_BOUNDARY_DOORWAY = 3,
    RADAR_CAPTURE_BOUNDARY_PARTITION = 4,
} radar_capture_boundary_t;

typedef enum {
    RADAR_CAPTURE_CARD_HEIGHT_LOW = 0,
    RADAR_CAPTURE_CARD_HEIGHT_CHEST = 1,
    RADAR_CAPTURE_CARD_HEIGHT_OVERHEAD = 2,
} radar_capture_card_height_t;

typedef enum {
    RADAR_CAPTURE_CARD_POSE_UPRIGHT = 0,
    RADAR_CAPTURE_CARD_POSE_FLAT = 1,
    RADAR_CAPTURE_CARD_POSE_MIXED = 2,
} radar_capture_card_pose_t;

typedef enum {
    RADAR_CAPTURE_OBS_LEVEL_L0_LOS = 0,
    RADAR_CAPTURE_OBS_LEVEL_L1_INDOOR = 1,
    RADAR_CAPTURE_OBS_LEVEL_L2_NLOS = 2,
    RADAR_CAPTURE_OBS_LEVEL_L3_LOST = 3,
} radar_capture_observed_level_t;

typedef enum {
    RADAR_CAPTURE_OBS_CLASS_NONE_LOS = 0,
    RADAR_CAPTURE_OBS_CLASS_FAR_LOS = 1,
    RADAR_CAPTURE_OBS_CLASS_ATTENUATING_STATIC = 2,
    RADAR_CAPTURE_OBS_CLASS_FOLIAGE_DYNAMIC = 3,
    RADAR_CAPTURE_OBS_CLASS_REFLECTION = 4,
    RADAR_CAPTURE_OBS_CLASS_UNKNOWN = 5,
} radar_capture_observed_obstruction_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint32_t batch_id;
    uint32_t expected_captures;
    uint32_t committed_captures;
    uint32_t next_offset;
    uint32_t header_crc32;
    uint8_t reserved[100];
} radar_capture_batch_header_t;

_Static_assert(sizeof(radar_capture_batch_header_t) == 128,
               "batch header must be 128 bytes");

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint32_t batch_id;
    uint32_t run_id;
    uint32_t capture_id;
    uint16_t scene_id;
    uint8_t condition_environment;
    uint8_t condition_link;
    uint8_t condition_obstruction;
    uint8_t condition_propagation;
    uint8_t condition_schema_version;
    uint8_t observed_level;
    uint8_t observed_obstruction;
    uint8_t room_geometry;
    uint8_t room_size;
    uint8_t material;
    uint8_t furniture_density;
    uint8_t boundary;
    uint8_t card_height;
    uint8_t card_pose;
    float target_distance_m;
    float measured_distance_m;
    uint32_t record_count;
    uint32_t payload_length;
    uint32_t payload_crc32;
    uint32_t firmware_hash32;
    uint32_t boot_sequence;
    uint32_t reset_reason;
    uint32_t record_crc32;
    uint8_t reserved[40];
} radar_capture_batch_capture_header_t;

_Static_assert(sizeof(radar_capture_batch_capture_header_t) == 112,
               "capture batch header must be 112 bytes");

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t batch_id;
    uint32_t run_id;
    uint32_t capture_crc32;
} radar_capture_batch_commit_t;

_Static_assert(sizeof(radar_capture_batch_commit_t) == 16,
               "capture commit marker must be 16 bytes");

typedef struct {
    uint32_t batch_id;
    uint32_t run_id;
    uint32_t capture_id;
    uint16_t scene_id;
    uint8_t condition_environment;
    uint8_t condition_link;
    uint8_t condition_obstruction;
    uint8_t condition_propagation;
    uint8_t condition_schema_version;
    uint8_t observed_level;
    uint8_t observed_obstruction;
    uint8_t room_geometry;
    uint8_t room_size;
    uint8_t material;
    uint8_t furniture_density;
    uint8_t boundary;
    uint8_t card_height;
    uint8_t card_pose;
    float target_distance_m;
    float measured_distance_m;
    uint32_t record_count;
    uint32_t boot_sequence;
    uint32_t reset_reason;
    uint32_t firmware_hash32;
} radar_capture_batch_capture_meta_t;

static inline size_t radar_capture_batch_align(size_t value) {
    const size_t mask = RADAR_CAPTURE_BATCH_SECTOR_SIZE - 1u;
    if (value > SIZE_MAX - mask) return 0;
    return (value + mask) & ~mask;
}

static inline size_t radar_capture_batch_commit_offset(size_t record_offset,
                                                        size_t payload_length) {
    // Header and payload are written as independent padded blocks.
    size_t header_span = radar_capture_batch_align(
        sizeof(radar_capture_batch_capture_header_t));
    size_t payload_span = radar_capture_batch_align(payload_length);
    if (header_span == 0 || payload_span == 0 ||
        record_offset > SIZE_MAX - header_span ||
        record_offset + header_span > SIZE_MAX - payload_span) return 0;
    return record_offset + header_span + payload_span;
}

static inline size_t radar_capture_batch_data_offset(void) {
    return radar_capture_batch_align(sizeof(radar_capture_batch_header_t));
}

static inline size_t radar_capture_batch_capture_span(size_t payload_length) {
    // Each write_padded() call occupies its own 4 KiB-aligned span. The
    // commit marker is written after the padded payload, so aligning the
    // combined byte count would underestimate the next record offset.
    size_t commit_offset = radar_capture_batch_commit_offset(0, payload_length);
    size_t commit_span = radar_capture_batch_align(
        sizeof(radar_capture_batch_commit_t));
    if (commit_offset == 0 || commit_span == 0 ||
        commit_offset > SIZE_MAX - commit_span) return 0;
    return commit_offset + commit_span;
}

static inline bool radar_capture_batch_capacity_fits(size_t capture_count,
                                                     size_t payload_length) {
    if (capture_count > RADAR_CAPTURE_BATCH_MAX_CAPTURES) return false;
    size_t data_offset = radar_capture_batch_data_offset();
    size_t span = radar_capture_batch_capture_span(payload_length);
    if (data_offset == 0 || span == 0 || data_offset > RADAR_CAPTURE_BATCH_FLASH_SIZE ||
        capture_count > (RADAR_CAPTURE_BATCH_FLASH_SIZE - data_offset) / span) {
        return false;
    }
    return data_offset + capture_count * span <= RADAR_CAPTURE_BATCH_FLASH_SIZE;
}
uint32_t radar_capture_batch_crc32(const void *data, size_t length);
uint32_t radar_capture_batch_record_crc32(
    const radar_capture_batch_capture_header_t *header,
    const uint8_t *payload, size_t payload_length);
bool radar_capture_batch_header_valid(const radar_capture_batch_header_t *header);
bool radar_capture_batch_capture_header_valid(
    const radar_capture_batch_capture_header_t *header);

// Flash-backed operations. These use an empty, non-running meta-pass OTA
// partition; the current deployment uses the full usable ota_2 region.
esp_err_t radar_capture_batch_open(void);
esp_err_t radar_capture_batch_begin(uint32_t batch_id, uint32_t expected_captures);
esp_err_t radar_capture_batch_append(const radar_capture_batch_capture_meta_t *meta,
                                     const uint8_t *payload, size_t payload_length,
                                     uint32_t payload_crc32);
typedef esp_err_t (*radar_capture_batch_record_cb)(
    const radar_capture_batch_capture_header_t *header,
    const uint8_t *payload, size_t payload_length, void *context);

esp_err_t radar_capture_batch_export(radar_capture_batch_record_cb callback,
                                      void *context);
esp_err_t radar_capture_batch_erase(void);
bool radar_capture_batch_can_append(size_t max_payload_length);
const char *radar_capture_batch_space_status(void);
uint32_t radar_capture_batch_id(void);
uint32_t radar_capture_batch_count(void);
const char *radar_capture_scene_label(uint16_t scene_id);
const char *radar_capture_environment_label(uint8_t environment);
const char *radar_capture_link_label(uint8_t link);
const char *radar_capture_obstacle_label(uint8_t obstacle);
const char *radar_capture_material_label(uint8_t material);
const char *radar_capture_boundary_label(uint8_t boundary);
const char *radar_capture_room_size_label(uint8_t size);
const char *radar_capture_furniture_label(uint8_t density);
const char *radar_capture_card_height_label(uint8_t height);
const char *radar_capture_card_pose_label(uint8_t pose);
uint16_t radar_capture_scene_menu_id(unsigned index);
unsigned radar_capture_scene_menu_count(void);
radar_capture_environment_id_t radar_capture_scene_environment(uint16_t scene_id);
radar_capture_obstruction_id_t radar_capture_scene_obstruction(uint16_t scene_id);
radar_capture_propagation_id_t radar_capture_scene_propagation(uint16_t scene_id);
radar_capture_link_condition_id_t radar_capture_scene_link(uint16_t scene_id);
bool radar_capture_context_option_available(
    radar_capture_environment_id_t environment,
    radar_capture_link_condition_id_t link,
    radar_capture_context_field_t field, uint8_t value);
bool radar_capture_condition_has_warning(
    radar_capture_environment_id_t environment,
    radar_capture_link_condition_id_t link, uint8_t obstacle,
    uint8_t space);
size_t radar_capture_batch_used_bytes(void);
