#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    RADAR_CAPTURE_IDLE = 0,
    RADAR_CAPTURE_COUNTDOWN,
    RADAR_CAPTURE_SETTLE,
    RADAR_CAPTURE_RUNNING,
    RADAR_CAPTURE_SUMMARY,
    RADAR_CAPTURE_INVALID,
} radar_capture_phase_t;

typedef struct {
    uint32_t elapsed_ms;
    int8_t local_rssi;
    int8_t peer_rssi;
    bool peer_alive;
    bool ftm_valid;
    int16_t ftm_status;
    float ftm_distance_m;
    float ftm_spread_m;
    uint16_t ftm_session_count;
    bool ble_ready;
    bool ble_peer_alive;
    int8_t ble_rssi;
    int32_t ble_age_ms;
    uint32_t csi_frames;
    uint8_t environment_level;
} radar_capture_observation_t;

#define RADAR_CAPTURE_TARGET_COUNT 8
extern const float radar_capture_targets_m[RADAR_CAPTURE_TARGET_COUNT];

void radar_capture_init(void);
esp_err_t radar_capture_start(float target_distance_m);
void radar_capture_abort(void);
void radar_capture_tick(const radar_capture_observation_t *observation);
radar_capture_phase_t radar_capture_phase(void);
float radar_capture_target_m(void);
uint32_t radar_capture_elapsed_ms(void);
unsigned radar_capture_record_count(void);
bool radar_capture_data_valid(void);
bool radar_capture_save_succeeded(void);

// Encodes and saves the completed run to the existing NVS partition, then emits CSV after read-back verification.
esp_err_t radar_capture_save(void);

// Re-exports the newest valid NVS record without changing or erasing it.
esp_err_t radar_capture_export_saved(void);
