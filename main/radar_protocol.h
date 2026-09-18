#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RADAR_PACKET_NAME_LEN 12
#define RADAR_MAC_LEN 6
#define RADAR_BLE_ID_LEN 6
#define RADAR_PROTO_VER 2
#define RADAR_PKT_FLAG_SWEEPING 0x01
#define RADAR_PKT_FLAG_FTM_INITIATOR 0x02
#define RADAR_PKT_FLAG_VALID_AP_MAC 0x04
#define RADAR_PKT_FLAG_VALID_BLE_ID 0x08

// v1 is the prefix through dist_dm; v2 adds identity and sweep metadata.
typedef struct __attribute__((packed)) {
    char device_name[RADAR_PACKET_NAME_LEN];
    uint32_t sequence;
    uint8_t proto_ver;
    uint8_t flags;
    int8_t rssi_to_peer;
    int8_t reserved;
    int16_t angle_deg10;
    int16_t dist_dm;
    uint16_t payload_len;
    uint8_t ap_mac[RADAR_MAC_LEN];
    uint8_t ble_identity[RADAR_BLE_ID_LEN];
    uint32_t sweep_id;
    uint32_t sweep_elapsed_ms;
    uint8_t hop_index;
    uint8_t channel;
    uint8_t v2_reserved[2];
} radar_packet_t;

#define RADAR_PACKET_V1_PREFIX ((unsigned)offsetof(radar_packet_t, payload_len))

bool radar_protocol_seq_newer(uint32_t sequence, uint32_t previous);
bool radar_protocol_valid_rssi(int rssi);
bool radar_protocol_valid_angle10(int16_t angle_deg10);
bool radar_protocol_valid_distance_dm(int16_t distance_dm);
bool radar_protocol_valid_mac(const uint8_t mac[RADAR_MAC_LEN]);
bool radar_protocol_valid_v2(const radar_packet_t *packet, unsigned length);
