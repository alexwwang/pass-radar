#pragma once

// CSI amplitude/selectivity statistics. The math remains host-testable; the
// ESP-IDF capture adapter is compiled only for the firmware target.
#include <stdbool.h>
#include <stdint.h>

#define RADAR_CSI_MAX_SUBCARRIERS 64
#define RADAR_CSI_HISTORY 8

typedef struct {
    float amplitude[RADAR_CSI_MAX_SUBCARRIERS];
    unsigned count;
} radar_csi_frame_t;

typedef struct {
    radar_csi_frame_t frames[RADAR_CSI_HISTORY];
    unsigned head;
    unsigned count;
} radar_csi_history_t;

void radar_csi_history_init(radar_csi_history_t *history);
bool radar_csi_history_push(radar_csi_history_t *history,
                            const float *amplitude, unsigned count);
float radar_csi_selectivity_db(const radar_csi_history_t *history);

// Global capture adapter used by the tracking task and the simulator.
bool radar_csi_init(void);
void radar_csi_set_peer_mac(const uint8_t mac[6]);
void radar_csi_start_window(int64_t t0_us);
unsigned radar_csi_stop_window(void);
float radar_csi_recent_selectivity_db(void);
unsigned radar_csi_received_frames(void);
// Test/simulator injection uses the same global history as the hardware callback.
bool radar_csi_push_amplitudes(const float *amplitude, unsigned count);
