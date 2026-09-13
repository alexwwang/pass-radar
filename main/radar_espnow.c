// radar_espnow.c —— ESP-NOW 广播接收 + RSSI 采集。
// 接收回调运行于 Wi-Fi 任务,用 volatile 变量向 LVGL 定时器传递数据。
#include "radar_espnow.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include <string.h>

static const char *TAG = "radar_espnow";

// ESP-NOW 接收回调是 wifi_task 上下文,volatile 保证编译器不优化读取。
static volatile float s_filtered_rssi = -90.0f;
static volatile int   s_raw_rssi      = -90;
static volatile bool  s_sweeping      = false;

// 扫盘采样缓冲区(单写者:ESP-NOW 回调;单读者:UI 定时器)。
static radar_sample_t s_samples[RADAR_MAX_SAMPLES];
static volatile unsigned s_sample_count = 0;
static int64_t s_sweep_start_us = 0;

// 卡尔曼滤波器实例(全局单例,回调里就地更新)。
static radar_kalman_t s_kalman;

// NVS 就绪标记(esp_wifi_init 依赖 NVS)。
static bool s_nvs_ready;
static bool s_netif_ready;
static bool s_event_loop_ready;
static bool s_initialized;

// ---------- ESP-NOW 接收回调(Wi-Fi 任务,不得阻塞/触碰 LVGL) ----------

static void espnow_recv_cb(const esp_now_recv_info_t *recv_info,
                           const uint8_t *data, int len) {
    (void)data; (void)len;

    int raw = recv_info->rx_ctrl->rssi;
    s_raw_rssi = raw;

    // 卡尔曼滤波更新
    float filtered = radar_kalman_update(&s_kalman, (float)raw);
    s_filtered_rssi = filtered;

    // 扫盘采样
    if (s_sweeping && s_sample_count < RADAR_MAX_SAMPLES) {
        uint32_t elapsed_ms = (uint32_t)((esp_timer_get_time() - s_sweep_start_us) / 1000);
        s_samples[s_sample_count].timestamp_ms = elapsed_ms;
        s_samples[s_sample_count].rssi = filtered;
        s_sample_count++;
    }
}

// ---------- 初始化 ----------

static esp_err_t nvs_prepare(void) {
    if (s_nvs_ready) return ESP_OK;
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 初始化失败: %s", esp_err_to_name(err));
        return err;
    }
    s_nvs_ready = true;
    return ESP_OK;
}

static esp_err_t network_prepare(void) {
    if (!s_netif_ready) {
        esp_err_t err = esp_netif_init();
        if (err != ESP_OK) return err;
        s_netif_ready = true;
    }
    if (!s_event_loop_ready) {
        esp_err_t err = esp_event_loop_create_default();
        if (err != ESP_OK) return err;
        s_event_loop_ready = true;
    }
    return ESP_OK;
}

esp_err_t radar_espnow_init(void) {
    if (s_initialized) return ESP_OK;

    esp_err_t err;

    err = nvs_prepare();
    if (err != ESP_OK) return err;

    err = network_prepare();
    if (err != ESP_OK) return err;

    // Wi-Fi 初始化(ESP-NOW 依赖)
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi init: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) return err;

    // 最大功率发射(19.5dBm ≈ 78 in 0.25dBm units)
    esp_wifi_set_max_tx_power(78);

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi start: %s", esp_err_to_name(err));
        return err;
    }

    // ESP-NOW 初始化
    err = esp_now_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ESP-NOW init: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_now_register_recv_cb(espnow_recv_cb);
    if (err != ESP_OK) return err;

    // 卡尔曼滤波器初始化
    radar_kalman_init(&s_kalman, -70.0f);

    s_initialized = true;
    ESP_LOGI(TAG, "ESP-NOW 就绪");
    return ESP_OK;
}

// ---------- 广播发射 ----------

static const uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static esp_timer_handle_t s_broadcast_timer;
static radar_packet_t s_tx_packet;
static volatile bool s_broadcasting;

static void broadcast_tick(void *arg) {
    (void)arg;
    s_tx_packet.sequence++;
    esp_now_send(BROADCAST_MAC, (const uint8_t *)&s_tx_packet, sizeof(s_tx_packet));
}

esp_err_t radar_espnow_start_broadcast(const char *device_name) {
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (s_broadcasting) return ESP_OK;

    // 注册广播 peer
    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, BROADCAST_MAC, 6);
    peer.channel = 0;  // 当前信道
    peer.encrypt = false;
    esp_err_t err = esp_now_add_peer(&peer);
    if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST) return err;

    // 填充数据包
    memset(&s_tx_packet, 0, sizeof(s_tx_packet));
    strncpy(s_tx_packet.device_name, device_name, RADAR_PACKET_NAME_LEN - 1);
    s_tx_packet.sequence = 0;

    // 创建 50ms 周期定时器(20Hz)
    esp_timer_create_args_t timer_args = {
        .callback = broadcast_tick,
        .name = "radar_tx",
    };
    err = esp_timer_create(&timer_args, &s_broadcast_timer);
    if (err != ESP_OK) return err;

    err = esp_timer_start_periodic(s_broadcast_timer, 50000);  // 50ms in us
    if (err != ESP_OK) {
        esp_timer_delete(s_broadcast_timer);
        return err;
    }

    s_broadcasting = true;
    ESP_LOGI(TAG, "广播发射启动: %s @20Hz", device_name);
    return ESP_OK;
}

void radar_espnow_stop_broadcast(void) {
    if (!s_broadcasting) return;
    esp_timer_stop(s_broadcast_timer);
    esp_timer_delete(s_broadcast_timer);
    s_broadcast_timer = NULL;
    s_broadcasting = false;
    esp_now_del_peer(BROADCAST_MAC);
}

void radar_espnow_deinit(void) {
    radar_espnow_stop_broadcast();
    esp_now_unregister_recv_cb();
    esp_now_deinit();
    esp_wifi_stop();
    esp_wifi_deinit();
    s_initialized = false;
}

// ---------- 查询接口 ----------

float radar_espnow_filtered_rssi(void) { return s_filtered_rssi; }
int   radar_espnow_raw_rssi(void)      { return s_raw_rssi; }
bool  radar_espnow_is_sweeping(void)   { return s_sweeping; }

// ---------- 扫盘采样控制 ----------

void radar_espnow_start_sweep(void) {
    s_sample_count = 0;
    s_sweep_start_us = esp_timer_get_time();
    s_sweeping = true;
}

unsigned radar_espnow_stop_sweep(void) {
    s_sweeping = false;
    return s_sample_count;
}

unsigned radar_espnow_get_samples(radar_sample_t *out, unsigned max_count) {
    unsigned n = s_sample_count;
    if (n > max_count) n = max_count;
    memcpy(out, s_samples, n * sizeof(radar_sample_t));
    return n;
}
