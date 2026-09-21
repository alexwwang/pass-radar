#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RADAR_CAPTURE_BUDGET_MAX_RECORDS 128u
#define RADAR_CAPTURE_BUDGET_RAW_RECORD_BYTES 20u
#define RADAR_CAPTURE_BUDGET_STORAGE_HEADER_BYTES 76u
#define RADAR_CAPTURE_BUDGET_CODEC_MAX_PAYLOAD 8192u
#define RADAR_CAPTURE_BUDGET_SAFETY_MARGIN 1536u
#define RADAR_CAPTURE_BUDGET_MEASURED_MIN_LARGEST 14336u

#define RADAR_CAPTURE_BUDGET_RECORD_BYTES \
    (RADAR_CAPTURE_BUDGET_MAX_RECORDS * RADAR_CAPTURE_BUDGET_RAW_RECORD_BYTES)
#define RADAR_CAPTURE_BUDGET_ALLOC_BYTES \
    (RADAR_CAPTURE_BUDGET_RECORD_BYTES + \
     RADAR_CAPTURE_BUDGET_STORAGE_HEADER_BYTES + \
     RADAR_CAPTURE_BUDGET_CODEC_MAX_PAYLOAD)

static inline bool radar_capture_budget_fits(size_t largest_free_block,
                                             size_t required_bytes) {
    return largest_free_block >= required_bytes +
                                   RADAR_CAPTURE_BUDGET_SAFETY_MARGIN;
}

static inline size_t radar_capture_budget_required(size_t record_count,
                                                   size_t record_bytes,
                                                   size_t scratch_bytes) {
    return record_count * record_bytes + scratch_bytes;
}
