#pragma once

// 雷达 UI 模块 —— 极坐标雷达显示 + 扫盘动画 + 置信度呈现。
// 所有 LVGL 操作必须在 LVGL 任务(lv_timer)或持有 bsp_lvgl_lock 的上下文中执行。
//
// v2:主页面显示融合距离与置信度标记(FTM/双向/单链路),
//     结果页显示角度分歧(±)、距离置信度,新增 1m 标定页。

#include "bsp_button.h"
#include "radar_fusion.h"
#include <stdbool.h>

// 页面状态
typedef enum {
    RADAR_PAGE_MAIN = 0,   // 实时追踪:融合距离 + 置信度 + 链路状态
    RADAR_PAGE_SWEEPING,   // 扫盘中:4s 倒计时 + 动画
    RADAR_PAGE_RESULT,     // 结果:角度(±分歧) + 距离 + 置信度
    RADAR_PAGE_CALIB,      // 1m 标定中
} radar_page_t;

// 初始化雷达 UI(创建主页面并载入)。
void radar_ui_init(void);

// 更新主页面(每 100ms 由 lv_timer 调用)。
// fr:融合引擎输出;raw_rssi:原始信号;ble_alive:BLE 哨兵链路可达。
void radar_ui_update_tracking(const radar_fusion_result_t *fr, int raw_rssi,
                              bool ble_alive);

// 切换到扫盘页面(4s 倒计时动画)。
void radar_ui_show_sweeping(void);

// 更新扫盘倒计时进度(0~1)。
void radar_ui_update_sweep_progress(float progress);

// 切换到结果页面。
// angle_deg < 0 或 weak_signal 时显示 "OUT OF RANGE"。
// spread_deg:信道投票最大分歧(角精度估计);conf:融合置信度。
void radar_ui_show_result(int angle_deg, float distance_m,
                          radar_confidence_t conf, float spread_deg,
                          bool weak_signal);

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
