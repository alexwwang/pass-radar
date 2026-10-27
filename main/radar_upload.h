#pragma once
#ifndef RADAR_UPLOAD_H
#define RADAR_UPLOAD_H

#include <stdbool.h>
#include "esp_err.h"

// main/radar_upload.h —— 独立安装模式的上行迷你服务(方案 A 独立通道)。
//
// Wi-Fi 优先,三种模式(radar_upload_mode()):
//   STA —— 复用 meta-pass 配网凭据(NVS "metapass" sta_ssid/sta_pass,刷
//           standalone 固件不清 NVS)连家庭路由器,手机同局域网,拉取+
//           上传全程 Wi-Fi,零蜂窝流量。
//   PROV —— 无凭据。开放热点 "passradar-up" + 配网门户(captive portal
//           自动弹出或 http://192.168.4.1/):扫网 + 表单 → 设备存凭据并
//           原地转 STA。知情退路:门户页链接直达 /up(将经手机数据网络)。
//   AP —— 有凭据但连不上(密码改了等)。热点兜底,屏幕明示走数据网络,
//         门户仍在 http://192.168.4.1/ 供重配。
// 交互刻意与 meta-pass 手机管理同构(设备 boot 页 + CF 托管模块
// capture-upload.js);上传数据 POST 同一 ingest 契约。
//
// meta-pass 子固件模式下不走本服务:长按任意键回启动器,用启动器自带的
// 手机管理拉数据(同一 ingest 契约)。
//
// 生命周期:enter 改 AP 配置(开放/可见/无 FTM),exit 停 httpd + 停 DNS、
// STA 断开(频道还原 1)、恢复 FTM AP 配置。进入前请确保不在扫盘/标定/
// 测距会话中(调用侧在页面层保证)。

typedef enum {
    RADAR_UPLOAD_NONE = 0,   // 未进入
    RADAR_UPLOAD_STA,        // 家庭 Wi-Fi,全程 Wi-Fi 上传
    RADAR_UPLOAD_PROV,       // 热点 + 配网门户(无凭据)
    RADAR_UPLOAD_AP,         // 热点兜底(有凭据但连不上,明示走数据网络)
} radar_upload_mode_t;

esp_err_t radar_upload_enter(void);
esp_err_t radar_upload_exit(void);
bool radar_upload_active(void);

// 设备 IP 文本(STA = 局域网 IP,热点 = "192.168.4.1"),进入成功后可用。
const char *radar_upload_ap_ip(void);

// 当前模式(UI 按模式出文案,见 radar_ui_update_upload)。
radar_upload_mode_t radar_upload_mode(void);

// 有凭据但 STA 连不上(AP 模式),UI 据此提示重配。
bool radar_upload_join_failed(void);

// STA 模式连上的路由器 SSID(UI 展示);未连为空串。
const char *radar_upload_sta_ssid(void);

// ---- 分裂启动(上传专用启动) ----
// Wi-Fi 扫描/关联被 BT 共存调度压死(真机:贴脸路由扫不到、控制器运行时
// 无法安全下电)。方案:DOWN 双击 → 置 NVS 标志 + esp_restart → 下次启动
// 跳过 BLE 初始化(BT 控制器根本不上电,与 meta-pass 同条件)并自动进
// 上传模式;退出上传页再清标志 + restart 回完整固件。
void radar_upload_boot_request(void);      // 置标志并重启(调用不返回)
bool radar_upload_boot_requested(void);    // 启动早期检查
// UI-only boot (board simulators / UI screenshots): skip BLE + radio
// bring-up entirely. Shares the upboot NVS namespace; key "ui".
bool radar_ui_only_requested(void);
void radar_upload_boot_clear(void);        // 清标志(退出时;随后重启)

#endif
