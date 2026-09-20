#pragma once

#include <stdbool.h>
#include <stdint.h>

#define RADAR_DIAG_RECORD_MAGIC 0x52444731u /* RDG1 */
#define RADAR_DIAG_RECORD_VERSION 1u

typedef enum {
    RADAR_DIAG_STAGE_BOOT = 1,
    RADAR_DIAG_STAGE_DISPLAY_READY,
    RADAR_DIAG_STAGE_RADIO_INIT,
    RADAR_DIAG_STAGE_RADIO_READY,
    RADAR_DIAG_STAGE_CSI_READY,
    RADAR_DIAG_STAGE_PEER_SEEN,
    RADAR_DIAG_STAGE_RUNNING,
} radar_diag_stage_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t sequence;
    uint32_t reset_reason;
    uint16_t stage;
    uint16_t flags;
    int16_t peer_rssi;
    uint16_t csi_frames;
    uint32_t uptime_ms;
    uint32_t crc32;
} radar_diag_record_t;

_Static_assert(sizeof(radar_diag_record_t) == 32, "diagnostic record must be one NVS entry");

// Initializes NVS and records a new boot breadcrumb. Safe to call once.
// It also reports the previous NVS/RTC breadcrumb through the WARN log level.
bool radar_diag_persist_init(void);

// Persists only stage transitions or changed flags, avoiding high-frequency flash writes.
void radar_diag_persist_stage(radar_diag_stage_t stage, uint16_t flags,
                              int peer_rssi, unsigned csi_frames);

// Returns the record from the previous boot, if it was valid.
bool radar_diag_persist_previous(radar_diag_record_t *out);
uint32_t radar_diag_current_sequence(void);
uint32_t radar_diag_current_reset_reason(void);

bool radar_diag_record_valid(const radar_diag_record_t *record);
uint32_t radar_diag_record_crc(const radar_diag_record_t *record);
