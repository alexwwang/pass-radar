#include "radar_protocol.h"
#include <stddef.h>
#include <string.h>

bool radar_protocol_seq_newer(uint32_t sequence, uint32_t previous) {
    return (int32_t)(sequence - previous) > 0;
}

bool radar_protocol_valid_rssi(int rssi) {
    return rssi >= -127 && rssi <= 20;
}

bool radar_protocol_valid_angle10(int16_t angle_deg10) {
    return angle_deg10 == -1 || (angle_deg10 >= 0 && angle_deg10 <= 3600);
}

bool radar_protocol_valid_distance_dm(int16_t distance_dm) {
    return distance_dm == -1 || (distance_dm >= 0 && distance_dm <= 10000);
}

bool radar_protocol_valid_mac(const uint8_t mac[RADAR_MAC_LEN]) {
    static const uint8_t zero[RADAR_MAC_LEN] = {0};
    return mac && memcmp(mac, zero, RADAR_MAC_LEN) != 0;
}

bool radar_protocol_valid_v2(const radar_packet_t *packet, unsigned length) {
    return packet && length >= sizeof(*packet) &&
           packet->proto_ver >= RADAR_PROTO_MIN_VER &&
           packet->proto_ver <= RADAR_PROTO_VER &&
           packet->payload_len == sizeof(*packet) &&
           radar_protocol_valid_rssi(packet->rssi_to_peer) &&
           radar_protocol_valid_angle10(packet->angle_deg10) &&
           radar_protocol_valid_distance_dm(packet->dist_dm) &&
           radar_protocol_valid_mac(packet->ap_mac);
}
