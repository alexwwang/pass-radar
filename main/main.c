// main/main.c —— pass-radar:户外 ESP-NOW 定向雷达(测距/定向增强版)。
//
// 按键语义:
//   确定  短按   主页面=启动 360° 扫盘;结果页=返回主页面
//   确定  长按   主页面=1m 标定(两卡相距 1m 静止 3s);扫盘中=取消;结果页=返回
//   确定  LONG2  按住 3s 回 meta-pass 启动器(子固件适配约定,非系统策略)
//   上    双击   主页面↔诊断页(v1/v2 管线 A/B 回归 + 现场诊断)
//                实测长按 UP 曾误回启动器(疑为 ADC 梯度键长按电压漂移被
//                读成 OK),故诊断页入口用双击,避开一切长按
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

// 追踪页距离显示平滑系数(IIR,0~1,越大越跟手)。真机实测室内 RSSI 距离
// 在 1~5m 间跳动,把瞬时融合距离压平到可读。
#define TRACK_DIST_SMOOTH 0.35f

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

// 双端扫盘跟随与最近投票分歧(结果页/主页面置信度用)。
static uint32_t s_followed_sweep_id = 0;
static float    s_last_vote_spread = 0.0f;

// 追踪页平滑距离(INVALID 表示未初始化)。
static float s_track_dist_m = RADAR_DIST_INVALID;

// ---------- 前置声明 ----------
static void start_sweep(void);

// ---------- 融合(供追踪页与扫盘结果共用) ----------

static radar_fusion_result_t run_fusion(void) {
    float local = radar_espnow_filtered_rssi();
    bool  alive = radar_espnow_peer_alive();
    float peer  = alive ? (float)radar_espnow_peer_rssi() : RADAR_RSSI_ABSENT;
    uint8_t peer_identity[RADAR_BLE_ID_LEN];
    if (radar_espnow_peer_ble_identity(peer_identity)) {
        radar_ble_set_expected_identity(peer_identity);
    } else {
        // 链路丢失即解除 BLE 哨兵绑定,避免锁死旧目标。
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

    // 双端同扫跟随(调研 5.4 P0):对端开始扫盘且本端空闲时自动进入扫盘,
    // 按 sweep_id 一次性触发,错过 1s 以上不补扫(中途加入同步无意义)。
    if (page == RADAR_PAGE_MAIN || page == RADAR_PAGE_RESULT) {
        uint32_t peer_sweep_id = 0, peer_elapsed = 0;
        if (radar_espnow_peer_sweep(&peer_sweep_id, &peer_elapsed) &&
            peer_sweep_id != s_followed_sweep_id) {
            s_followed_sweep_id = peer_sweep_id;
            if (peer_elapsed <= 1000) {
                start_sweep();
                return;  // 已切换到扫盘页,本轮轮询结束
            }
        }
    }

    if (page == RADAR_PAGE_MAIN) {
        radar_fusion_result_t fr = run_fusion();
        int raw = radar_espnow_raw_rssi();
        // 追踪页距离 IIR 平滑;结果失效(OOR)时立即复位,避免旧值回显。
        if (fr.conf == RADAR_CONF_OOR || fr.distance_m <= 0.0f) {
            s_track_dist_m = RADAR_DIST_INVALID;
        } else if (s_track_dist_m == RADAR_DIST_INVALID) {
            s_track_dist_m = fr.distance_m;
        } else {
            s_track_dist_m += TRACK_DIST_SMOOTH * (fr.distance_m - s_track_dist_m);
        }
        radar_fusion_result_t shown = fr;
        shown.distance_m = s_track_dist_m;
        radar_ui_update_tracking(&shown, raw, radar_ble_peer_alive(),
                                 s_last_vote_spread);
    } else if (page == RADAR_PAGE_DEBUG) {
        // 诊断页:同速率复用融合管线(环境估计每 100ms 仍只喂一次),
        // 并排显示 v1 纯 RSSI 距离作 A/B 回归基线(调研 5.9)。
        radar_fusion_result_t fr = run_fusion();
        radar_debug_info_t di = {
            .raw_rssi = radar_espnow_raw_rssi(),
            .filtered_rssi = radar_espnow_filtered_rssi(),
            .v1_dist_m = radar_sweep_calc_distance(radar_espnow_filtered_rssi()),
            .fr = &fr,
            .ftm_valid = false,
            .ftm_dist_m = RADAR_DIST_INVALID,
            .ftm_spread_m = 0.0f,
            .ftm_bursts = 0,
            .ble_alive = radar_ble_peer_alive(),
            .ble_rssi = radar_ble_peer_rssi(),
            .env = s_env_est.env,
            .env_mean_rssi = s_env_est.mean_rssi,
            .tx_ref_sym = s_tx_ref_sym,
            .vote_spread_deg = s_last_vote_spread,
        };
        radar_ftm_snapshot_t snap;
        if (radar_ftm_get_snapshot(&snap)) {
            di.ftm_valid = true;
            di.ftm_dist_m = snap.distance_m;
            di.ftm_spread_m = snap.spread_m;
            di.ftm_bursts = snap.valid_bursts;
        } else {
            // 无有效快照时显示最近一次失败原因(status 编码),现场定位共存/对端问题。
            di.ftm_last_status = (int)radar_ftm_last_status();
        }
        radar_ui_update_debug(&di);
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

    // 停止采样并取回数据(恢复固定信道),恢复 FTM 与 BLE 扫描
    unsigned count = radar_espnow_stop_sweep();
    radar_ftm_pause(false);
    radar_ble_pause(false);
    radar_sample_t samples[RADAR_MAX_SAMPLES];
    count = radar_espnow_get_samples(samples, RADAR_MAX_SAMPLES);

    // 旋转速度自校正(调研 5.4 P1):估计实际完成时刻,剔除提前停转后的
    // 尾部平台样本,并缩短时间→角度映射基准。未检出修正时保持 4s。
    uint32_t sweep_duration = RADAR_SWEEP_DURATION_MS;
    count = radar_sweep_effective_duration_ms(samples, count,
                                              RADAR_SWEEP_DURATION_MS,
                                              &sweep_duration);
    if (sweep_duration != RADAR_SWEEP_DURATION_MS) {
        ESP_LOGI(TAG, "sweep self-correction: duration %u -> %lu ms (%u samples)",
                 RADAR_SWEEP_DURATION_MS, (unsigned long)sweep_duration, count);
    }

    // 角度:信道分集谐波拟合 + 投票;失败回退全局插值峰检测。
    bool sweep_valid = count >= 12;
    for (unsigned i = 1; sweep_valid && i < count; ++i) {
        if (samples[i].timestamp_ms <= samples[i - 1].timestamp_ms ||
            samples[i].timestamp_ms > sweep_duration) {
            sweep_valid = false;
        }
    }
    float angle = RADAR_ANGLE_INVALID;
    float spread = sweep_valid ? 0.0f : 90.0f;
    float ch_angles[4];
    unsigned nch = sweep_valid ? radar_sweep_channel_angles(samples, count,
                                                              sweep_duration,
                                                              ch_angles, 4) : 0;
    if (nch > 0) {
        angle = radar_sweep_angle_vote(ch_angles, nch, &spread);
    } else if (sweep_valid && radar_sweep_peak_prominence(samples, count) >=
               RADAR_SWEEP_MIN_PROMINENCE_DB) {
        angle = radar_sweep_peak_angle_interp(samples, count, sweep_duration);
        spread = 45.0f;  // 单源估计,给保守分歧
    }

    // 双端角度融合(调研 5.4 P0):对端结果随协议 v2 持续广播,扫盘结束时
    // 仍有效;双观测独立误差减半,环形均值融合,分歧计入 spread。
    float peer_angle = radar_espnow_peer_angle();
    if (angle >= 0.0f && peer_angle >= 0.0f) {
        float fused = radar_fusion_angle_pair(angle, peer_angle);
        if (fused >= 0.0f) {
            float d = radar_fusion_angle_diff(angle, peer_angle);
            angle = fused;
            if (d > spread) spread = d;
        }
    } else if (angle < 0.0f && peer_angle >= 0.0f) {
        // 本端无方向但对端有效:沿用对端结果,标保守分歧。
        angle = peer_angle;
        spread = 45.0f;
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
    s_last_vote_spread = spread;

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
    radar_ble_pause(true);   // 让出射频:跳频 + 扫描无法同时进行
    s_sweep_start_ms = esp_timer_get_time() / 1000;
    radar_espnow_start_sweep();
    radar_ui_show_sweeping();

    s_sweep_timer = lv_timer_create(sweep_done, RADAR_SWEEP_DURATION_MS, NULL);
    lv_timer_set_repeat_count(s_sweep_timer, 1);
}

static void start_calib(void) {
    radar_ftm_pause(true);   // 标定期间稳定信道,暂停 FTM 会话
    radar_ble_pause(true);   // 标定采样不受扫描干扰
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
        } else if (btn == BSP_BTN_UP && ev == BSP_BTN_DOUBLE) {
            // 诊断/回归页:meta-pass 占用任意键长按(回启动器),改用双击。
            radar_ui_show_debug();
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
    radar_ftm_pause(false);
    radar_ble_pause(false);
    radar_ui_show_main();
}
        break;

    case RADAR_PAGE_CALIB:
        // 长按或短按取消标定
        if (btn == BSP_BTN_OK && (ev == BSP_BTN_LONG || ev == BSP_BTN_CLICK)) {
        if (s_calib_timer) {
            lv_timer_delete(s_calib_timer);
            s_calib_timer = NULL;
        }
        radar_ftm_pause(false);
        radar_ble_pause(false);
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

    case RADAR_PAGE_DEBUG:
        // OK 返回追踪页;UP 双击再次切换(页面向)。
        if (btn == BSP_BTN_OK ||
            (btn == BSP_BTN_UP && ev == BSP_BTN_DOUBLE)) {
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
