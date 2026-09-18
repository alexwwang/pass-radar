// tests/test_radar_protocol.c —— radar v2 协议纯逻辑门控。
#include "../main/radar_protocol.h"
#include "../main/radar_sweep.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST_SWEEP_DURATION_MS 4000

static int failures;
#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fputc('\n', stderr); \
        failures++; \
    } \
} while (0)

static int seq_newer(uint32_t seq, uint32_t previous) {
    return radar_protocol_seq_newer(seq, previous);
}

static int valid_angle(int16_t angle) {
    return radar_protocol_valid_angle10(angle);
}

static int valid_distance(int16_t distance) {
    return radar_protocol_valid_distance_dm(distance);
}

static void test_packet_contract(void) {
    radar_packet_t pkt = {0};
    pkt.proto_ver = RADAR_PROTO_VER;
    pkt.payload_len = sizeof(pkt);
    pkt.rssi_to_peer = -60;
    pkt.angle_deg10 = 3599;
    pkt.dist_dm = 100;
    pkt.ap_mac[0] = 1;

    CHECK(sizeof(pkt) > 16, "v2 packet must extend v1 prefix");
    CHECK(pkt.payload_len == sizeof(pkt), "payload length must match v2 struct");
    CHECK(radar_protocol_valid_v2(&pkt, sizeof(pkt)), "valid v2 packet should pass");
    CHECK(valid_angle(pkt.angle_deg10), "359.9 degree angle should be valid");
    CHECK(valid_distance(pkt.dist_dm), "10m distance should be valid");

    pkt.angle_deg10 = 3601;
    CHECK(!valid_angle(pkt.angle_deg10), "angle > 360 should be rejected");
    pkt.angle_deg10 = -2;
    CHECK(!valid_angle(pkt.angle_deg10), "angle below invalid sentinel should be rejected");
    pkt.dist_dm = -2;
    CHECK(!valid_distance(pkt.dist_dm), "distance below invalid sentinel should be rejected");

    printf("  packet_contract: PASS\n");
}

static void test_sequence_freshness(void) {
    CHECK(seq_newer(2, 1), "increasing sequence should be newer");
    CHECK(!seq_newer(2, 2), "duplicate sequence should be rejected");
    CHECK(!seq_newer(1, 2), "rollback sequence should be rejected");
    CHECK(seq_newer(1, UINT32_MAX), "sequence wrap should remain monotonic");
    printf("  sequence_freshness: PASS\n");
}

static void test_sweep_resync(void) {
    uint32_t elapsed = 0;
    CHECK(radar_sweep_channel_at(elapsed, TEST_SWEEP_DURATION_MS) == 1,
          "hop 0 should use channel 1");
    elapsed = 100;
    CHECK(radar_sweep_channel_at(elapsed, TEST_SWEEP_DURATION_MS) == 6,
          "hop 1 should use channel 6");
    elapsed = 200;
    CHECK(radar_sweep_channel_at(elapsed, TEST_SWEEP_DURATION_MS) == 11,
          "hop 2 should use channel 11");
    elapsed = 400;
    CHECK(radar_sweep_channel_at(elapsed, TEST_SWEEP_DURATION_MS) == 6,
          "hop 4 should resync to channel 6");
    elapsed = TEST_SWEEP_DURATION_MS - 1;
    CHECK(radar_sweep_channel_at(elapsed, TEST_SWEEP_DURATION_MS) == 1,
          "last in-range millisecond should use channel 1");
    printf("  sweep_resync: PASS\n");
}

int main(void) {
    test_packet_contract();
    test_sequence_freshness();
    test_sweep_resync();
    if (failures) {
        fprintf(stderr, "FAILED: %d check(s)\n", failures);
        return 1;
    }
    printf("PASS: radar protocol rules verified\n");
    return 0;
}
