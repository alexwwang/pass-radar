#include "radar_csi.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_wifi.h"
#include "esp_err.h"
#endif

static int cmp_float(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

void radar_csi_history_init(radar_csi_history_t *history) {
    if (history) memset(history, 0, sizeof(*history));
}

bool radar_csi_history_push(radar_csi_history_t *history,
                            const float *amplitude, unsigned count) {
    if (!history || !amplitude || count < 8) return false;
    if (count > RADAR_CSI_MAX_SUBCARRIERS) count = RADAR_CSI_MAX_SUBCARRIERS;
    unsigned slot = history->head;
    memcpy(history->frames[slot].amplitude, amplitude, count * sizeof(float));
    history->frames[slot].count = count;
    history->head = (slot + 1u) % RADAR_CSI_HISTORY;
    if (history->count < RADAR_CSI_HISTORY) history->count++;
    return true;
}

float radar_csi_selectivity_db(const radar_csi_history_t *history) {
    if (!history || history->count == 0) return NAN;
    float values[RADAR_CSI_MAX_SUBCARRIERS];
    const radar_csi_frame_t *frame = &history->frames[
        (history->head + RADAR_CSI_HISTORY - 1u) % RADAR_CSI_HISTORY];
    unsigned n = 0;
    for (unsigned i = 0; i < frame->count; ++i) {
        if (isfinite(frame->amplitude[i]) && frame->amplitude[i] > 0.0f) {
            values[n++] = 20.0f * log10f(frame->amplitude[i]);
        }
    }
    if (n < 8) return NAN;
    qsort(values, n, sizeof(float), cmp_float);
    unsigned lo = (unsigned)(0.10f * (float)(n - 1));
    unsigned hi = (unsigned)(0.90f * (float)(n - 1));
    return values[hi] - values[lo];
}

static radar_csi_history_t s_history;
static uint8_t s_peer_mac[6];
static bool s_peer_mac_valid;
static bool s_window_open;
static int64_t s_window_start_us;
static unsigned s_received_frames;
static bool s_initialized;

bool radar_csi_push_amplitudes(const float *amplitude, unsigned count) {
    if (!radar_csi_history_push(&s_history, amplitude, count)) return false;
    s_received_frames++;
    return true;
}

#ifdef ESP_PLATFORM
static void csi_rx_cb(void *ctx, wifi_csi_info_t *info) {
    (void)ctx;
    if (!info || !info->buf || info->len < 16 || !s_peer_mac_valid ||
        memcmp(info->mac, s_peer_mac, sizeof(s_peer_mac)) != 0) return;

    // C3 CSI is interleaved signed I/Q bytes. Skip the invalid leading word
    // reported by the driver, then retain up to 64 subcarrier magnitudes.
    const unsigned skip = info->first_word_invalid ? 4u : 0u;
    if (skip >= info->len) return;
    const int8_t *iq = (const int8_t *)info->buf + skip;
    unsigned pairs = (info->len - skip) / 2u;
    if (pairs > RADAR_CSI_MAX_SUBCARRIERS) pairs = RADAR_CSI_MAX_SUBCARRIERS;
    float amplitude[RADAR_CSI_MAX_SUBCARRIERS];
    for (unsigned i = 0; i < pairs; ++i) {
        float re = (float)iq[i * 2u];
        float im = (float)iq[i * 2u + 1u];
        amplitude[i] = sqrtf(re * re + im * im);
    }
    if (s_window_open) (void)radar_csi_push_amplitudes(amplitude, pairs);
}
#endif

bool radar_csi_init(void) {
    if (s_initialized) return true;
    radar_csi_history_init(&s_history);
    s_peer_mac_valid = false;
    s_window_open = false;
    s_window_start_us = 0;
    s_received_frames = 0;
#ifdef ESP_PLATFORM
    wifi_csi_config_t config = {
        .lltf_en = true,
        .htltf_en = false,
        .stbc_htltf2_en = false,
        .ltf_merge_en = true,
        .channel_filter_en = true,
        .manu_scale = false,
        .shift = false,
    };
    if (esp_wifi_set_csi_config(&config) != ESP_OK ||
        esp_wifi_set_csi_rx_cb(csi_rx_cb, NULL) != ESP_OK ||
        esp_wifi_set_csi(true) != ESP_OK) {
        return false;
    }
#endif
    s_initialized = true;
    return true;
}

void radar_csi_set_peer_mac(const uint8_t mac[6]) {
    if (!mac) {
        memset(s_peer_mac, 0, sizeof(s_peer_mac));
        s_peer_mac_valid = false;
        return;
    }
    memcpy(s_peer_mac, mac, sizeof(s_peer_mac));
    s_peer_mac_valid = true;
}

void radar_csi_start_window(int64_t t0_us) {
    s_window_start_us = t0_us;
    s_window_open = true;
}

unsigned radar_csi_stop_window(void) {
    s_window_open = false;
    (void)s_window_start_us;
    return s_received_frames;
}

float radar_csi_recent_selectivity_db(void) {
    return radar_csi_selectivity_db(&s_history);
}

unsigned radar_csi_received_frames(void) { return s_received_frames; }
