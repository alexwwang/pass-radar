#pragma once
#ifndef RADAR_UPLOAD_H
#define RADAR_UPLOAD_H

#include <stdbool.h>
#include "esp_err.h"

// main/radar_upload.h —— 独立安装模式的上行迷你服务(方案 A 独立通道)。
//
// 复用已在跑的 Wi-Fi SoftAP(APSTA)挂迷你 HTTP 服务:手机连热点
// "passradar-up"(开放) → 开 http://192.168.4.1 → 云端模块拉批采集
// 原始字节(RCB1)→ 手机代传 metapass.chuanxilu.net /api/capture/ingest。
// 交互刻意与 meta-pass 手机管理同构(设备 boot 页 + CF 托管模块)。
//
// meta-pass 子固件模式下不走本服务:长按任意键回启动器,用启动器自带的
// 手机管理拉数据(同一 ingest 契约)。
//
// 生命周期:radar_upload_enter() 会保存并改写 AP 配置(可见/开放/无 FTM),
// radar_upload_exit() 停 httpd 并恢复 FTM AP 配置。进入前请确保不在
// 扫盘/标定/测距会话中(调用侧在页面层保证)。

esp_err_t radar_upload_enter(void);
esp_err_t radar_upload_exit(void);
bool radar_upload_active(void);

// AP 网卡 IP 文本(进入成功后可用,供 UI 展示),如 "192.168.4.1"。
const char *radar_upload_ap_ip(void);

#endif
