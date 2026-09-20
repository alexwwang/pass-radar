// radar_ui.c —— 极坐标雷达 UI:追踪 / 扫盘 / 结果 / 标定 四个子页面。
// 使用 ui_pixel 基元 + lv_obj 定位块构建 240×320 雷达画面。
#include "radar_ui.h"
#include "ui_pixel.h"
#include "radar_sweep.h"
#include "radar_ftm.h"
#include "radar_ble.h"
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
static lv_obj_t *s_result_meta;
static lv_obj_t *s_result_sector;

// 标定页面对象
static lv_obj_t *s_calib_bar;

// 诊断页面对象
static lv_obj_t *s_debug_label;
static lv_obj_t *s_capture_label;

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

static void format_ble_status(char *out, size_t out_size,
                              const radar_debug_info_t *info) {
    const char *stack = info->ble_ready ? "OK" : "NO";
    const char *peer = info->ble_alive ? "YES" : "NO";
    if (info->ble_last_rssi <= 20 && info->ble_last_rssi >= -127 &&
        info->ble_peer_age_ms >= 0) {
        (void)snprintf(out, out_size, "BLE S:%s P:%s r%d age%ds",
                       stack, peer, info->ble_last_rssi,
                       info->ble_peer_age_ms / 1000);
    } else {
        (void)snprintf(out, out_size, "BLE S:%s P:%s r-- age--",
                       stack, peer);
    }
}

// 扫盘角度是相对于按下 OK 时顶边方向的顺时针角度，不是地理方位。
static const char *relative_direction_label(int angle_deg) {
    static const char *const labels[] = {
        "FRONT", "FRONT-RIGHT", "RIGHT", "REAR-RIGHT",
        "REAR", "REAR-LEFT", "LEFT", "FRONT-LEFT",
    };
    int normalized = angle_deg % 360;
    if (normalized < 0) normalized += 360;
    unsigned sector = (unsigned)((normalized + 22) / 45) % 8;
    return labels[sector];
}

// ---------- 页面构建/销毁 ----------

static void build_main_page(void) {
    s_scr = ui_pixel_screen_create("STATUS");

    // 待机页没有实时方向观测，不显示静态雷达图，避免暗示方向有效。
    // 三行状态固定放在屏幕中部，避免底部文字被裁切。
    s_dist_label = add_info_label(s_scr, 92, &lv_font_montserrat_20);
    s_rssi_label = add_info_label(s_scr, 136, &lv_font_montserrat_14);
    s_hint_label = add_info_label(s_scr, 178, &lv_font_montserrat_14);
    lv_label_set_text(s_dist_label, "WAITING FOR PEER");
    lv_label_set_text(s_rssi_label, "RSSI:--  1way [--]");
    lv_label_set_text(s_hint_label, "[OK] Sweep  [LONG] Calib");

    lv_screen_load(s_scr);
}

static void build_sweeping_page(void) {
    s_scr = ui_pixel_screen_create("SWEEPING");
    build_radar_panel();

    s_sweep_msg = add_info_label(s_scr, 232, &lv_font_montserrat_14);
    lv_label_set_text(s_sweep_msg, "0=TOP EDGE AT OK\nROTATE CW");

    // 进度条(屏幕底部)
    s_progress_bar = ui_pixel_panel_create(s_scr, 30, 280, 0, 8, RADAR_SWEEP);

    // 扫盘动画点(面板子对象,与其他页面坐标一致)
    s_sweep_dot = create_dot(s_radar_panel, RADAR_SWEEP, RADAR_SWEEP_DOT);

    lv_screen_load(s_scr);
}

static void build_result_page(void) {
    s_scr = ui_pixel_screen_create("RESULT");

    // 保留扫盘后的雷达盘面，再在盘面下方叠加文字结果。
    // 页面切换会销毁 SWEEPING 页面，因此结果页必须重新创建自己的面板。
    build_radar_panel();
    s_result_dot = create_dot(s_radar_panel, RADAR_TARGET, RADAR_DOT_SIZE);
    s_result_angle = add_info_label(s_scr, 228, &lv_font_montserrat_14);
    s_result_dist = add_info_label(s_scr, 248, &lv_font_montserrat_14);
    s_result_meta = add_info_label(s_scr, 268, &lv_font_montserrat_14);
    s_hint_label = add_info_label(s_scr, 292, &lv_font_montserrat_14);
    lv_label_set_text(s_hint_label, "0=TOP EDGE AT OK");

    lv_screen_load(s_scr);
}

static void build_calib_page(void) {
    s_scr = ui_pixel_screen_create("CALIBRATE");

    s_sweep_msg = add_info_label(s_scr, 108, &lv_font_montserrat_14);
    lv_label_set_text(s_sweep_msg, "Hold two cards 1m apart\nKeep still...");

    s_calib_bar = ui_pixel_panel_create(s_scr, 30, 280, 0, 8, RADAR_SWEEP);

    lv_screen_load(s_scr);
}

static void build_capture_page(void) {
    s_scr = ui_pixel_screen_create("CAPTURE");
    s_capture_label = add_info_label(s_scr, 92, &lv_font_montserrat_14);
    lv_obj_set_width(s_capture_label, 230);
    lv_label_set_text(s_capture_label, "TARGET --");
    lv_screen_load(s_scr);
}

static void build_debug_page(void) {
    s_scr = ui_pixel_screen_create("DEBUG");

    // 单块多行文本，诊断页也不显示没有实时方向意义的静态雷达图。
    s_debug_label = add_info_label(s_scr, 78, &lv_font_montserrat_14);
    lv_obj_set_width(s_debug_label, 230);
    lv_label_set_text(s_debug_label, "...");

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
        s_result_meta = NULL;
        s_result_sector = NULL;
        s_calib_bar = NULL;
        s_debug_label = NULL;
        s_capture_label = NULL;
    }
}

// ---------- 公共接口 ----------

void radar_ui_init(void) {
    s_page = RADAR_PAGE_MAIN;
    build_main_page();
}

void radar_ui_update_tracking(const radar_fusion_result_t *fr, int raw_rssi,
                              bool ble_alive, float vote_spread_deg,
                              const char *class_label, const char *hint_label) {
    if (s_page != RADAR_PAGE_MAIN || !s_scr || !fr) return;

    // 主页面方向可信度门控:最近投票分歧 > 20° 时 HIGH 封顶为 MED
    // (调研 5.6 HIGH 条件含"信道投票一致")。
    radar_confidence_t conf = fr->conf;
    if (conf == RADAR_CONF_HIGH && vote_spread_deg > 20.0f) {
        conf = RADAR_CONF_MED;
    }

    // 距离行:超界时显示链路状态;非 HIGH 置信度加 ~ 前缀
    if (s_dist_label) {
        if (fr->conf == RADAR_CONF_OOR) {
            // BLE 哨兵远/近两档(调研 5.5):RSSI > -75dBm 判近档。
            // 待机页第一行只保留短状态，确保 240px 屏幕不截断。
            lv_label_set_text(s_dist_label,
                              ble_alive ? (radar_ble_peer_rssi() > -75
                                               ? "FAR NEAR BLE"
                                               : "FAR BLE")
                                        : "OUT OF RANGE");
        } else {
            int dm = (int)(fr->distance_m * 10.0f + 0.5f);
            char distance_text[96];
            if (fr->upper_bound_only && fr->dist_hi_m > 0.0f) {
                (void)snprintf(distance_text, sizeof(distance_text),
                               "DIST <= %.0fm %s L%d",
                               (double)fr->dist_hi_m, conf_tag(conf),
                               (int)fr->level);
            } else if (fr->dist_lo_m > 0.0f && fr->dist_hi_m > 0.0f) {
                (void)snprintf(distance_text, sizeof(distance_text),
                               "DIST %s%d.%dm %s L%d",
                               conf == RADAR_CONF_HIGH ? "" : "~",
                               dm / 10, dm % 10, conf_tag(conf),
                               (int)fr->level);
            } else {
                (void)snprintf(distance_text, sizeof(distance_text),
                               "DIST %s%d.%dm %s L%d",
                               conf == RADAR_CONF_HIGH ? "" : "~",
                               dm / 10, dm % 10, conf_tag(conf),
                               (int)fr->level);
            }
            lv_label_set_text(s_dist_label, distance_text);
        }
    }

    if (s_hint_label) {
        // 第三行优先显示动作提示；无动作时显示遮挡分类，最后才显示默认操作。
        const char *status_hint = (hint_label && hint_label[0] != '\0')
            ? hint_label
            : ((class_label && class_label[0] != '\0')
                ? class_label
                : "[OK] SWEEP  [LONG] CALIB");
        lv_label_set_text(s_hint_label, status_hint);
    }

    // 信号行:原始 RSSI + 观测源/置信度标记。
    // 先在本地缓冲区完成格式化，避免 LVGL 内置 varargs formatter 在
    // 无效/已失效字符串参数下解引用崩溃。
    if (s_rssi_label) {
        char rssi_text[64];
        (void)snprintf(rssi_text, sizeof(rssi_text), "RSSI:%d %s%s [%s]",
                       raw_rssi,
                       fr->ftm_used ? "FTM " : "",
                       fr->conf == RADAR_CONF_MED ||
                       fr->conf == RADAR_CONF_HIGH ? "2way" : "1way",
                       conf_tag(conf));
        lv_label_set_text(s_rssi_label, rssi_text);
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
        if (s_result_angle) lv_label_set_text(s_result_angle, "DIR: UNKNOWN");
        if (s_result_dist)  lv_label_set_text(s_result_dist,
                                              weak_signal ? "SIGNAL TOO WEAK"
                                                          : "NO CLEAR DIRECTION");
        if (s_result_meta) lv_label_set_text(s_result_meta, "DIST --  CONF --");
        return;
    }

    // 结果页三行:相对方位、顺时针角度、0°基准。
    if (s_result_angle) {
        char direction_text[48];
        (void)snprintf(direction_text, sizeof(direction_text), "DIR: %s",
                       relative_direction_label(angle_deg));
        lv_label_set_text(s_result_angle, direction_text);
        lv_obj_set_style_text_color(s_result_angle,
            lv_color_hex(conf == RADAR_CONF_LOW ? RADAR_WARN : RADAR_TEXT), 0);
    }
    if (s_result_dist) {
        char angle_text[48];
        (void)snprintf(angle_text, sizeof(angle_text), "CW %d+/-%.0f deg",
                       angle_deg, (double)spread_deg);
        lv_label_set_text(s_result_dist, angle_text);
    }
    if (s_result_meta) {
        char meta_text[48];
        (void)snprintf(meta_text, sizeof(meta_text), "DIST %.1fm  CONF %s",
                       (double)distance_m, conf_tag(conf));
        lv_label_set_text(s_result_meta, meta_text);
    }
    if (s_result_dot) {
        int x, y;
        polar_to_xy(angle_deg, RADAR_RADIUS - 10, &x, &y);
        lv_obj_set_pos(s_result_dot, x - RADAR_DOT_SIZE / 2,
                       y - RADAR_DOT_SIZE / 2);
        lv_obj_remove_flag(s_result_dot, LV_OBJ_FLAG_HIDDEN);
    }
}

void radar_ui_set_result_hint(const char *hint_label) {
    if (s_page == RADAR_PAGE_RESULT && s_hint_label) {
        // 保留结果页的 0° 基准说明；异常场景提示追加在同一行。
        if (hint_label && hint_label[0] != '\0') {
            char hint_text[64];
            (void)snprintf(hint_text, sizeof(hint_text), "%s / 0=TOP EDGE",
                           hint_label);
            lv_label_set_text(s_hint_label, hint_text);
        } else {
            lv_label_set_text(s_hint_label, "0=TOP EDGE AT OK");
        }
    }
}

void radar_ui_show_calib(void) {
    page_teardown();
    s_page = RADAR_PAGE_CALIB;
    build_calib_page();
}

void radar_ui_show_debug(void) {
    page_teardown();
    s_page = RADAR_PAGE_DEBUG;
    build_debug_page();
}

void radar_ui_show_capture(float target_distance_m) {
    page_teardown();
    s_page = RADAR_PAGE_CAPTURE;
    build_capture_page();
    radar_ui_update_capture("READY", target_distance_m, 0, 0, false, false);
}

void radar_ui_update_capture(const char *phase, float target_distance_m,
                             uint32_t elapsed_ms, unsigned record_count,
                             bool valid, bool saved) {
    if (s_page != RADAR_PAGE_CAPTURE || !s_capture_label) return;
    char text[160];
    unsigned seconds = elapsed_ms / 1000u;
    const char *state = saved ? "SAVED CSV" : (valid ? "VALID" : "WAITING");
    (void)snprintf(text, sizeof(text),
                   "TARGET %.1fm\n%s\nTIME %us\nRECORDS %u\n%s\nOK: RETURN",
                   (double)target_distance_m, phase ? phase : "--", seconds,
                   record_count, state);
    lv_label_set_text(s_capture_label, text);
}

void radar_ui_update_debug(const radar_debug_info_t *info) {
    if (s_page != RADAR_PAGE_DEBUG || !s_scr || !s_debug_label || !info) return;

    // 行1:v1 基线(纯 RSSI 路径损耗)vs v2 融合(A/B 回归,调研 5.9)
    // 行2:FTM 质量(失败时带原因码)/ BLE 哨兵
    // 行3:环境与标定参数 / 最近投票分歧
    char ftm[40];
    if (info->ftm_valid) {
        snprintf(ftm, sizeof(ftm), "FTM ok %.1f +/-%.1f q%u %s r%.0f%%",
                 (double)info->ftm_dist_m, (double)info->ftm_spread_m,
                 info->ftm_hist_count,
                 info->ftm_calibrated ? "CAL" : "RAW",
                 (double)(info->ftm_success_rate * 100.0f));
    } else if (info->ftm_last_status != 0) {
        snprintf(ftm, sizeof(ftm), "FTM fail s%d n%u age%ds r%.0f%%",
                 info->ftm_last_status, info->ftm_session_count,
                 info->ftm_snapshot_age_ms >= 0 ? info->ftm_snapshot_age_ms / 1000 : -1,
                 (double)(info->ftm_success_rate * 100.0f));
    } else {
        // No valid snapshot is a transient state while a session is pending,
        // not a permanent UI mode. Show it explicitly as WAIT and let the
        // next poll replace it with FTM ok or FTM fail automatically.
        snprintf(ftm, sizeof(ftm), "FTM WAIT n%u %s age%ds r%s",
                 info->ftm_session_count,
                 info->ftm_session_busy ? "BUSY" : "IDLE",
                 info->ftm_snapshot_age_ms >= 0 ? info->ftm_snapshot_age_ms / 1000 : -1,
                 isfinite(info->ftm_success_rate)
                     ? (info->ftm_success_rate >= 0.995f ? "100" : "<100")
                     : "--");
    }
    {
        char debug_text[288];
        char ble_status[48];
        char export_line[24];
        format_ble_status(ble_status, sizeof(ble_status), info);
        // 导出反馈行:DOWN 长按后 5 秒内显示结果,平时显示操作提示。
        if (info->capture_export_status && info->capture_export_status[0]) {
            (void)snprintf(export_line, sizeof(export_line), "%s",
                           info->capture_export_status);
        } else {
            (void)snprintf(export_line, sizeof(export_line), "DOWN LONG: EXPORT");
        }
        if (info->fr && info->fr->conf != RADAR_CONF_OOR &&
            info->fr->distance_m > 0.0f) {
            int v2dm = (int)(info->fr->distance_m * 10.0f + 0.5f);
            (void)snprintf(debug_text, sizeof(debug_text),
                "v1 %d.%d m | v2 %s%d.%d m [%s]\n"
                "%s\n"
                "%s\n"
                "env %d n%.1f tx %.0f sp%.0f cap%.1fm\n"
                "%s",
                (int)info->v1_dist_m, (int)(info->v1_dist_m * 10.0f) % 10,
                info->fr->conf == RADAR_CONF_HIGH ? "" : "~",
                v2dm / 10, v2dm % 10, conf_tag(info->fr->conf),
                ftm,
                ble_status,
                (int)info->env,
                (double)radar_env_path_loss_n(info->env),
                (double)info->tx_ref_sym, (double)info->vote_spread_deg,
                (double)info->capture_target_m,
                export_line);
        } else {
            (void)snprintf(debug_text, sizeof(debug_text),
                "v1 %d.%d m | v2 OUT OF RANGE\n"
                "%s\n"
                "%s\n"
                "env %d n%.1f tx %.0f sp%.0f cap%.1fm\n"
                "%s",
                (int)info->v1_dist_m, (int)(info->v1_dist_m * 10.0f) % 10,
                ftm,
                ble_status,
                (int)info->env,
                (double)radar_env_path_loss_n(info->env),
                (double)info->tx_ref_sym, (double)info->vote_spread_deg,
                (double)info->capture_target_m,
                export_line);
        }
        lv_label_set_text(s_debug_label, debug_text);
    }
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
