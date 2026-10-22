#pragma once

// 雷达 UI 模块 —— 极坐标雷达显示 + 扫盘动画 + 置信度呈现。
// 所有 LVGL 操作必须在 LVGL 任务(lv_timer)或持有 bsp_lvgl_lock 的上下文中执行。
//
// v2:主页面显示融合距离与置信度标记(FTM/双向/单链路),
//     结果页显示相对起始方向、顺时针角度分歧(+/-),新增 1m 标定页。

#include "bsp_button.h"
#include "radar_fusion.h"
#include "radar_capture_batch.h"
#include <stdbool.h>
#include <stdint.h>

// 页面状态
typedef enum {
    RADAR_PAGE_MAIN = 0,   // 实时追踪:融合距离 + 置信度 + 链路状态
    RADAR_PAGE_SWEEPING,   // 扫盘中:4s 倒计时 + 动画
    RADAR_PAGE_RESULT,     // 结果:相对方位 + 顺时针角度(+/-分歧)
    RADAR_PAGE_CALIB,      // 1m 标定中
    RADAR_PAGE_DEBUG,      // 诊断/回归:v1/v2 管线 A/B 对比
    RADAR_PAGE_CAPTURE_MENU, // Debug二级Capture菜单
    RADAR_PAGE_CAPTURE_EDITOR, // Capture上下文候选值编辑
    RADAR_PAGE_CAPTURE,    // 静态距离自动采集
    RADAR_PAGE_UPLOAD,     // 独立模式上行服务:AP 热点 + 迷你 HTTP(手机拉批次)
} radar_page_t;

// 诊断页数据快照(由 main.c 每 100ms 填充)。
typedef struct {
    int   raw_rssi;
    float filtered_rssi;
    float v1_dist_m;        // v1 纯 RSSI 路径损耗距离(A/B 基线)
    const radar_fusion_result_t *fr;   // v2 融合输出(可为 NULL)
    bool  ftm_valid;
    float ftm_dist_m;
    float ftm_spread_m;
    float ftm_hist_spread_m;
    float ftm_success_rate;
    unsigned ftm_bursts;
    unsigned ftm_hist_count;
    unsigned ftm_session_count;
    bool ftm_session_busy;
    int ftm_snapshot_age_ms;
    bool ftm_calibrated;
    unsigned ftm_probe_attempts;
    unsigned ftm_probe_successes;
    int   ftm_last_status;  // 无有效快照时的失败原因(status 或 -esp_err_t)
    bool  ble_ready;
    bool  ble_alive;
    int   ble_rssi;
    int   ble_last_rssi;
    int   ble_peer_age_ms;
    radar_env_t env;
    float env_mean_rssi;    // 环境估计长期均值
    float tx_ref_sym;       // 当前 1m 对称参考(标定值)
    float vote_spread_deg;  // 最近一次扫盘投票/融合分歧
    float capture_target_m; // 当前 Debug Capture 目标标签
    const char *capture_scene_name; // 当前场景标签
    uint32_t mem_free_bytes; // INTERNAL|8BIT 当前空闲堆
    uint32_t mem_largest_bytes; // INTERNAL|8BIT 当前最大连续块
    uint32_t mem_min_free_bytes; // 本次启动以来最低空闲堆
    const char *capture_export_status; // DOWN 长按重复导出反馈（空串=无）
    const char *capture_batch_status; // 批次剩余空间状态
} radar_debug_info_t;

// 切换到诊断页并刷新(由 main.c 调用;入口=追踪页 UP 长按)。
void radar_ui_show_debug(void);
void radar_ui_show_upload(void);
void radar_ui_update_debug(const radar_debug_info_t *info);
void radar_ui_show_capture_menu(void);
void radar_ui_show_capture_editor(radar_capture_context_field_t field);
void radar_ui_update_capture_editor(radar_capture_context_field_t field,
                                    radar_capture_environment_id_t environment,
                                    radar_capture_link_condition_id_t link,
                                    radar_capture_room_geometry_t space,
                                    uint8_t selected_value);
void radar_ui_update_capture_menu(const char *environment_name,
                                  const char *link_name,
                                  const char *context_name,
                                  const char *room_size_name,
                                  const char *obstacle_name,
                                  const char *material_name,
                                  const char *furniture_name,
                                  const char *boundary_name,
                                  const char *card_height_name,
                                  const char *card_pose_name,
                                  const char *bearing_name,
                                  float target_distance_m, unsigned batch_count,
                                  const char *batch_status,
                                  unsigned selected_item, bool confirm_erase,
                                  const char *action_status);
void radar_ui_show_capture(float target_distance_m);
void radar_ui_update_capture(const char *phase, float target_distance_m,
                            uint32_t elapsed_ms, unsigned record_count,
                            bool valid, bool saved);

// 初始化雷达 UI(创建主页面并载入)。
void radar_ui_init(void);

// 更新主页面(每 100ms 由 lv_timer 调用)。
// fr:融合引擎输出;raw_rssi:原始信号;ble_alive:BLE 哨兵链路可达;
// vote_spread_deg:最近一次扫盘的信道投票/双端融合分歧(无数据传 0)。
// vote_spread_deg > 20 时 HIGH 封顶为 MED(方向不可信档)。
void radar_ui_update_tracking(const radar_fusion_result_t *fr, int raw_rssi,
                              bool ble_alive, float vote_spread_deg,
                              const char *class_label, const char *hint_label);

// 切换到扫盘页面(4s 倒计时动画)。
void radar_ui_show_sweeping(void);

// 更新扫盘倒计时进度(0~1)。
void radar_ui_update_sweep_progress(float progress);

// 切换到结果页面。
// angle_deg < 0 或 weak_signal 时显示 "OUT OF RANGE"。
// spread_deg:信道投票最大分歧(ASCII +/- 角精度估计);conf:融合置信度。
void radar_ui_show_result(int angle_deg, float distance_m,
                          radar_confidence_t conf, float spread_deg,
                          bool weak_signal);
void radar_ui_set_result_hint(const char *hint_label);

// 标定页面(3s 采样进度)。
void radar_ui_show_calib(void);
void radar_ui_update_calib(float progress);

// 返回主页面。
void radar_ui_show_main(void);

// 按键处理(由 main.c 的 on_key 在持有锁时调用)。
void radar_ui_key(bsp_btn_t btn, bsp_btn_ev_t ev);

// 当前页面状态查询。
radar_page_t radar_ui_current_page(void);

// 销毁 UI 资源。
void radar_ui_deinit(void);
