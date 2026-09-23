#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RADAR_CAPTURE_BUDGET_MAX_RECORDS 128u
#define RADAR_CAPTURE_BUDGET_RAW_RECORD_BYTES 20u
#define RADAR_CAPTURE_BUDGET_STORAGE_HEADER_BYTES 76u
#define RADAR_CAPTURE_BUDGET_CODEC_MAX_PAYLOAD 4096u
#define RADAR_CAPTURE_BUDGET_SAFETY_MARGIN 2048u
// Measured floor of the largest free internal 8-bit block observed while
// BLE, LVGL, ESP-NOW and FTM have been running for minutes (USB monitor,
// 2026-09-23): the heap fragmented far below the earlier 14336-byte
// observation. The capture buffers are therefore split into two
// independent allocations, each of which must fit this floor.
#define RADAR_CAPTURE_BUDGET_MEASURED_MIN_LARGEST 8192u

#define RADAR_CAPTURE_BUDGET_RECORD_BYTES \
    (RADAR_CAPTURE_BUDGET_MAX_RECORDS * RADAR_CAPTURE_BUDGET_RAW_RECORD_BYTES)
#define RADAR_CAPTURE_BUDGET_ALLOC_BYTES \
    (RADAR_CAPTURE_BUDGET_RECORD_BYTES + \
     RADAR_CAPTURE_BUDGET_STORAGE_HEADER_BYTES + \
     RADAR_CAPTURE_BUDGET_CODEC_MAX_PAYLOAD)

static inline bool radar_capture_budget_fits(size_t largest_free_block,
                                             size_t required_bytes) {
    return required_bytes <= largest_free_block &&
           RADAR_CAPTURE_BUDGET_SAFETY_MARGIN <=
               largest_free_block - required_bytes;
}

// Each capture allocation (records buffer, scratch buffer) must fit the
// worst measured contiguous block on its own; the two allocations are
// independent and never live in one contiguous region.
static inline size_t radar_capture_budget_max_single_alloc(void) {
    size_t record_bytes = RADAR_CAPTURE_BUDGET_MAX_RECORDS *
                          RADAR_CAPTURE_BUDGET_RAW_RECORD_BYTES;
    size_t scratch_bytes = RADAR_CAPTURE_BUDGET_STORAGE_HEADER_BYTES +
                           RADAR_CAPTURE_BUDGET_CODEC_MAX_PAYLOAD;
    return record_bytes > scratch_bytes ? record_bytes : scratch_bytes;
}

static inline size_t radar_capture_budget_required(size_t record_count,
                                                   size_t record_bytes,
                                                   size_t scratch_bytes) {
    if (record_count != 0 &&
        record_bytes > SIZE_MAX / record_count) return SIZE_MAX;
    size_t records = record_count * record_bytes;
    if (scratch_bytes > SIZE_MAX - records) return SIZE_MAX;
    return records + scratch_bytes;
}
