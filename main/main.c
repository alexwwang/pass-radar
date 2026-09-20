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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lvgl.h"
#include "metapass_hook.h"
#include "radar_ble.h"
#include "radar_espnow.h"
#include "radar_ftm.h"
#include "radar_fusion.h"
#include "radar_obstruct.h"
#include "radar_csi.h"
#include "radar_diag_persist.h"
#include "radar_capture.h"
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
static radar_level_est_t s_level_est;
static radar_obstruct_hold_t s_obstruct_hold;
static radar_obstruct_class_t s_obstruct_class = OBS_NONE_LOS;
static radar_hint_t s_obstruct_hint = HINT_NONE;
static float s_tx_ref_sym = RADAR_TX_POWER_REF;  // 1m 对称参考(可标定)

// 标定采样窗口(LVGL 任务内使用)。
#define CALIB_MAX_SAMPLES 32
static float   s_calib_samples[CALIB_MAX_SAMPLES];
static unsigned s_calib_cnt = 0;
static float   s_calib_max_gap_db = 0.0f;
static float   s_calib_ftm_samples[CALIB_MAX_SAMPLES];
static unsigned s_calib_ftm_cnt;
static float   s_calib_old_ftm_offset;

// 双端扫盘跟随与最近投票分歧(结果页/主页面置信度用)。
static uint32_t s_followed_sweep_id = 0;
static float    s_last_vote_spread = 0.0f;

// 追踪页平滑距离(INVALID 表示未初始化)。
static float s_track_dist_m = RADAR_DIST_INVALID;
static float s_last_mean_rssi = NAN;
static int64_t s_raised_until_ms;
static float s_prev_sweep_angle = RADAR_ANGLE_INVALID;
static int64_t s_prev_sweep_ms;
static unsigned s_walk_state;
static float s_walk_reference = NAN;
static int64_t s_walk_cap_until_ms;
static int64_t s_walk_armed_ms;
static bool s_diag_peer_recorded;
static volatile bool s_radio_ready;
static unsigned s_capture_target_index;
#define WALK_ARMED 1u
#define WALK_DONE 2u
#define WALK_CAP_M 25.0f

// ---------- 前置声明 ----------
static void start_sweep(void);

static const char *capture_phase_name(radar_capture_phase_t phase) {
    switch (phase) {
    case RADAR_CAPTURE_COUNTDOWN: return "COUNTDOWN";
    case RADAR_CAPTURE_SETTLE: return "SETTLE";
    case RADAR_CAPTURE_RUNNING: return "CAPTURE";
    case RADAR_CAPTURE_SUMMARY: return "SUMMARY";
    case RADAR_CAPTURE_INVALID: return "INVALID";
    default: return "READY";
    }
}

// ---------- 融合(供追踪页与扫盘结果共用) ----------

static radar_fusion_result_t run_fusion(void) {
    float local = radar_espnow_filtered_rssi();
    bool alive = radar_espnow_peer_alive();
    float peer = alive ? (float)radar_espnow_peer_rssi() : RADAR_RSSI_ABSENT;
    uint8_t peer_identity[RADAR_BLE_ID_LEN];
    // Keep a validated BLE identity across short ESP-NOW liveness gaps.
    // Clearing it here made BLE matching depend on a single 100 ms poll and
    // could discard the binding before the coded-PHY scan callback arrived.
    if (radar_espnow_peer_ble_identity(peer_identity)) {
        radar_ble_set_expected_identity(peer_identity);
    }

    radar_env_est_update(&s_env_est, local);
    static float raw_history[RADAR_RAW_RING];
    static uint32_t raw_timestamps[RADAR_RAW_RING];
    unsigned raw_count = radar_espnow_raw_history(raw_history, raw_timestamps,
                                                   RADAR_RAW_RING);
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    float p2p_1s = NAN, p2p_10s = NAN;
    radar_obstruct_p2p(raw_history, raw_timestamps, raw_count, now_ms,
                       &p2p_1s, &p2p_10s);

    radar_ftm_snapshot_t ftm_snapshot;
    radar_fusion_ftm_observation_t ftm = {0};
    if (radar_ftm_get_snapshot(&ftm_snapshot)) {
        ftm.valid = true;
        ftm.fresh = true;
        ftm.calibrated = ftm_snapshot.calibrated;
        ftm.distance_m = ftm_snapshot.distance_m;
        ftm.spread_m = ftm_snapshot.spread_m;
        ftm.valid_bursts = ftm_snapshot.valid_bursts;
    }
    float ftm_success_rate = radar_ftm_success_rate();
    uint8_t csi_peer_mac[RADAR_MAC_LEN];
    if (radar_espnow_peer_ap_mac(csi_peer_mac)) {
        radar_csi_set_peer_mac(csi_peer_mac);
    }
    float csi_selectivity = radar_csi_recent_selectivity_db();
    radar_level_features_t level_features = {
        .ftm_success_rate = ftm_success_rate,
        .ftm_iqr_m = ftm.valid ? ftm.spread_m : NAN,
        .rssi_p2p_db = isfinite(p2p_1s) ? p2p_1s : 0.0f,
        .sym_gap_db = radar_fusion_sym_valid(local, peer) ? fabsf(local - peer) : NAN,
        .csi_selectivity_db = csi_selectivity,
        .mean_rssi_dbm = s_env_est.mean_rssi,
        .peer_alive = alive,
    };
    radar_level_t level = radar_level_update(&s_level_est, &level_features,
                                             esp_timer_get_time() / 1000);
    radar_fusion_result_t result = radar_fusion_update_v3(
        &ftm, local, peer, s_tx_ref_sym, level, alive);

    radar_obstruct_features_t features = {
        .excess_loss_db = result.ftm_dist_m > 0.0f
            ? radar_obstruct_excess_loss_db(result.rssi_dist_m, result.ftm_dist_m,
                                            radar_env_path_loss_n(radar_fusion_env_of_level(level)))
            : NAN,
        .kfactor_db = radar_obstruct_kfactor_db(raw_history, raw_count),
        .p2p_1s_db = p2p_1s,
        .p2p_10s_db = p2p_10s,
        .csi_sel_db = level_features.csi_selectivity_db,
        .mirror_err_deg = NAN,
        .mean_rssi_dbm = radar_obstruct_mean_recent(raw_history, raw_timestamps,
                                                     raw_count, now_ms, 8000),
        .level = level,
    };
    s_last_mean_rssi = features.mean_rssi_dbm;
    radar_obstruct_class_t observed = radar_obstruct_classify(&features);
    s_obstruct_class = radar_obstruct_hold_update(&s_obstruct_hold, observed, 20);
    s_obstruct_hint = radar_obstruct_apply(
        s_obstruct_class, &features, raw_history, raw_count, s_tx_ref_sym,
        radar_fusion_env_of_level(level), &result, &s_last_vote_spread);
    if (radar_espnow_peer_wants_still() && !radar_espnow_is_sweeping()) {
        s_obstruct_hint = HINT_PEER_STAND_STILL;
    }
    if (s_walk_state == WALK_ARMED &&
        now_ms - (uint32_t)s_walk_armed_ms > 60000u) {
        s_walk_state = 0;
    }
    if (s_walk_cap_until_ms > 0 && now_ms < s_walk_cap_until_ms) {
        if (result.distance_m > WALK_CAP_M) result.distance_m = WALK_CAP_M;
        if (result.dist_hi_m > WALK_CAP_M) result.dist_hi_m = WALK_CAP_M;
    }
    return result;
}

// ---------- 定时器回调(LVGL 任务上下文,可直接操作 lv_obj) ----------

// 100ms 轮询:按页面更新显示(追踪融合 / 扫盘进度 / 标定采样)。
static void poll_tick(lv_timer_t *t) {
    (void)t;
    radar_page_t page = radar_ui_current_page();
    int64_t now_ms = esp_timer_get_time() / 1000;
    radar_fusion_set_breakpoint_m(now_ms < s_raised_until_ms
                                  ? RADAR_TWO_RAY_BREAK_RAISED_M
                                  : RADAR_TWO_RAY_BREAK_M);
    if (now_ms >= s_raised_until_ms && s_raised_until_ms != 0) {
        s_raised_until_ms = 0;
        s_track_dist_m = RADAR_DIST_INVALID;
    }

    // FTM 周期驱动(无线初始化失败时完全跳过)
    if (s_radio_ready) {
        radar_ftm_tick();
        radar_ble_diag_tick();
    }

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
        if (radar_espnow_peer_alive() && !s_diag_peer_recorded) {
            s_diag_peer_recorded = true;
            radar_diag_persist_stage(RADAR_DIAG_STAGE_PEER_SEEN, 2u,
                                     radar_espnow_peer_rssi(),
                                     radar_csi_received_frames());
        }
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
                                 s_last_vote_spread,
                                 radar_obstruct_class_label(s_obstruct_class),
                                 radar_hint_label(s_obstruct_hint));
    } else if (page == RADAR_PAGE_CAPTURE) {
        radar_ftm_snapshot_t snap = {0};
        bool ftm_valid = radar_ftm_get_snapshot(&snap);
        radar_capture_observation_t observation = {
            .local_rssi = (int8_t)radar_espnow_raw_rssi(),
            .peer_rssi = (int8_t)radar_espnow_peer_rssi(),
            .peer_alive = radar_espnow_peer_alive(),
            .ftm_valid = ftm_valid,
            .ftm_status = (int16_t)radar_ftm_last_status(),
            .ftm_distance_m = ftm_valid ? snap.distance_m : RADAR_DIST_INVALID,
            .ftm_spread_m = ftm_valid ? snap.spread_m : 0.0f,
            .ftm_session_count = (uint16_t)radar_ftm_session_count(),
            .ble_ready = radar_ble_ready(),
            .ble_peer_alive = radar_ble_peer_alive(),
            .ble_rssi = (int8_t)radar_ble_peer_last_rssi(),
            .ble_age_ms = radar_ble_peer_age_ms(),
            .csi_frames = radar_csi_received_frames(),
            .environment_level = (uint8_t)s_env_est.env,
        };
        radar_capture_tick(&observation);
        radar_ui_update_capture(capture_phase_name(radar_capture_phase()),
                                radar_capture_target_m(),
                                radar_capture_elapsed_ms(),
                                radar_capture_record_count(),
                                radar_capture_data_valid(),
                                radar_capture_save_succeeded());
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
            .ftm_hist_spread_m = 0.0f,
            .ftm_success_rate = radar_ftm_success_rate(),
            .ftm_bursts = 0,
            .ftm_hist_count = 0,
            .ftm_session_count = radar_ftm_session_count(),
            .ftm_session_busy = radar_ftm_session_busy(),
            .ftm_snapshot_age_ms = radar_ftm_snapshot_age_ms(),
            .ftm_calibrated = radar_ftm_is_calibrated(),
            .ftm_probe_attempts = radar_ftm_probe_attempts(),
            .ftm_probe_successes = radar_ftm_probe_successes(),
            .ble_ready = radar_ble_ready(),
            .ble_alive = radar_ble_peer_alive(),
            .ble_rssi = radar_ble_peer_rssi(),
            .ble_last_rssi = radar_ble_peer_last_rssi(),
            .ble_peer_age_ms = radar_ble_peer_age_ms(),
            .env = s_env_est.env,
            .env_mean_rssi = s_env_est.mean_rssi,
            .tx_ref_sym = s_tx_ref_sym,
            .vote_spread_deg = s_last_vote_spread,
            .capture_target_m = radar_capture_targets_m[s_capture_target_index],
        };
        radar_ftm_snapshot_t snap;
        if (radar_ftm_get_snapshot(&snap)) {
            di.ftm_valid = true;
            di.ftm_dist_m = snap.distance_m;
            di.ftm_spread_m = snap.spread_m;
            di.ftm_hist_spread_m = snap.hist_spread_m;
            di.ftm_hist_count = snap.hist_count;
            di.ftm_snapshot_age_ms = radar_ftm_snapshot_age_ms();
            di.ftm_calibrated = snap.calibrated;
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
        radar_ftm_snapshot_t calib_ftm;
        if (radar_ftm_get_snapshot(&calib_ftm) &&
            s_calib_ftm_cnt < CALIB_MAX_SAMPLES) {
            s_calib_ftm_samples[s_calib_ftm_cnt++] = calib_ftm.distance_m;
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

    int64_t sweep_elapsed_ms = (esp_timer_get_time() / 1000) - s_sweep_start_ms;
    // Keep timing and sample completeness observable without requiring a
    // Debug Capture implementation. This is a low-rate boundary log.
    ESP_LOGI(TAG, "SWEEP_DONE elapsed_ms=%lld", (long long)sweep_elapsed_ms);

    // 停止采样并取回数据(恢复固定信道),恢复 FTM 与 BLE 扫描
    unsigned count = radar_espnow_stop_sweep();
    radar_ftm_pause(false);
    radar_ble_pause(false);
    radar_sample_t samples[RADAR_MAX_SAMPLES];
    count = radar_espnow_get_samples(samples, RADAR_MAX_SAMPLES);
    ESP_LOGI(TAG, "SWEEP_SAMPLES count=%u", count);

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
    radar_espnow_set_homing(false);
    bool need_consensus = s_obstruct_class == OBS_FOLIAGE_DYNAMIC ||
                          s_obstruct_class == OBS_REFLECTION ||
                          fr.level == RADAR_LEVEL_L2_NLOS;
    if (need_consensus && angle >= 0.0f) {
        bool recent = s_prev_sweep_angle >= 0.0f &&
                      (esp_timer_get_time() / 1000 - s_prev_sweep_ms) < 60000;
        if (recent && radar_fusion_angle_diff(angle, s_prev_sweep_angle) <= 30.0f) {
            float pair[2] = { angle, s_prev_sweep_angle };
            angle = radar_fusion_circular_mean(pair, 2);
            if (spread < 30.0f) spread = 30.0f;
        } else {
            spread = 90.0f;
            s_obstruct_hint = HINT_SWEEP_AGAIN;
        }
        s_prev_sweep_angle = angle;
        s_prev_sweep_ms = esp_timer_get_time() / 1000;
    }

    radar_ui_show_result(angle >= 0.0f ? (int)(angle + 0.5f) : -1,
                         fr.distance_m, conf, spread, weak);
    radar_ui_set_result_hint(radar_hint_label(s_obstruct_hint));

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

    if (s_calib_ftm_cnt >= 3) {
        float offset = radar_fusion_ftm_offset_from_calib(s_calib_ftm_samples,
                                                          s_calib_ftm_cnt);
        if (isfinite(offset) && radar_espnow_ftm_offset_save(offset) == ESP_OK) {
            radar_ftm_set_offset_m(offset);
            radar_ftm_set_calibrated(true);
            ESP_LOGI(TAG, "FTM 标定偏移=%.2f m (%u samples)",
                     (double)offset, s_calib_ftm_cnt);
        } else if (isfinite(offset)) {
            ESP_LOGW(TAG, "FTM 标定值保存失败,保持未标定状态");
        }
    } else {
        radar_ftm_set_offset_m(s_calib_old_ftm_offset);
    }

    radar_espnow_ftm_set_calibrating(false);
    if (s_calib_timer) {
        lv_timer_delete(s_calib_timer);
        s_calib_timer = NULL;
    }
    radar_ui_show_main();
}

// ---------- 扫盘/标定启动 ----------

static void start_sweep(void) {
    ESP_LOGI(TAG, "SWEEP_START");
    radar_ftm_pause(true);   // 跳频期间暂停 FTM
    radar_ble_pause(true);   // 让出射频:跳频 + 扫描无法同时进行
    s_sweep_start_ms = esp_timer_get_time() / 1000;
    radar_espnow_set_homing(true);
    radar_espnow_start_sweep();
    radar_ui_show_sweeping();

    s_sweep_timer = lv_timer_create(sweep_done, RADAR_SWEEP_DURATION_MS, NULL);
    lv_timer_set_repeat_count(s_sweep_timer, 1);
}

static void start_calib(void) {
    radar_ftm_pause(true);   // 标定期间稳定信道,暂停 FTM 会话
    radar_espnow_ftm_set_calibrating(true);
    radar_ble_pause(true);   // 标定采样不受扫描干扰
    memset(s_calib_samples, 0, sizeof(s_calib_samples));
    s_calib_cnt = 0;
    s_calib_ftm_cnt = 0;
    s_calib_max_gap_db = 0.0f;
    s_calib_old_ftm_offset = radar_ftm_offset_m();
    radar_ftm_set_offset_m(0.0f);
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
        if (btn == BSP_BTN_UP && ev == BSP_BTN_CLICK) {
            s_raised_until_ms = esp_timer_get_time() / 1000 + 5000;
            s_track_dist_m = RADAR_DIST_INVALID;
        } else if (btn == BSP_BTN_DOWN && ev == BSP_BTN_CLICK &&
                   (s_obstruct_hint == HINT_WALK_TEST || s_walk_state != 0)) {
            int64_t now = esp_timer_get_time() / 1000;
            if (s_walk_state == 0) {
                s_walk_reference = s_last_mean_rssi;
                s_walk_state = WALK_ARMED;
                s_walk_armed_ms = now;
                s_obstruct_hint = HINT_WALK_TEST;
            } else if (s_walk_state == WALK_ARMED) {
                float delta = s_last_mean_rssi - s_walk_reference;
                if (delta > 3.0f) {
                    s_walk_cap_until_ms = now + 60000;
                }
                s_walk_state = WALK_DONE;
            }
        } else if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
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
            radar_espnow_set_homing(false);
            radar_ftm_pause(false);
            if (s_sweep_timer) {
                lv_timer_delete(s_sweep_timer);
                s_sweep_timer = NULL;
            }
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
        radar_espnow_ftm_set_calibrating(false);
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
        // Debug 页:UP/DOWN 选择目标距离,OK 启动固定窗口 Capture。
        if (btn == BSP_BTN_UP && ev == BSP_BTN_CLICK) {
            s_capture_target_index =
                (s_capture_target_index + 1u) % RADAR_CAPTURE_TARGET_COUNT;
        } else if (btn == BSP_BTN_DOWN && ev == BSP_BTN_CLICK) {
            s_capture_target_index =
                (s_capture_target_index + RADAR_CAPTURE_TARGET_COUNT - 1u) %
                RADAR_CAPTURE_TARGET_COUNT;
        } else if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
            if (radar_capture_start(radar_capture_targets_m[s_capture_target_index]) == ESP_OK) {
                radar_ui_show_capture(radar_capture_targets_m[s_capture_target_index]);
            }
        } else if (btn == BSP_BTN_DOWN && ev == BSP_BTN_LONG) {
            // Re-export the last valid Flash capture; never erase it here.
            (void)radar_capture_export_saved();
        } else if (btn == BSP_BTN_UP && ev == BSP_BTN_DOUBLE) {
            radar_ui_show_main();
        }
        break;

    case RADAR_PAGE_CAPTURE:
        if (btn == BSP_BTN_OK && (ev == BSP_BTN_CLICK || ev == BSP_BTN_LONG)) {
            if (radar_capture_phase() == RADAR_CAPTURE_SUMMARY ||
                radar_capture_phase() == RADAR_CAPTURE_INVALID) {
                radar_ui_show_debug();
            } else {
                radar_capture_abort();
                radar_ui_show_debug();
            }
        }
        break;
    }

    bsp_lvgl_unlock();
}

// ---------- 无线软依赖任务 ----------

static void ble_init_task(void *arg) {
    (void)arg;
    // BLE is optional. Keep it out of the Wi-Fi/radio setup task so a missing
    // controller (for example in a board simulator) cannot hold up the rest
    // of the application.
    esp_err_t err = radar_ble_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "BLE sentinel unavailable; continuing without BLE");
    }
    vTaskDelete(NULL);
}

static void radio_init_task(void *arg) {
    (void)arg;
    radar_diag_persist_stage(RADAR_DIAG_STAGE_RADIO_INIT, 0, -127, 0);
    esp_err_t espnow_err = radar_espnow_init();
    if (espnow_err != ESP_OK) {
        s_radio_ready = false;
        ESP_LOGW(TAG, "ESP-NOW unavailable (%s); UI remains available without peer data",
                 esp_err_to_name(espnow_err));
        radar_diag_persist_stage(RADAR_DIAG_STAGE_RUNNING, 0, -127, 0);
        vTaskDelete(NULL);
        return;
    }

    s_radio_ready = true;
    radar_diag_persist_stage(RADAR_DIAG_STAGE_RADIO_READY, 0, -127, 0);
    float calib;
    if (radar_espnow_calib_load(&calib) == ESP_OK &&
        isfinite(calib) && calib >= -60.0f && calib <= -30.0f) {
        s_tx_ref_sym = calib;
    } else {
        s_tx_ref_sym = RADAR_TX_POWER_REF;
    }

    if (radar_ftm_init() != ESP_OK) {
        ESP_LOGW(TAG, "FTM unavailable; using RSSI fusion");
    }
    float ftm_offset;
    if (radar_espnow_ftm_offset_load(&ftm_offset) == ESP_OK && isfinite(ftm_offset)) {
        radar_ftm_set_offset_m(ftm_offset);
        radar_ftm_set_calibrated(true);
    }

    bool csi_ready = radar_csi_init();
    if (csi_ready) radar_csi_start_window(esp_timer_get_time());
    radar_diag_persist_stage(csi_ready ? RADAR_DIAG_STAGE_CSI_READY
                                       : RADAR_DIAG_STAGE_RUNNING,
                             csi_ready ? 1u : 0u, -127,
                             radar_csi_received_frames());

    BaseType_t ble_task_ok = xTaskCreate(ble_init_task, "radar_ble",
                                         6144, NULL, 1, NULL);
    if (ble_task_ok != pdPASS) {
        ESP_LOGW(TAG, "BLE task could not start; continuing without BLE");
    }
    if (radar_espnow_start_broadcast("pass-radar") != ESP_OK) {
        ESP_LOGW(TAG, "ESP-NOW broadcast unavailable; continuing without peer data");
    }
    radar_diag_persist_stage(RADAR_DIAG_STAGE_RUNNING, 0, -127,
                             radar_csi_received_frames());
    vTaskDelete(NULL);
}

// ---------- 入口 ----------

void app_main(void) {
    (void)radar_diag_persist_init();
    radar_diag_persist_stage(RADAR_DIAG_STAGE_BOOT, 0, -127, 0);
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
    radar_diag_persist_stage(RADAR_DIAG_STAGE_DISPLAY_READY, 0, -127, 0);

    // 按键为软依赖:失败只影响交互,不阻塞启动。
    if (bsp_button_init(on_key, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败,设备将无法操作");
    }

    // 环境估计器与标定值
    radar_env_est_init(&s_env_est);
    radar_level_init(&s_level_est, esp_timer_get_time() / 1000);
    radar_obstruct_hold_init(&s_obstruct_hold);
    radar_capture_init();

    // 先构建 UI:即使射频链路不可用,用户也能看到雷达界面(显示无信号)。
    if (bsp_lvgl_lock(1000)) {
        radar_ui_init();

        // 100ms 轮询定时器:融合 + 追踪显示 + 扫盘/标定进度
        s_poll_timer = lv_timer_create(poll_tick, 100, NULL);
        s_sweep_timer = NULL;
        s_calib_timer = NULL;

        bsp_lvgl_unlock();
    }

    // Radio is a soft dependency. Start it after the display and input path;
    // unsupported simulator peripherals must never block the UI task.
    // Lowest application priority: a missing/blocked radio driver must never
    // starve LVGL, the button task, or the display refresh path.
    BaseType_t radio_task_ok = xTaskCreate(radio_init_task, "radar_radio",
                                           8192, NULL, 1, NULL);
    if (radio_task_ok != pdPASS) {
        ESP_LOGW(TAG, "radio task could not start; continuing with UI only");
        radar_diag_persist_stage(RADAR_DIAG_STAGE_RUNNING, 0, -127, 0);
    }
    // meta-pass 子固件适配:签名固件常驻,未签名试运行(重启回启动器)。
    esp_err_t mv = metapass_mark_valid();
    if (mv != ESP_OK) {
        ESP_LOGI(TAG, "试运行模式(下次重启回启动器): %s", esp_err_to_name(mv));
    }

    ESP_LOGI(TAG, "就绪");
}
