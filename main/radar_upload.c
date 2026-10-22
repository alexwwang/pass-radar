// main/radar_upload.c —— 独立安装模式的上行迷你服务。见头文件。
//
// Wi-Fi 优先(meta-pass 凭据复用,2026-10-22):enter 先读 NVS "metapass"
// 命名空间的 sta_ssid/sta_pass(设备经历过 meta-pass 配网即存在;刷
// standalone 固件不清 NVS),尝试 STA 连家庭路由器,≤10s 拿到 LAN IP 则
// httpd 直接伺服 —— 手机与设备同一局域网,全程 Wi-Fi,零蜂窝流量。
// 无凭据/连接失败退回开放热点 passradar-up,并在设备屏幕明示上传将经
// 手机数据网络(知情,不偷跑)。退出:STA 模式断开连接、频道回 1;
// 热点模式恢复 FTM AP 配置。
#include "radar_upload.h"

#include "radar_capture_batch.h"

#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define TAG "radar_upload"

#define UPLOAD_AP_SSID   "passradar-up"
#define UPLOAD_AP_CHANNEL 1
// radar_espnow.c 的 home channel 同为 1(radar_ftm.c FTM_AP_CHANNEL=1);
// 该宏是 radar_espnow.c 私有,此处以字面量保持同值。
#define HOME_CHANNEL     1
#define READ_CHUNK_MAX   4096
#define STA_TIMEOUT_MS   10000

// meta-pass 配网凭据的存放契约(meta_store_net.c):默认 NVS 分区,
// namespace "metapass",key sta_ssid / sta_pass。
#define CRED_NS   "metapass"
#define CRED_SSID "sta_ssid"
#define CRED_PASS "sta_pass"

// 设备 boot 页:与 meta-pass SHELL_HTML 同构 —— 正文极小,模块从 CF 托管
// 域名加载(capture-upload.js),升级只部署云端,固件不用重刷。%s = 设备
// 实际 IP(STA 模式为局域网 IP,热点模式为 192.168.4.1)。
static const char SHELL_HTML_FMT[] =
    "<!doctype html><html lang=zh><head><meta charset=utf-8>"
    "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>pass-radar upload</title></head><body>"
    "<h3>pass-radar upload</h3>"
    "<div id=st>device %s</div>"
    "<div id=cu-root>加载上传模块…(需可访问 metapass.chuanxilu.net)</div>"
    "<noscript><p>需要启用 JavaScript。</p></noscript>"
    "<script type=module src=\"https://metapass.chuanxilu.net/capture-upload.js\"></script>"
    "</body></html>";

static httpd_handle_t s_httpd;
static esp_netif_t *s_ap_netif;
static esp_netif_t *s_sta_netif;
static bool s_active;
static bool s_sta_mode;          // true = 家庭 Wi-Fi(STA),false = 开放热点
static wifi_config_t s_saved_ap;
static char s_ip[16] = "192.168.4.1";
static char s_ssid[33];          // STA 模式连上的路由器名(UI 展示)
static uint32_t s_bytes_served;

static SemaphoreHandle_t s_sta_sem;
static bool s_events_registered;

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data) {
    (void)arg; (void)base; (void)data;
    if (id == WIFI_EVENT_STA_START) {
        (void)esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_DISCONNECTED && s_sta_sem) {
        xSemaphoreGive(s_sta_sem);   // 等 IP 的一方据此判失败
    }
}

static void ip_event_handler(void *arg, esp_event_base_t base,
                             int32_t id, void *data) {
    (void)arg; (void)base; (void)data;
    if (id == IP_EVENT_STA_GOT_IP && s_sta_sem) {
        xSemaphoreGive(s_sta_sem);
    }
}

// 读 meta-pass 配网凭据并尝试 STA 连接;10s 内拿到 IP 返回 ESP_OK。
// 拿不到(无凭据/连不上)返回错误,调用方退回热点模式。
static esp_err_t try_sta_join(void) {
    nvs_handle_t nvs;
    if (nvs_open(CRED_NS, NVS_READONLY, &nvs) != ESP_OK) return ESP_ERR_NOT_FOUND;
    char ssid[33] = {0}, pass[65] = {0};
    size_t len = sizeof(ssid);
    esp_err_t err = nvs_get_str(nvs, CRED_SSID, ssid, &len);
    if (err == ESP_OK && len > 1) {
        len = sizeof(pass);
        err = nvs_get_str(nvs, CRED_PASS, pass, &len);
    }
    nvs_close(nvs);
    if (err != ESP_OK || ssid[0] == '\0') return ESP_ERR_NOT_FOUND;

    if (!s_sta_netif) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
        if (!s_sta_netif) return ESP_FAIL;
    }
    if (!s_events_registered) {
        if (esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                &wifi_event_handler, NULL, NULL) != ESP_OK ||
            esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                &ip_event_handler, NULL, NULL) != ESP_OK) {
            return ESP_FAIL;
        }
        s_events_registered = true;
    }
    if (!s_sta_sem) {
        s_sta_sem = xSemaphoreCreateBinary();
        if (!s_sta_sem) return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_sta_sem, 0);   // 清残留

    wifi_config_t sta;
    memset(&sta, 0, sizeof(sta));
    memcpy(sta.sta.ssid, ssid, sizeof(sta.sta.ssid) - 1);
    memcpy(sta.sta.password, pass, sizeof(sta.sta.password) - 1);
    sta.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    err = esp_wifi_set_config(WIFI_IF_STA, &sta);
    if (err != ESP_OK) return err;
    err = esp_wifi_connect();   // 未启动 STA 时返回错;APSTA 模式下 STA 已随 start 起来
    if (err != ESP_OK) return err;

    if (xSemaphoreTake(s_sta_sem, pdMS_TO_TICKS(STA_TIMEOUT_MS)) != pdTRUE) {
        (void)esp_wifi_disconnect();
        return ESP_ERR_TIMEOUT;
    }
    // 信号可能是 disconnected 给的:以是否拿到 IP 为准
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(s_sta_netif, &ip) != ESP_OK || ip.ip.addr == 0) {
        (void)esp_wifi_disconnect();
        return ESP_ERR_TIMEOUT;
    }
    snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ip.ip));
    snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
    ESP_LOGI(TAG, "sta joined ssid=%s ip=%s", s_ssid, s_ip);
    return ESP_OK;
}

static esp_err_t h_root(httpd_req_t *req) {
    char html[sizeof(SHELL_HTML_FMT) + 16];
    snprintf(html, sizeof(html), SHELL_HTML_FMT, s_ip);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

// GET /api/capture/info —— 批数据区概览(头部字段 + 容量 + 设备哈希)。
// 业务结果一律 200 + JSON(与 meta-pass 安装服务同纪律:非 200 会把真实
// 原因吞成不可用)。
static esp_err_t h_info(httpd_req_t *req) {
    radar_capture_batch_header_t hdr;
    const esp_err_t rd = radar_capture_batch_data_read(0, &hdr, sizeof(hdr));
    const bool magic_ok = rd == ESP_OK && hdr.magic == RADAR_CAPTURE_BATCH_MAGIC;
    char device[33];
    for (int i = 0; i < 16; i++) snprintf(device + i * 2, 3, "%02x", hdr.device_hash[i]);
    device[32] = '\0';
    char json[288];
    snprintf(json, sizeof(json),
             "{\"size\":%u,\"magic_ok\":%s,\"version\":%u,"
             "\"batch\":%lu,\"expected\":%lu,\"committed\":%lu,"
             "\"device\":\"%s\",\"served\":%lu}",
             (unsigned)radar_capture_batch_data_size(),
             magic_ok ? "true" : "false",
             magic_ok ? (unsigned)hdr.version : 0u,
             magic_ok ? (unsigned long)hdr.batch_id : 0ul,
             magic_ok ? (unsigned long)hdr.expected_captures : 0ul,
             magic_ok ? (unsigned long)hdr.committed_captures : 0ul,
             device, (unsigned long)s_bytes_served);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

// GET /api/capture/read?offset=N&len=M —— 原始字节块,相对批数据区起点,
// 越界 400(与 meta-pass data/read 同形:设备解析,调用方不给绝对地址)。
static esp_err_t h_read(httpd_req_t *req) {
    char qs[64];
    if (httpd_req_get_url_query_len(req) >= sizeof(qs) ||
        httpd_req_get_url_query_str(req, qs, sizeof(qs)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad query");
    }
    char val[16];
    long offset, len;
    if (httpd_query_key_value(qs, "offset", val, sizeof(val)) != ESP_OK ||
        (offset = strtol(val, NULL, 10)) < 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad offset");
    }
    if (httpd_query_key_value(qs, "len", val, sizeof(val)) != ESP_OK ||
        (len = strtol(val, NULL, 10)) <= 0 || len > READ_CHUNK_MAX) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad len");
    }
    uint8_t buf[READ_CHUNK_MAX];   // 栈上:server stack_size 已配 8192
    const esp_err_t err =
        radar_capture_batch_data_read((size_t)offset, buf, (size_t)len);
    if (err == ESP_ERR_INVALID_SIZE) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "out of range");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "read failed");
    }
    s_bytes_served += (uint32_t)len;
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)buf, (ssize_t)len);
}

const char *radar_upload_ap_ip(void) { return s_ip; }

bool radar_upload_is_sta(void) { return s_active && s_sta_mode; }

const char *radar_upload_sta_ssid(void) { return s_ssid; }

static esp_err_t start_httpd(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;                 // read handler 栈上 4KB 块缓冲
    cfg.max_uri_handlers = 8;
    cfg.lru_purge_enable = true;
    esp_err_t err = httpd_start(&s_httpd, &cfg);
    if (err != ESP_OK) return err;
    const httpd_uri_t uris[] = {
        { "/", HTTP_GET, h_root, NULL },
        { "/api/capture/info", HTTP_GET, h_info, NULL },
        { "/api/capture/read", HTTP_GET, h_read, NULL },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        err = httpd_register_uri_handler(s_httpd, &uris[i]);
        if (err != ESP_OK) {
            (void)httpd_stop(s_httpd);
            s_httpd = NULL;
            return err;
        }
    }
    return ESP_OK;
}

esp_err_t radar_upload_enter(void) {
    if (s_active) return ESP_OK;
    s_sta_mode = false;
    s_ssid[0] = '\0';

    // 1) Wi-Fi 优先:复用 meta-pass 配网凭据上家庭路由器。
    if (try_sta_join() == ESP_OK) {
        s_sta_mode = true;
    } else {
        // 2) 退回开放热点(无凭据/连不上)。屏幕明示将走手机数据网络。
        if (esp_wifi_get_config(WIFI_IF_AP, &s_saved_ap) != ESP_OK) {
            return ESP_ERR_INVALID_STATE;   // Wi-Fi 未初始化(enter 时机过早)
        }
        if (!s_ap_netif) {
            s_ap_netif = esp_netif_create_default_wifi_ap();
            if (!s_ap_netif) return ESP_FAIL;
        }
        wifi_config_t up;
        memset(&up, 0, sizeof(up));
        memcpy(up.ap.ssid, UPLOAD_AP_SSID, sizeof(UPLOAD_AP_SSID) - 1);
        up.ap.ssid_len = sizeof(UPLOAD_AP_SSID) - 1;
        up.ap.channel = UPLOAD_AP_CHANNEL;
        up.ap.authmode = WIFI_AUTH_OPEN;       // 开放:免输密码,热点即配对
        up.ap.max_connection = 4;
        up.ap.ftm_responder = false;           // 上传期间不做 FTM responder
        esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &up);
        if (err != ESP_OK) return err;
        snprintf(s_ip, sizeof(s_ip), "192.168.4.1");
    }

    const esp_err_t err = start_httpd();
    if (err != ESP_OK) {
        if (!s_sta_mode) (void)esp_wifi_set_config(WIFI_IF_AP, &s_saved_ap);
        return err;
    }
    s_bytes_served = 0;
    s_active = true;
    ESP_LOGI(TAG, "upload service up mode=%s ip=%s", s_sta_mode ? "sta" : "ap", s_ip);
    return ESP_OK;
}

esp_err_t radar_upload_exit(void) {
    if (!s_active) return ESP_OK;
    s_active = false;
    if (s_httpd) {
        (void)httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    if (s_sta_mode) {
        (void)esp_wifi_disconnect();
        (void)esp_wifi_set_channel(HOME_CHANNEL, WIFI_SECOND_CHAN_NONE);
        ESP_LOGI(TAG, "sta left, channel restored to %d", HOME_CHANNEL);
    } else {
        (void)esp_wifi_set_config(WIFI_IF_AP, &s_saved_ap);   // 恢复 FTM AP 配置
    }
    ESP_LOGI(TAG, "upload service down (served %lu bytes)",
             (unsigned long)s_bytes_served);
    return ESP_OK;
}

bool radar_upload_active(void) { return s_active; }
