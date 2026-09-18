// main/main.c —— pass-radar:户外 ESP-NOW 定向雷达(测距/定向增强版)。
//
// 按键语义:
//   确定  短按   主页面=启动 360° 扫盘;结果页=返回主页面
//   确定  长按   主页面=1m 标定(两卡相距 1m 静止 3s);扫盘中=取消;结果页=返回
//   上/下 短按   (保留,暂不分配)
//
// 数据流:
//   ESP-NOW 双向 RSSI + Wi-Fi FTM(RTT)+ BLE 哨兵
//     → radar_fusion(纯逻辑)→ 距离/置信度 → UI
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "lvgl.h"
#include "metapass_hook.h"
#include "radar_ble.h"
#include "radar_espnow.h"
#include "radar_ftm.h"
#include "radar_fusion.h"
#include "radar_sweep.h"
#include "radar_ui.h"
#include <math.h>

static const char *TAG = "pass-radar";

// 标定采样时长
#define CALIB_DURATION_MS 3000

// ---------- 状态 ----------
static lv_timer_t *s_poll_timer;    // 100ms 轮询融合并更新 UI
static lv_timer_t *s_sweep_timer;   // 4s 扫盘一次性定时器
static lv_timer_t *s_calib_timer;   // 3s 标定一次性定时器
static int64_t     s_sweep_start_ms;

static radar_env_est_t s_env_est;   // 环境自适应估计器
static float s_tx_ref_sym = RADAR_TX_POWER_REF;  // 1m 对称参考(可标定)

// 标定采样窗口(LVGL 任务内使用)。
#define CALIB_MAX_SAMPLES 32
static float   s_calib_samples[CALIB_MAX_SAMPLES];
static unsigned s_calib_cnt = 0;
static float   s_calib_max_gap_db = 0.0f;

// ---------- 融合(供追踪页与扫盘结果共用) ----------

static radar_fusion_result_t run_fusion(void) {
    float local = radar_espnow_filtered_rssi();
    bool  alive = radar_espnow_peer_alive();
    float peer  = alive ? (float)radar_espnow_peer_rssi() : RADAR_RSSI_ABSENT;
    uint8_t peer_identity[RADAR_BLE_ID_LEN];
    if (radar_espnow_peer_ble_identity(peer_identity)) {
        radar_ble_set_expected_identity(peer_identity);
    } else {
        radar_ble_set_expected_identity(NULL);
    }

    // 环境估计(每次轮询喂样本)
    radar_env_t env = radar_env_est_update(&s_env_est, local);

    radar_ftm_snapshot_t ftm_snapshot;
    radar_fusion_ftm_observation_t ftm = {0};
    if (radar_ftm_get_snapshot(&ftm_snapshot)) {
        ftm.valid = true;
        ftm.fresh = true;
        ftm.distance_m = ftm_snapshot.distance_m;
        ftm.spread_m = ftm_snapshot.spread_m;
        ftm.valid_bursts = ftm_snapshot.valid_bursts;
    }
    return radar_fusion_update_ftm(&ftm, local, peer, s_tx_ref_sym, env);
}

// ---------- 定时器回调(LVGL 任务上下文,可直接操作 lv_obj) ----------

// 100ms 轮询:按页面更新显示(追踪融合 / 扫盘进度 / 标定采样)。
static void poll_tick(lv_timer_t *t) {
    (void)t;
    radar_page_t page = radar_ui_current_page();

    // FTM 周期驱动(任何页面都需要;跳频期内部自动跳过)
    radar_ftm_tick();

    if (page == RADAR_PAGE_MAIN) {
        radar_fusion_result_t fr = run_fusion();
        int raw = radar_espnow_raw_rssi();
        radar_ui_update_tracking(&fr, raw, radar_ble_peer_alive());
    } else if (page == RADAR_PAGE_SWEEPING) {
        int64_t elapsed = (esp_timer_get_time() / 1000) - s_sweep_start_ms;
        float progress = (float)elapsed / (float)RADAR_SWEEP_DURATION_MS;
        if (progress > 1.0f) progress = 1.0f;
        radar_ui_update_sweep_progress(progress);
    } else if (page == RADAR_PAGE_CALIB) {
        // 累计对称 RSSI
        float local = radar_espnow_filtered_rssi();
        float peer  = (float)radar_espnow_peer_rssi();
        if (radar_fusion_sym_valid(local, peer) && s_calib_cnt < CALIB_MAX_SAMPLES) {
            s_calib_samples[s_calib_cnt++] = radar_fusion_sym_rssi(local, peer);
            float gap = fabsf(local - peer);
            if (gap > s_calib_max_gap_db) s_calib_max_gap_db = gap;
        }
        // 进度条复用扫盘计时基
        int64_t elapsed = (esp_timer_get_time() / 1000) - s_sweep_start_ms;
        float progress = (float)elapsed / (float)CALIB_DURATION_MS;
        if (progress > 1.0f) progress = 1.0f;
        radar_ui_update_calib(progress);
    }
}

// 4s 扫盘结束:信道分集投票 → 双源距离融合 → 置信度结果页。
static void sweep_done(lv_timer_t *t) {
    (void)t;
    if (radar_ui_current_page() != RADAR_PAGE_SWEEPING) return;

    // 停止采样并取回数据(恢复固定信道),恢复 FTM
    unsigned count = radar_espnow_stop_sweep();
    radar_ftm_pause(false);
    radar_sample_t samples[RADAR_MAX_SAMPLES];
    count = radar_espnow_get_samples(samples, RADAR_MAX_SAMPLES);

    // 角度:信道分集谐波拟合 + 投票;失败回退全局插值峰检测。
    bool sweep_valid = count >= 12;
    for (unsigned i = 1; sweep_valid && i < count; ++i) {
        if (samples[i].timestamp_ms <= samples[i - 1].timestamp_ms ||
            samples[i].timestamp_ms > RADAR_SWEEP_DURATION_MS) {
            sweep_valid = false;
        }
    }
    float angle = RADAR_ANGLE_INVALID;
    float spread = sweep_valid ? 0.0f : 90.0f;
    float ch_angles[4];
    unsigned nch = sweep_valid ? radar_sweep_channel_angles(samples, count,
                                                              RADAR_SWEEP_DURATION_MS,
                                                              ch_angles, 4) : 0;
    if (nch > 0) {
        angle = radar_sweep_angle_vote(ch_angles, nch, &spread);
    } else if (sweep_valid && radar_sweep_peak_prominence(samples, count) >=
               RADAR_SWEEP_MIN_PROMINENCE_DB) {
        angle = radar_sweep_peak_angle_interp(samples, count,
                                              RADAR_SWEEP_DURATION_MS);
        spread = 45.0f;  // 单源估计,给保守分歧
    }

    // 距离:走融合管线(FTM 在扫盘期间暂停,用最近有效值)
    radar_fusion_result_t fr = run_fusion();
    float peak_rssi = radar_sweep_peak_rssi(samples, count);
    bool weak = (peak_rssi < RADAR_RSSI_MIN_VALID);

    // 方向置信度叠加:投票分歧大则降一级
    radar_confidence_t conf = fr.conf;
    if (angle >= 0.0f && !weak) {
        if (spread > 45.0f && conf > RADAR_CONF_LOW) conf = RADAR_CONF_LOW;
        else if (spread > 20.0f && conf > RADAR_CONF_MED) conf = RADAR_CONF_MED;
    }

    // 广播本端结果(供对端参考显示)
    radar_espnow_report_result(angle, fr.distance_m);

    radar_ui_show_result(angle >= 0.0f ? (int)(angle + 0.5f) : -1,
                         fr.distance_m, conf, spread, weak);

    if (s_sweep_timer) {
        lv_timer_delete(s_sweep_timer);
        s_sweep_timer = NULL;
    }
}

// 3s 标定结束:中值/截尾平均并通过稳定性门控后存 NVS。
static int calib_cmp_float(const void *a, const void *b) {
    float fa = *(const float *)a;
    float fb = *(const float *)b;
    return (fa > fb) - (fa < fb);
}

static void calib_done(lv_timer_t *t) {
    (void)t;
    if (radar_ui_current_page() != RADAR_PAGE_CALIB) return;

    if (s_calib_cnt >= 10 && s_calib_max_gap_db <= RADAR_SYM_MAX_GAP_DB) {
        qsort(s_calib_samples, s_calib_cnt, sizeof(float), calib_cmp_float);
        unsigned begin = s_calib_cnt >= 5 ? 1 : 0;
        unsigned end = s_calib_cnt >= 5 ? s_calib_cnt - 1 : s_calib_cnt;
        float sum = 0.0f;
        for (unsigned i = begin; i < end; ++i) sum += s_calib_samples[i];
        float sym = sum / (float)(end - begin);
        float variance = 0.0f;
        for (unsigned i = begin; i < end; ++i) {
            float delta = s_calib_samples[i] - sym;
            variance += delta * delta;
        }
        variance /= (float)(end - begin);
        if (isfinite(sym) && isfinite(variance) && variance <= 4.0f) {
            s_tx_ref_sym = radar_fusion_calib_ref(sym);
            if (radar_espnow_calib_save(s_tx_ref_sym) == ESP_OK) {
                ESP_LOGI(TAG, "标定完成: tx_ref_sym=%.1f (%u samples, var %.2f)",
                         (double)s_tx_ref_sym, s_calib_cnt, (double)variance);
            } else {
                ESP_LOGW(TAG, "标定值保存失败");
            }
        } else {
            ESP_LOGW(TAG, "标定方差过大或非法(var %.2f),保持原值", (double)variance);
        }
    } else {
        ESP_LOGW(TAG, "标定未通过门控(samples=%u gap=%.1f),保持原值",
                 s_calib_cnt, (double)s_calib_max_gap_db);
    }

    if (s_calib_timer) {
        lv_timer_delete(s_calib_timer);
        s_calib_timer = NULL;
    }
    radar_ui_show_main();
}

// ---------- 扫盘/标定启动 ----------

static void start_sweep(void) {
    radar_ftm_pause(true);   // 跳频期间暂停 FTM
    s_sweep_start_ms = esp_timer_get_time() / 1000;
    radar_espnow_start_sweep();
    radar_ui_show_sweeping();

    s_sweep_timer = lv_timer_create(sweep_done, RADAR_SWEEP_DURATION_MS, NULL);
    lv_timer_set_repeat_count(s_sweep_timer, 1);
}

static void start_calib(void) {
    memset(s_calib_samples, 0, sizeof(s_calib_samples));
    s_calib_cnt = 0;
    s_calib_max_gap_db = 0.0f;
    s_sweep_start_ms = esp_timer_get_time() / 1000;  // 复用计时基
    radar_ui_show_calib();

    s_calib_timer = lv_timer_create(calib_done, CALIB_DURATION_MS, NULL);
    lv_timer_set_repeat_count(s_calib_timer, 1);
}

// ---------- 按键分发(button 任务,须持有锁) ----------

static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    if (!bsp_lvgl_lock(500)) return;

    // LONG2:退回 meta-pass 启动器(先于 LONG 触发,LONG 视为安全的返回上级)。
    if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG2) {
        bsp_lvgl_unlock();
        metapass_return_to_launcher();
        return;  // 不会到达(restart)
    }

    radar_page_t page = radar_ui_current_page();

    switch (page) {
    case RADAR_PAGE_MAIN:
        if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
            start_sweep();
        } else if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
            start_calib();
        }
        break;

    case RADAR_PAGE_SWEEPING:
        // 长按取消扫盘
        if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
            radar_espnow_stop_sweep();
            radar_ftm_pause(false);
            if (s_sweep_timer) {
                lv_timer_delete(s_sweep_timer);
                s_sweep_timer = NULL;
            }
            radar_ui_show_main();
        }
        break;

    case RADAR_PAGE_CALIB:
        // 长按取消标定
        if (btn == BSP_BTN_OK && (ev == BSP_BTN_LONG || ev == BSP_BTN_CLICK)) {
            if (s_calib_timer) {
                lv_timer_delete(s_calib_timer);
                s_calib_timer = NULL;
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

    // 环境估计器与标定值
    radar_env_est_init(&s_env_est);

    // 先构建 UI:即使射频链路不可用,用户也能看到雷达界面(显示无信号)。
    if (bsp_lvgl_lock(1000)) {
        radar_ui_init();

        // 100ms 轮询定时器:融合 + 追踪显示 + 扫盘/标定进度
        s_poll_timer = lv_timer_create(poll_tick, 100, NULL);
        s_sweep_timer = NULL;
        s_calib_timer = NULL;

        bsp_lvgl_unlock();
    }

    // ESP-NOW 初始化放在 UI 之后:失败不阻塞界面,仅影响信号采集。
    if (radar_espnow_init() != ESP_OK) {
        ESP_LOGE(TAG, "ESP-NOW 初始化失败,雷达将无法接收信号");
    } else {
        // 标定值(NVS,失败保持默认 -42dBm)
        float calib;
        if (radar_espnow_calib_load(&calib) == ESP_OK &&
            isfinite(calib) && calib >= -60.0f && calib <= -30.0f) {
            s_tx_ref_sym = calib;
            ESP_LOGI(TAG, "载入标定值 tx_ref_sym=%.1f", (double)calib);
        } else {
            ESP_LOGW(TAG, "标定值无效,回退默认 tx_ref_sym=%.1f",
                     (double)RADAR_TX_POWER_REF);
            s_tx_ref_sym = RADAR_TX_POWER_REF;
        }

        // FTM 测距(软依赖:失败降级为纯 RSSI 融合)
        if (radar_ftm_init() != ESP_OK) {
            ESP_LOGW(TAG, "FTM 初始化失败,距离将仅基于 RSSI");
        }

        // BLE 哨兵链路(软依赖:失败无超界提示)
        if (radar_ble_init() != ESP_OK) {
            ESP_LOGW(TAG, "BLE 哨兵初始化失败");
        }

        // 自动启动 20Hz 广播发射:两台设备刷同一固件即可互相对测。
        if (radar_espnow_start_broadcast("pass-radar") != ESP_OK) {
            ESP_LOGE(TAG, "广播发射启动失败");
        }
    }
    // meta-pass 子固件适配:签名固件常驻,未签名试运行(重启回启动器)。
    esp_err_t mv = metapass_mark_valid();
    if (mv != ESP_OK) {
        ESP_LOGI(TAG, "试运行模式(下次重启回启动器): %s", esp_err_to_name(mv));
    }

    ESP_LOGI(TAG, "就绪");
}
