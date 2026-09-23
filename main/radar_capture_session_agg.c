#include "radar_capture_session_agg.h"

#include <string.h>

// 环形统计工具：排序缓冲固定 RADAR_CAPTURE_AGG_MAX_SESSIONS（≤16 项），
// 栈上 qsort 使用安全（host 与设备同路径）。
static void sort_floats(float *values, unsigned count) {
    for (unsigned i = 1; i < count; ++i) {
        float key = values[i];
        unsigned j = i;
        while (j > 0 && values[j - 1] > key) {
            values[j] = values[j - 1];
            --j;
        }
        values[j] = key;
    }
}

void radar_capture_session_agg_reset(radar_capture_session_agg_t *agg) {
    if (!agg) return;
    memset(agg, 0, sizeof(*agg));
}

void radar_capture_session_agg_add(radar_capture_session_agg_t *agg,
                                   unsigned session_id, float raw_median_m,
                                   uint8_t raw_bursts) {
    if (!agg || session_id == 0u) return;
    if (session_id == agg->last_id) return; // 同一 session 的重复轮询
    if (!(raw_median_m > 0.0f) || raw_median_m > 1000.0f) return;
    if (agg->count >= RADAR_CAPTURE_AGG_MAX_SESSIONS) return;
    agg->medians[agg->count] = raw_median_m;
    agg->bursts[agg->count] = raw_bursts;
    agg->last_id = session_id;
    agg->count++;
}

bool radar_capture_session_agg_refresh(radar_capture_session_agg_t *agg) {
    if (!agg) return false;
    if (agg->count == 0u) {
        agg->ftm_raw_median_m = 0.0f;
        agg->ftm_raw_iqr_m = 0.0f;
        agg->ftm_raw_sessions = 0u;
        agg->ftm_raw_bursts = 0u;
        return false;
    }

    unsigned n = agg->count;
    float buf[RADAR_CAPTURE_AGG_MAX_SESSIONS];
    memcpy(buf, agg->medians, n * sizeof(buf[0]));
    sort_floats(buf, n);

    agg->ftm_raw_median_m = (n & 1u) ? buf[n / 2u]
                                     : 0.5f * (buf[n / 2u - 1u] + buf[n / 2u]);
    unsigned q1 = n / 4u;
    unsigned q3 = (3u * n) / 4u;
    if (q3 >= n) q3 = n - 1u;
    agg->ftm_raw_iqr_m = buf[q3] - buf[q1];

    agg->ftm_raw_sessions = (uint16_t)n;
    uint32_t bursts = 0u;
    for (unsigned i = 0; i < n; ++i) bursts += agg->bursts[i];
    agg->ftm_raw_bursts = bursts > (uint32_t)UINT16_MAX ? UINT16_MAX
                                                        : (uint16_t)bursts;
    return true;
}
