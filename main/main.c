// main/main.c —— pass-radar:户外 ESP-NOW 定向雷达。
//
// 按键语义:
//   确定  短按   主页面=启动 360° 扫盘;结果页=返回主页面
//   确定  长按   任意页=返回主页面(取消扫盘)
//   上/下 短按   (保留,暂不分配)
#include <stdio.h>
#include <string.h>

#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "lvgl.h"
#include "radar_espnow.h"
#include "radar_sweep.h"
#include "radar_ui.h"

static const char *TAG = "pass-radar";

// ---------- 状态 ----------
static lv_timer_t *s_poll_timer;   // 100ms 轮询 RSSI 并更新 UI
static lv_timer_t *s_sweep_timer;  // 4s 扫盘一次性定时器

// ---------- 定时器回调(LVGL 任务上下文,可直接操作 lv_obj) ----------

// 100ms 轮询:读 ESP-NOW 最新 RSSI,更新主页面显示。
static void poll_tick(lv_timer_t *t) {
    (void)t;
    if (radar_ui_current_page() != RADAR_PAGE_MAIN) return;

    float filtered = radar_espnow_filtered_rssi();
    int raw = radar_espnow_raw_rssi();
    float dist = radar_sweep_calc_distance(filtered);

    radar_ui_update_tracking(filtered, raw, dist);
}

// 扫盘进度更新(由 poll_tick 在扫盘页面时代用)。
static void sweep_progress_tick(lv_timer_t *t) {
    (void)t;
    if (radar_ui_current_page() != RADAR_PAGE_SWEEPING) return;

    static int64_t sweep_start_ms = 0;
    if (sweep_start_ms == 0) {
        sweep_start_ms = esp_timer_get_time() / 1000;
    }
    int64_t elapsed = (esp_timer_get_time() / 1000) - sweep_start_ms;
    float progress = (float)elapsed / (float)RADAR_SWEEP_DURATION_MS;
    if (progress > 1.0f) progress = 1.0f;
    radar_ui_update_sweep_progress(progress);
}

// 4s 扫盘结束:停止采样,计算峰值角度,显示结果。
static void sweep_done(lv_timer_t *t) {
    (void)t;
    if (radar_ui_current_page() != RADAR_PAGE_SWEEPING) return;

    // 停止采样并取回数据
    unsigned count = radar_espnow_stop_sweep();
    radar_sample_t samples[RADAR_MAX_SAMPLES];
    count = radar_espnow_get_samples(samples, RADAR_MAX_SAMPLES);

    // 计算峰值角度与 RSSI
    int angle = radar_sweep_peak_angle(samples, count, RADAR_SWEEP_DURATION_MS);
    float peak_rssi = radar_sweep_peak_rssi(samples, count);
    float dist = radar_sweep_calc_distance(peak_rssi);
    bool weak = (peak_rssi < RADAR_RSSI_MIN_VALID);

    radar_ui_show_result(angle, dist, weak);

    // 清理一次性定时器
    if (s_sweep_timer) {
        lv_timer_delete(s_sweep_timer);
        s_sweep_timer = NULL;
    }
}

// ---------- 扫盘启动 ----------

static void start_sweep(void) {
    radar_espnow_start_sweep();
    radar_ui_show_sweeping();

    // 创建 4s 一次性定时器
    s_sweep_timer = lv_timer_create(sweep_done, RADAR_SWEEP_DURATION_MS, NULL);
    lv_timer_set_repeat_count(s_sweep_timer, 1);
}

// ---------- 按键分发(button 任务,须持有锁) ----------

static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    if (!bsp_lvgl_lock(500)) return;

    radar_page_t page = radar_ui_current_page();

    switch (page) {
    case RADAR_PAGE_MAIN:
        if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
            start_sweep();
        }
        break;

    case RADAR_PAGE_SWEEPING:
        // 长按取消扫盘
        if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
            radar_espnow_stop_sweep();
            if (s_sweep_timer) {
                lv_timer_delete(s_sweep_timer);
                s_sweep_timer = NULL;
            }
            radar_ui_show_main();
        }
        break;

    case RADAR_PAGE_RESULT:
        if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
            radar_ui_show_main();
        } else if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
            radar_ui_show_main();
        }
        break;
    }

    bsp_lvgl_unlock();
}

// ---------- 入口 ----------

void app_main(void) {
    ESP_LOGI(TAG, "pass-radar 启动");

    bsp_i2c_init();
    bsp_i2c_scan();

    // 显示是 UI 硬依赖,失败直接退出(沿用基线纪律)。
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败,雷达无法启动。"
                      "检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(100);

    // 按键为软依赖:失败只影响交互,不阻塞启动。
    if (bsp_button_init(on_key, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败,设备将无法操作");
    }

    // ESP-NOW 初始化(Wi-Fi 依赖 NVS/netif/event-loop,由 radar_espnow 内部处理)。
    if (radar_espnow_init() != ESP_OK) {
        ESP_LOGE(TAG, "ESP-NOW 初始化失败");
    }

    if (bsp_lvgl_lock(1000)) {
        radar_ui_init();

        // 100ms 轮询定时器:更新追踪显示 + 扫盘进度
        s_poll_timer = lv_timer_create(poll_tick, 100, NULL);
        s_sweep_timer = NULL;

        bsp_lvgl_unlock();
    }

    ESP_LOGI(TAG, "就绪");
}
