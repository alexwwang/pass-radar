#pragma once

// 雷达 UI 模块 —— 极坐标雷达显示 + 扫盘动画。
// 所有 LVGL 操作必须在 LVGL 任务(lv_timer)或持有 bsp_lvgl_lock 的上下文中执行。

#include "bsp_button.h"
#include <stdbool.h>

// 页面状态
typedef enum {
    RADAR_PAGE_MAIN = 0,   // 实时追踪:显示距离 + 信号强度
    RADAR_PAGE_SWEEPING,   // 扫盘中:4s 倒计时 + 动画
    RADAR_PAGE_RESULT,     // 结果:显示检测角度 + 距离
} radar_page_t;

// 初始化雷达 UI(创建主页面并载入)。
void radar_ui_init(void);

// 更新主页面上的 RSSI 与距离显示(每 100ms 由 lv_timer 调用)。
void radar_ui_update_tracking(float filtered_rssi, int raw_rssi, float distance_m);

// 切换到扫盘页面(4s 倒计时动画)。
void radar_ui_show_sweeping(void);

// 更新扫盘倒计时进度(0~1)。
void radar_ui_update_sweep_progress(float progress);

// 切换到结果页面,显示检测到的角度与距离。
// angle < 0 或信号微弱时显示 "OUT OF RANGE"。
void radar_ui_show_result(int angle_deg, float distance_m, bool weak_signal);

// 返回主页面。
void radar_ui_show_main(void);

// 按键处理(由 main.c 的 on_key 在持有锁时调用)。
void radar_ui_key(bsp_btn_t btn, bsp_btn_ev_t ev);

// 当前页面状态查询。
radar_page_t radar_ui_current_page(void);

// 销毁 UI 资源。
void radar_ui_deinit(void);
