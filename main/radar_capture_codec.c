#include "radar_capture_codec.h"

#include <string.h>

#define CODEC_TAG_FIRST 0u
#define CODEC_TAG_DELTA 1u
#define CODEC_TAG_REPEAT 2u
#define CODEC_TAG_ABSOLUTE 3u

static uint32_t zigzag_encode(int32_t value) {
    return ((uint32_t)value << 1) ^ (uint32_t)(value >> 31);
}

static int32_t zigzag_decode(uint32_t value) {
    return (int32_t)((value >> 1) ^ (uint32_t)-(int32_t)(value & 1u));
}

static size_t put_varint(uint8_t *out, size_t capacity, uint32_t value) {
    size_t count = 0;
    do {
        if (count >= capacity) return 0;
        uint8_t byte = (uint8_t)(value & 0x7fu);
        value >>= 7;
        out[count++] = (uint8_t)(byte | (value ? 0x80u : 0u));
    } while (value != 0);
    return count;
}

static bool get_varint(const uint8_t *data, size_t length, size_t *offset,
                       uint32_t *value) {
    uint32_t result = 0;
    unsigned shift = 0;
    while (*offset < length && shift <= 28) {
        uint8_t byte = data[(*offset)++];
        result |= (uint32_t)(byte & 0x7fu) << shift;
        if ((byte & 0x80u) == 0) {
            *value = result;
            return true;
        }
        shift += 7;
    }
    return false;
}

static size_t put_signed(uint8_t *out, size_t capacity, int32_t value) {
    return put_varint(out, capacity, zigzag_encode(value));
}

static bool get_signed(const uint8_t *data, size_t length, size_t *offset,
                       int32_t *value) {
    uint32_t encoded;
    if (!get_varint(data, length, offset, &encoded)) return false;
    *value = zigzag_decode(encoded);
    return true;
}

static bool put_u32(uint8_t *out, size_t capacity, size_t *offset,
                    uint32_t value) {
    size_t written = put_varint(out + *offset, capacity - *offset, value);
    if (written == 0) return false;
    *offset += written;
    return true;
}

static bool put_s32(uint8_t *out, size_t capacity, size_t *offset,
                    int32_t value) {
    size_t written = put_signed(out + *offset, capacity - *offset, value);
    if (written == 0) return false;
    *offset += written;
    return true;
}

static bool get_u32(const uint8_t *data, size_t length, size_t *offset,
                    uint32_t *value) {
    return get_varint(data, length, offset, value);
}

static bool get_s32(const uint8_t *data, size_t length, size_t *offset,
                    int32_t *value) {
    return get_signed(data, length, offset, value);
}

static bool put_tag(uint8_t *out, size_t capacity, size_t *offset, uint8_t tag) {
    if (*offset >= capacity) return false;
    out[(*offset)++] = tag;
    return true;
}

static bool put_delta_u32(uint8_t *out, size_t capacity, size_t *offset,
                          uint32_t current, uint32_t previous) {
    return put_u32(out, capacity, offset, current - previous);
}

static bool put_delta_s32(uint8_t *out, size_t capacity, size_t *offset,
                          int32_t current, int32_t previous) {
    return put_s32(out, capacity, offset, current - previous);
}

static bool get_delta_u32(const uint8_t *data, size_t length, size_t *offset,
                          uint32_t previous, uint32_t *current) {
    uint32_t delta;
    if (!get_u32(data, length, offset, &delta)) return false;
    *current = previous + delta;
    return true;
}

static bool get_delta_s32(const uint8_t *data, size_t length, size_t *offset,
                          int32_t previous, int32_t *current) {
    int32_t delta;
    if (!get_s32(data, length, offset, &delta)) return false;
    *current = previous + delta;
    return true;
}

static uint16_t distance_to_cm(float value) {
    if (!(value >= 0.0f) || value > 655.34f) return RADAR_CAPTURE_INVALID_U16;
    return (uint16_t)(value * 100.0f + 0.5f);
}

static float cm_to_distance(uint16_t value) {
    return value == RADAR_CAPTURE_INVALID_U16 ? -1.0f : (float)value / 100.0f;
}

static uint16_t spread_to_cm(float value) {
    if (!(value >= 0.0f) || value > 655.34f) return RADAR_CAPTURE_INVALID_U16;
    return (uint16_t)(value * 100.0f + 0.5f);
}

static float cm_to_spread(uint16_t value) {
    return value == RADAR_CAPTURE_INVALID_U16 ? 0.0f : (float)value / 100.0f;
}

static uint16_t age_to_ms(int32_t value) {
    if (value < 0 || value > 65534) return RADAR_CAPTURE_INVALID_U16;
    return (uint16_t)value;
}

static int32_t ms_to_age(uint16_t value) {
    return value == RADAR_CAPTURE_INVALID_U16 ? -1 : (int32_t)value;
}

static bool put_record(const radar_capture_record_t *record,
                       const radar_capture_record_t *previous,
                       uint8_t *out, size_t capacity, size_t *offset,
                       bool first) {
    uint16_t distance = distance_to_cm(record->ftm_distance_m);
    uint16_t spread = spread_to_cm(record->ftm_spread_m);
    uint16_t age = age_to_ms(record->ble_age_ms);
    if (first) {
        if (!put_tag(out, capacity, offset, CODEC_TAG_FIRST)) return false;
        if (!put_u32(out, capacity, offset, record->elapsed_ms) ||
            !put_s32(out, capacity, offset, record->local_rssi) ||
            !put_s32(out, capacity, offset, record->peer_rssi) ||
            !put_u32(out, capacity, offset, record->flags) ||
            !put_s32(out, capacity, offset, record->ftm_status) ||
            !put_u32(out, capacity, offset, distance) ||
            !put_u32(out, capacity, offset, spread) ||
            !put_u32(out, capacity, offset, record->ftm_session_count) ||
            !put_s32(out, capacity, offset, record->ble_rssi) ||
            !put_u32(out, capacity, offset, age) ||
            !put_u32(out, capacity, offset, record->csi_frames) ||
            !put_u32(out, capacity, offset, record->env_level)) return false;
        return true;
    }

    if (memcmp(record, previous, sizeof(*record)) == 0) {
        return put_tag(out, capacity, offset, CODEC_TAG_REPEAT);
    }
    if (!put_tag(out, capacity, offset, CODEC_TAG_DELTA) ||
        !put_delta_u32(out, capacity, offset, record->elapsed_ms, previous->elapsed_ms) ||
        !put_delta_s32(out, capacity, offset, record->local_rssi, previous->local_rssi) ||
        !put_delta_s32(out, capacity, offset, record->peer_rssi, previous->peer_rssi) ||
        !put_s32(out, capacity, offset, (int32_t)record->flags - (int32_t)previous->flags) ||
        !put_delta_s32(out, capacity, offset, record->ftm_status, previous->ftm_status) ||
        !put_s32(out, capacity, offset, (int32_t)distance - (int32_t)distance_to_cm(previous->ftm_distance_m)) ||
        !put_s32(out, capacity, offset, (int32_t)spread - (int32_t)spread_to_cm(previous->ftm_spread_m)) ||
        !put_delta_u32(out, capacity, offset, record->ftm_session_count, previous->ftm_session_count) ||
        !put_delta_s32(out, capacity, offset, record->ble_rssi, previous->ble_rssi) ||
        !put_s32(out, capacity, offset, (int32_t)age - (int32_t)age_to_ms(previous->ble_age_ms)) ||
        !put_delta_u32(out, capacity, offset, record->csi_frames, previous->csi_frames) ||
        !put_s32(out, capacity, offset, (int32_t)record->env_level - (int32_t)previous->env_level)) return false;
    return true;
}

size_t radar_capture_codec_encode(const radar_capture_record_t *records,
                                  size_t record_count,
                                  uint8_t *out,
                                  size_t out_capacity) {
    if (!records || !out || record_count == 0 || record_count > RADAR_CAPTURE_MAX_RECORDS) return 0;
    size_t offset = 0;
    for (size_t i = 0; i < record_count; ++i) {
        if (!put_record(&records[i], i ? &records[i - 1] : NULL,
                        out, out_capacity, &offset, i == 0)) return 0;
    }
    return offset;
}

static bool read_record(const uint8_t *payload, size_t length, size_t *offset,
                        const radar_capture_record_t *previous,
                        radar_capture_record_t *record, bool first) {
    if (*offset >= length) return false;
    uint8_t tag = payload[(*offset)++];
    if (!first && tag == CODEC_TAG_REPEAT) {
        *record = *previous;
        return true;
    }
    if (first && tag != CODEC_TAG_FIRST) return false;
    if (!first && tag != CODEC_TAG_DELTA && tag != CODEC_TAG_ABSOLUTE) return false;

    int32_t value;
    uint32_t uvalue;
    uint32_t elapsed_ms;
    uint32_t csi_frames;
    if (first) {
        if (!get_u32(payload, length, offset, &elapsed_ms) ||
            !get_s32(payload, length, offset, &value)) return false;
        record->elapsed_ms = elapsed_ms;
        record->local_rssi = (int8_t)value;
        if (!get_s32(payload, length, offset, &value)) return false;
        record->peer_rssi = (int8_t)value;
        if (!get_u32(payload, length, offset, &uvalue)) return false;
        record->flags = (uint8_t)uvalue;
        if (!get_s32(payload, length, offset, &value)) return false;
        record->ftm_status = (int16_t)value;
        if (!get_u32(payload, length, offset, &uvalue)) return false;
        record->ftm_distance_m = cm_to_distance((uint16_t)uvalue);
        if (!get_u32(payload, length, offset, &uvalue)) return false;
        record->ftm_spread_m = cm_to_spread((uint16_t)uvalue);
        if (!get_u32(payload, length, offset, &uvalue)) return false;
        record->ftm_session_count = (uint16_t)uvalue;
        if (!get_s32(payload, length, offset, &value)) return false;
        record->ble_rssi = (int8_t)value;
        if (!get_u32(payload, length, offset, &uvalue)) return false;
        record->ble_age_ms = ms_to_age((uint16_t)uvalue);
        if (!get_u32(payload, length, offset, &csi_frames) ||
            !get_u32(payload, length, offset, &uvalue)) return false;
        record->csi_frames = csi_frames;
        record->env_level = (uint8_t)uvalue;
        memset(record->reserved, 0, sizeof(record->reserved));
        return true;
    }

    if (!get_delta_u32(payload, length, offset, previous->elapsed_ms, &elapsed_ms) ||
        !get_delta_s32(payload, length, offset, previous->local_rssi, &value)) return false;
    record->elapsed_ms = elapsed_ms;
    record->local_rssi = (int8_t)value;
    if (!get_delta_s32(payload, length, offset, previous->peer_rssi, &value)) return false;
    record->peer_rssi = (int8_t)value;
    if (!get_s32(payload, length, offset, &value)) return false;
    record->flags = (uint8_t)((int32_t)previous->flags + value);
    if (!get_delta_s32(payload, length, offset, previous->ftm_status, &value)) return false;
    record->ftm_status = (int16_t)value;
    uint16_t previous_distance = distance_to_cm(previous->ftm_distance_m);
    if (!get_s32(payload, length, offset, &value)) return false;
    record->ftm_distance_m = cm_to_distance((uint16_t)((int32_t)previous_distance + value));
    uint16_t previous_spread = spread_to_cm(previous->ftm_spread_m);
    if (!get_s32(payload, length, offset, &value)) return false;
    record->ftm_spread_m = cm_to_spread((uint16_t)((int32_t)previous_spread + value));
    if (!get_delta_u32(payload, length, offset, previous->ftm_session_count,
                       &uvalue)) return false;
    record->ftm_session_count = (uint16_t)uvalue;
    if (!get_delta_s32(payload, length, offset, previous->ble_rssi, &value)) return false;
    record->ble_rssi = (int8_t)value;
    uint16_t previous_age = age_to_ms(previous->ble_age_ms);
    if (!get_s32(payload, length, offset, &value)) return false;
    record->ble_age_ms = ms_to_age((uint16_t)((int32_t)previous_age + value));
    if (!get_delta_u32(payload, length, offset, previous->csi_frames,
                       &csi_frames)) return false;
    record->csi_frames = csi_frames;
    if (!get_s32(payload, length, offset, &value)) return false;
    record->env_level = (uint8_t)((int32_t)previous->env_level + value);
    memset(record->reserved, 0, sizeof(record->reserved));
    return true;
}

bool radar_capture_codec_decode(const uint8_t *payload,
                                size_t payload_length,
                                radar_capture_record_t *records,
                                size_t record_capacity,
                                size_t expected_count) {
    if (!payload || !records || expected_count == 0 || expected_count > record_capacity ||
        expected_count > RADAR_CAPTURE_MAX_RECORDS) return false;
    size_t offset = 0;
    for (size_t i = 0; i < expected_count; ++i) {
        if (!read_record(payload, payload_length, &offset, i ? &records[i - 1] : NULL,
                         &records[i], i == 0)) return false;
    }
    return offset == payload_length;
}

uint32_t radar_capture_codec_crc32(const void *data, size_t length) {
    const uint8_t *bytes = data;
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)-(int)(crc & 1u));
        }
    }
    return ~crc;
}
