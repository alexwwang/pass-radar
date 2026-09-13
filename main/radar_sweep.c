#include "radar_sweep.h"
#include <math.h>

float radar_sweep_calc_distance(float rssi) {
    // 自由空间路径损耗:distance = 10^((txPower - rssi) / (10 * n))
    return powf(10.0f, (RADAR_TX_POWER_REF - rssi) / (10.0f * RADAR_PATH_LOSS_N));
}

int radar_sweep_peak_angle(const radar_sample_t *samples, unsigned count,
                           uint32_t sweep_duration_ms) {
    if (count == 0 || sweep_duration_ms == 0) return -1;

    float max_rssi = -999.0f;
    uint32_t peak_time = 0;
    unsigned i;

    for (i = 0; i < count; i++) {
        if (samples[i].rssi > max_rssi) {
            max_rssi = samples[i].rssi;
            peak_time = samples[i].timestamp_ms;
        }
    }

    float ratio = (float)peak_time / (float)sweep_duration_ms;
    if (ratio > 1.0f) ratio = 1.0f;

    return (int)(ratio * 360.0f);
}

float radar_sweep_peak_rssi(const radar_sample_t *samples, unsigned count) {
    if (count == 0) return -999.0f;

    float max_rssi = samples[0].rssi;
    for (unsigned i = 1; i < count; i++) {
        if (samples[i].rssi > max_rssi) max_rssi = samples[i].rssi;
    }
    return max_rssi;
}
