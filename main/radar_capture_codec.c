#include "radar_capture_codec.h"

#include <string.h>

#define TAG_FIRST 0u
#define TAG_DELTA 1u
#define TAG_REPEAT 2u

static uint32_t zigzag_encode(int32_t value) {
    return ((uint32_t)value << 1) ^ (uint32_t)(value >> 31);
}

static int32_t zigzag_decode(uint32_t value) {
    return (int32_t)((value >> 1) ^ (uint32_t)-(int32_t)(value & 1u));
}

static bool put_varint(uint8_t *out, size_t capacity, size_t *offset,
                       uint32_t value) {
    do {
        if (*offset >= capacity) return false;
        uint8_t byte = (uint8_t)(value & 0x7fu);
        value >>= 7;
        out[(*offset)++] = (uint8_t)(byte | (value ? 0x80u : 0u));
    } while (value != 0);
    return true;
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

static bool put_signed(uint8_t *out, size_t capacity, size_t *offset,
                       int32_t value) {
    return put_varint(out, capacity, offset, zigzag_encode(value));
}

static bool get_signed(const uint8_t *data, size_t length, size_t *offset,
                       int32_t *value) {
    uint32_t encoded;
    if (!get_varint(data, length, offset, &encoded)) return false;
    *value = zigzag_decode(encoded);
    return true;
}

static bool put_delta(uint8_t *out, size_t capacity, size_t *offset,
                      int32_t current, int32_t previous) {
    return put_signed(out, capacity, offset, current - previous);
}

static bool get_delta(const uint8_t *data, size_t length, size_t *offset,
                      int32_t previous, int32_t *current) {
    int32_t delta;
    if (!get_signed(data, length, offset, &delta)) return false;
    *current = previous + delta;
    return true;
}

static bool put_record(const radar_capture_record_t *record,
                       const radar_capture_record_t *previous,
                       uint8_t *out, size_t capacity, size_t *offset,
                       bool first) {
    if (first) {
        if (!put_varint(out, capacity, offset, TAG_FIRST)) return false;
        return put_varint(out, capacity, offset, record->elapsed_ms) &&
               put_signed(out, capacity, offset, record->local_rssi) &&
               put_signed(out, capacity, offset, record->peer_rssi) &&
               put_varint(out, capacity, offset, record->flags) &&
               put_signed(out, capacity, offset, record->ftm_status) &&
               put_varint(out, capacity, offset, record->ftm_distance_cm) &&
               put_varint(out, capacity, offset, record->ftm_spread_cm) &&
               put_varint(out, capacity, offset, record->ftm_session_count) &&
               put_signed(out, capacity, offset, record->ble_rssi) &&
               put_varint(out, capacity, offset, record->ble_age_ms) &&
               put_varint(out, capacity, offset, record->csi_frames) &&
               put_varint(out, capacity, offset, record->env_level);
    }

    if (memcmp(record, previous, sizeof(*record)) == 0) {
        return put_varint(out, capacity, offset, TAG_REPEAT);
    }
    return put_varint(out, capacity, offset, TAG_DELTA) &&
           put_delta(out, capacity, offset, record->elapsed_ms, previous->elapsed_ms) &&
           put_delta(out, capacity, offset, record->local_rssi, previous->local_rssi) &&
           put_delta(out, capacity, offset, record->peer_rssi, previous->peer_rssi) &&
           put_delta(out, capacity, offset, record->flags, previous->flags) &&
           put_delta(out, capacity, offset, record->ftm_status, previous->ftm_status) &&
           put_delta(out, capacity, offset, record->ftm_distance_cm, previous->ftm_distance_cm) &&
           put_delta(out, capacity, offset, record->ftm_spread_cm, previous->ftm_spread_cm) &&
           put_delta(out, capacity, offset, record->ftm_session_count, previous->ftm_session_count) &&
           put_delta(out, capacity, offset, record->ble_rssi, previous->ble_rssi) &&
           put_delta(out, capacity, offset, record->ble_age_ms, previous->ble_age_ms) &&
           put_delta(out, capacity, offset, record->csi_frames, previous->csi_frames) &&
           put_delta(out, capacity, offset, record->env_level, previous->env_level);
}

size_t radar_capture_codec_encode(const radar_capture_record_t *records,
                                  size_t record_count, uint8_t *out,
                                  size_t out_capacity) {
    if (!records || !out || record_count == 0 ||
        record_count > RADAR_CAPTURE_MAX_RECORDS) return 0;
    size_t offset = 0;
    for (size_t i = 0; i < record_count; ++i) {
        if (!put_record(&records[i], i ? &records[i - 1] : NULL,
                        out, out_capacity, &offset, i == 0)) return 0;
    }
    return offset;
}

static bool read_record(const uint8_t *data, size_t length, size_t *offset,
                        const radar_capture_record_t *previous,
                        radar_capture_record_t *record, bool first) {
    uint32_t tag;
    if (!get_varint(data, length, offset, &tag)) return false;
    if (!first && tag == TAG_REPEAT) {
        *record = *previous;
        return true;
    }
    if ((first && tag != TAG_FIRST) || (!first && tag != TAG_DELTA)) return false;

    uint32_t unsigned_value;
    int32_t signed_value;
    if (first) {
        if (!get_varint(data, length, offset, &unsigned_value)) return false;
        record->elapsed_ms = (uint16_t)unsigned_value;
        if (!get_signed(data, length, offset, &signed_value)) return false;
        record->local_rssi = (int8_t)signed_value;
        if (!get_signed(data, length, offset, &signed_value)) return false;
        record->peer_rssi = (int8_t)signed_value;
        if (!get_varint(data, length, offset, &unsigned_value)) return false;
        record->flags = (uint8_t)unsigned_value;
        if (!get_signed(data, length, offset, &signed_value)) return false;
        record->ftm_status = (int16_t)signed_value;
        if (!get_varint(data, length, offset, &unsigned_value)) return false;
        record->ftm_distance_cm = (uint16_t)unsigned_value;
        if (!get_varint(data, length, offset, &unsigned_value)) return false;
        record->ftm_spread_cm = (uint16_t)unsigned_value;
        if (!get_varint(data, length, offset, &unsigned_value)) return false;
        record->ftm_session_count = (uint16_t)unsigned_value;
        if (!get_signed(data, length, offset, &signed_value)) return false;
        record->ble_rssi = (int8_t)signed_value;
        if (!get_varint(data, length, offset, &unsigned_value)) return false;
        record->ble_age_ms = (uint16_t)unsigned_value;
        if (!get_varint(data, length, offset, &unsigned_value)) return false;
        record->csi_frames = (uint16_t)unsigned_value;
        if (!get_varint(data, length, offset, &unsigned_value)) return false;
        record->env_level = (uint8_t)unsigned_value;
        record->reserved = 0;
        return true;
    }

#define GET_DELTA(field, type) \
    if (!get_delta(data, length, offset, previous->field, &signed_value)) return false; \
    record->field = (type)signed_value
    GET_DELTA(elapsed_ms, uint16_t);
    GET_DELTA(local_rssi, int8_t);
    GET_DELTA(peer_rssi, int8_t);
    GET_DELTA(flags, uint8_t);
    GET_DELTA(ftm_status, int16_t);
    GET_DELTA(ftm_distance_cm, uint16_t);
    GET_DELTA(ftm_spread_cm, uint16_t);
    GET_DELTA(ftm_session_count, uint16_t);
    GET_DELTA(ble_rssi, int8_t);
    GET_DELTA(ble_age_ms, uint16_t);
    GET_DELTA(csi_frames, uint16_t);
    GET_DELTA(env_level, uint8_t);
#undef GET_DELTA
    record->reserved = 0;
    return true;
}

bool radar_capture_codec_decode(const uint8_t *payload, size_t payload_length,
                                radar_capture_record_t *records,
                                size_t record_capacity, size_t expected_count) {
    if (!payload || !records || expected_count == 0 ||
        expected_count > record_capacity ||
        expected_count > RADAR_CAPTURE_MAX_RECORDS) return false;
    size_t offset = 0;
    for (size_t i = 0; i < expected_count; ++i) {
        if (!read_record(payload, payload_length, &offset,
                         i ? &records[i - 1] : NULL, &records[i], i == 0)) {
            return false;
        }
    }
    return offset == payload_length;
}

uint32_t radar_capture_codec_crc32(const void *data, size_t length) {
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
