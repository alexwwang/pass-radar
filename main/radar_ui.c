// radar_ui.c —— 极坐标雷达 UI:追踪 / 扫盘 / 结果 / 标定 四个子页面。
// 使用 ui_pixel 基元 + lv_obj 定位块构建 240×320 雷达画面。
#include "radar_ui.h"
#include "ui_pixel.h"
#include "lvgl.h"
#include <math.h>
#include <stdio.h>

// ---------- 布局常量(240×320 屏幕) ----------
#define RADAR_CX        110   // 圆心 X(面板坐标,panel 220 宽)
#define RADAR_CY         80   // 圆心 Y(面板坐标,panel 170 高)
#define RADAR_RADIUS     75   // 外圈半径
#define RADAR_DOT_SIZE    8   // 目标点尺寸
#define RADAR_RING_W      2   // 环线宽度(用细条模拟)
#define RADAR_SWEEP_DOT  10   // 扫盘动画点尺寸

// 颜色(雷达主题,在 ui_pixel 色系基础上扩展)
#define RADAR_BG     0x0A1628   // 深蓝黑背景
#define RADAR_RING   0x1E90FF   // 亮蓝环
#define RADAR_SWEEP  0x00FF41   // 扫描绿
#define RADAR_TARGET 0xFF4500   // 目标橙红
#define RADAR_TEXT   0xE0F0FF   // 文字浅蓝白
#define RADAR_DIM    0x335580   // 暗色辅助
#define RADAR_WARN   0xFFD700   // 低置信度黄

// ---------- 状态 ----------
static radar_page_t s_page = RADAR_PAGE_MAIN;

// 主页面对象
static lv_obj_t *s_scr;
static lv_obj_t *s_radar_panel;
static lv_obj_t *s_target_dot;
static lv_obj_t *s_dist_label;
static lv_obj_t *s_rssi_label;
static lv_obj_t *s_hint_label;

// 扫盘页面对象
static lv_obj_t *s_sweep_dot;
static lv_obj_t *s_progress_bar;
static lv_obj_t *s_sweep_msg;

// 结果页面对象
static lv_obj_t *s_result_dot;
static lv_obj_t *s_result_angle;
static lv_obj_t *s_result_dist;

// 标定页面对象
static lv_obj_t *s_calib_bar;

// ---------- 坐标转换 ----------

// 极坐标 → 笛卡尔(角度 0°=正北,顺时针递增)
static void polar_to_xy(int angle_deg, int radius, int *x, int *y) {
    float rad = (float)(angle_deg - 90) * 3.14159265f / 180.0f;
    *x = RADAR_CX + (int)((float)radius * cosf(rad));
    *y = RADAR_CY + (int)((float)radius * sinf(rad));
}

// ---------- 绘制雷达圆环 ----------

// 在外圈面板内绘制同心环(单个圆形边框对象,避免大量 lv_obj 耗尽 LVGL 内存)。
static void draw_ring(lv_obj_t *parent, int ring_r, uint32_t color) {
    lv_obj_t *ring = lv_obj_create(parent);
    lv_obj_set_size(ring, ring_r * 2, ring_r * 2);
    lv_obj_set_pos(ring, RADAR_CX - ring_r, RADAR_CY - ring_r);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ring, RADAR_RING_W, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(color), 0);
    lv_obj_set_style_border_opa(ring, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
}

// ---------- 公共小部件 ----------

static lv_obj_t *add_info_label(lv_obj_t *parent, int y, const lv_font_t *font) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(RADAR_TEXT), 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, y);
    return lbl;
}

static lv_obj_t *create_dot(lv_obj_t *parent, uint32_t color, int size) {
    lv_obj_t *dot = lv_obj_create(parent);
    lv_obj_set_size(dot, size, size);
    lv_obj_set_style_bg_color(dot, lv_color_hex(color), 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
    return dot;
}

// 雷达面板 + 中心点(三个页面共用结构)
static void build_radar_panel(void) {
    s_radar_panel = ui_pixel_panel_create(s_scr, 10, 50, 220, 170, RADAR_BG);
    draw_ring(s_radar_panel, 25, RADAR_DIM);
    draw_ring(s_radar_panel, 50, RADAR_DIM);
    draw_ring(s_radar_panel, RADAR_RADIUS, RADAR_RING);

    lv_obj_t *center = lv_obj_create(s_radar_panel);
    lv_obj_set_size(center, 6, 6);
    lv_obj_set_pos(center, RADAR_CX - 3, RADAR_CY - 3);
    lv_obj_set_style_bg_color(center, lv_color_hex(RADAR_TEXT), 0);
    lv_obj_set_style_border_width(center, 0, 0);
    lv_obj_set_style_radius(center, LV_RADIUS_CIRCLE, 0);
}

// 置信度短标记
static const char *conf_tag(radar_confidence_t conf) {
    switch (conf) {
    case RADAR_CONF_HIGH: return "HI";
    case RADAR_CONF_MED:  return "MED";
    case RADAR_CONF_LOW:  return "LOW";
    default:              return "--";
    }
}

// ---------- 页面构建/销毁 ----------

static void build_main_page(void) {
    s_scr = ui_pixel_screen_create("RADAR");
    build_radar_panel();

    // 目标点(初始隐藏)
    s_target_dot = create_dot(s_radar_panel, RADAR_TARGET, RADAR_DOT_SIZE);

    // 距离与信号强度
    s_dist_label = add_info_label(s_scr, 232, &lv_font_montserrat_20);
    s_rssi_label = add_info_label(s_scr, 258, &lv_font_montserrat_14);
    s_hint_label = add_info_label(s_scr, 290, &lv_font_montserrat_14);
    lv_label_set_text(s_hint_label, "[OK] Sweep  [LONG] Calib");

    lv_screen_load(s_scr);
}

static void build_sweeping_page(void) {
    s_scr = ui_pixel_screen_create("SWEEPING");
    build_radar_panel();

    s_sweep_msg = add_info_label(s_scr, 232, &lv_font_montserrat_14);
    lv_label_set_text(s_sweep_msg, "Rotate clockwise!\nHold card at chest level");

    // 进度条(屏幕底部)
    s_progress_bar = ui_pixel_panel_create(s_scr, 30, 280, 0, 8, RADAR_SWEEP);

    // 扫盘动画点(面板子对象,与其他页面坐标一致)
    s_sweep_dot = create_dot(s_radar_panel, RADAR_SWEEP, RADAR_SWEEP_DOT);

    lv_screen_load(s_scr);
}

static void build_result_page(void) {
    s_scr = ui_pixel_screen_create("RESULT");
    build_radar_panel();

    // 结果点
    s_result_dot = create_dot(s_radar_panel, RADAR_TARGET, RADAR_DOT_SIZE);

    // 结果文字
    s_result_angle = add_info_label(s_scr, 232, &lv_font_montserrat_20);
    s_result_dist = add_info_label(s_scr, 258, &lv_font_montserrat_14);
    s_hint_label = add_info_label(s_scr, 290, &lv_font_montserrat_14);
    lv_label_set_text(s_hint_label, "[OK] Again  [LONG] Back");

    lv_screen_load(s_scr);
}

static void build_calib_page(void) {
    s_scr = ui_pixel_screen_create("CALIBRATE");
    build_radar_panel();

    s_sweep_msg = add_info_label(s_scr, 232, &lv_font_montserrat_14);
    lv_label_set_text(s_sweep_msg, "Hold two cards 1m apart\nKeep still...");

    s_calib_bar = ui_pixel_panel_create(s_scr, 30, 280, 0, 8, RADAR_SWEEP);

    lv_screen_load(s_scr);
}

static void page_teardown(void) {
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
        s_radar_panel = NULL;
        s_target_dot = NULL;
        s_sweep_dot = NULL;
        s_result_dot = NULL;
        s_dist_label = NULL;
        s_rssi_label = NULL;
        s_hint_label = NULL;
        s_progress_bar = NULL;
        s_sweep_msg = NULL;
        s_result_angle = NULL;
        s_result_dist = NULL;
        s_calib_bar = NULL;
    }
}

// ---------- 公共接口 ----------

void radar_ui_init(void) {
    s_page = RADAR_PAGE_MAIN;
    build_main_page();
}

void radar_ui_update_tracking(const radar_fusion_result_t *fr, int raw_rssi,
                              bool ble_alive) {
    if (s_page != RADAR_PAGE_MAIN || !s_scr || !fr) return;

    // 距离行:超界时显示链路状态;非 HIGH 置信度加 ~ 前缀
    if (s_dist_label) {
        if (fr->conf == RADAR_CONF_OOR) {
            lv_label_set_text(s_dist_label,
                              ble_alive ? "FAR (BLE link)" : "OUT OF RANGE");
        } else {
            int dm = (int)(fr->distance_m * 10.0f + 0.5f);
            lv_label_set_text_fmt(s_dist_label, "DIST: %s%d.%d m",
                                  fr->conf == RADAR_CONF_HIGH ? "" : "~",
                                  dm / 10, dm % 10);
        }
    }

    // 信号行:原始 RSSI + 观测源/置信度标记
    if (s_rssi_label) {
        lv_label_set_text_fmt(s_rssi_label, "RSSI:%d %s%s [%s]",
                              raw_rssi,
                              fr->ftm_used ? "FTM " : "",
                              fr->conf == RADAR_CONF_MED ||
                              fr->conf == RADAR_CONF_HIGH ? "2way" : "1way",
                              conf_tag(fr->conf));
    }

    // 目标点:距离映射到半径(0~75px),显示在 12 点钟方向(追踪态无角度)
    if (s_target_dot) {
        if (fr->conf != RADAR_CONF_OOR && fr->distance_m > 0.0f) {
            int r = (int)(fr->distance_m * 8.0f);
            if (r > RADAR_RADIUS) r = RADAR_RADIUS;
            if (r < 5) r = 5;
            int x, y;
            polar_to_xy(0, r, &x, &y);
            lv_obj_set_pos(s_target_dot,
                           x - RADAR_DOT_SIZE / 2,
                           y - RADAR_DOT_SIZE / 2);
            lv_obj_remove_flag(s_target_dot, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_target_dot, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void radar_ui_show_sweeping(void) {
    page_teardown();
    s_page = RADAR_PAGE_SWEEPING;
    build_sweeping_page();
}

void radar_ui_update_sweep_progress(float progress) {
    if (s_page != RADAR_PAGE_SWEEPING || !s_scr) return;

    // 进度条宽度
    if (s_progress_bar) {
        int w = (int)(180.0f * progress);
        lv_obj_set_size(s_progress_bar, w > 0 ? w : 1, 8);
    }

    // 扫盘动画点:沿大圆旋转
    if (s_sweep_dot) {
        int angle = (int)(360.0f * progress);
        int x, y;
        polar_to_xy(angle, RADAR_RADIUS - 10, &x, &y);
        lv_obj_set_pos(s_sweep_dot,
                       x - RADAR_SWEEP_DOT / 2,
                       y - RADAR_SWEEP_DOT / 2);
        lv_obj_remove_flag(s_sweep_dot, LV_OBJ_FLAG_HIDDEN);
    }
}

void radar_ui_show_result(int angle_deg, float distance_m,
                          radar_confidence_t conf, float spread_deg,
                          bool weak_signal) {
    page_teardown();
    s_page = RADAR_PAGE_RESULT;
    build_result_page();

    if (weak_signal || angle_deg < 0 || conf == RADAR_CONF_OOR) {
        if (s_result_angle) lv_label_set_text(s_result_angle, "OUT OF RANGE");
        if (s_result_dist)  lv_label_set_text(s_result_dist,
                                              weak_signal ? "Signal too weak"
                                                          : "No clear direction");
        return;
    }

    // 角度行:附信道投票分歧(±)与置信度颜色
    if (s_result_angle) {
        lv_label_set_text_fmt(s_result_angle, "Angle: %d+/-%d deg",
                              angle_deg, (int)(spread_deg + 0.5f));
        lv_obj_set_style_text_color(s_result_angle,
            lv_color_hex(conf == RADAR_CONF_LOW ? RADAR_WARN : RADAR_TEXT), 0);
    }
    if (s_result_dist) {
        int dm = (int)(distance_m * 10.0f + 0.5f);
        lv_label_set_text_fmt(s_result_dist, "Dist: %s%d.%d m [%s]",
                              conf == RADAR_CONF_HIGH ? "" : "~",
                              dm / 10, dm % 10, conf_tag(conf));
    }

    // 在雷达上画出目标方向(低置信度用警示色)
    if (s_result_dot) {
        int r = (int)(distance_m * 8.0f);
        if (r > RADAR_RADIUS) r = RADAR_RADIUS;
        if (r < 5) r = 5;
        int x, y;
        polar_to_xy(angle_deg, r, &x, &y);
        lv_obj_set_pos(s_result_dot,
                       x - RADAR_DOT_SIZE / 2,
                       y - RADAR_DOT_SIZE / 2);
        lv_obj_set_style_bg_color(s_result_dot,
            lv_color_hex(conf == RADAR_CONF_LOW ? RADAR_WARN : RADAR_TARGET), 0);
        lv_obj_remove_flag(s_result_dot, LV_OBJ_FLAG_HIDDEN);
    }
}

void radar_ui_show_calib(void) {
    page_teardown();
    s_page = RADAR_PAGE_CALIB;
    build_calib_page();
}

void radar_ui_update_calib(float progress) {
    if (s_page != RADAR_PAGE_CALIB || !s_scr) return;
    if (s_calib_bar) {
        int w = (int)(180.0f * progress);
        lv_obj_set_size(s_calib_bar, w > 0 ? w : 1, 8);
    }
}

void radar_ui_show_main(void) {
    page_teardown();
    s_page = RADAR_PAGE_MAIN;
    build_main_page();
}

void radar_ui_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    (void)btn; (void)ev;
    // 页面切换逻辑由 main.c 的 on_key 统一处理
}

radar_page_t radar_ui_current_page(void) { return s_page; }

void radar_ui_deinit(void) {
    page_teardown();
}
