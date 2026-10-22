// main/radar_upload.c —— 独立安装模式的上行迷你服务。见头文件。
#include "radar_upload.h"

#include "radar_capture_batch.h"

#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define TAG "radar_upload"

#define UPLOAD_AP_SSID   "passradar-up"
#define UPLOAD_AP_CHANNEL 1
#define READ_CHUNK_MAX   4096

// 设备 boot 页:与 meta-pass SHELL_HTML 同构 —— 正文极小,模块从 CF 托管
// 域名加载(capture-upload.js),升级只部署云端,固件不用重刷。
static const char SHELL_HTML[] =
    "<!doctype html><html lang=zh><head><meta charset=utf-8>"
    "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>pass-radar upload</title></head><body>"
    "<h3>pass-radar upload</h3>"
    "<div id=st>device 192.168.4.1</div>"
    "<div id=cu-root>加载上传模块…(需可访问 metapass.chuanxilu.net)</div>"
    "<noscript><p>需要启用 JavaScript。</p></noscript>"
    "<script type=module src=\"https://metapass.chuanxilu.net/capture-upload.js\"></script>"
    "</body></html>";

static httpd_handle_t s_httpd;
static esp_netif_t *s_ap_netif;
static bool s_active;
static wifi_config_t s_saved_ap;
static char s_ap_ip[16] = "192.168.4.1";
static uint32_t s_bytes_served;

static esp_err_t h_root(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, SHELL_HTML, HTTPD_RESP_USE_STRLEN);
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

const char *radar_upload_ap_ip(void) { return s_ap_ip; }

esp_err_t radar_upload_enter(void) {
    if (s_active) return ESP_OK;
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

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;                 // read handler 栈上 4KB 块缓冲
    cfg.max_uri_handlers = 8;
    cfg.lru_purge_enable = true;
    err = httpd_start(&s_httpd, &cfg);
    if (err != ESP_OK) {
        (void)esp_wifi_set_config(WIFI_IF_AP, &s_saved_ap);
        return err;
    }
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
            (void)esp_wifi_set_config(WIFI_IF_AP, &s_saved_ap);
            return err;
        }
    }

    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(s_ap_netif, &ip) == ESP_OK) {
        snprintf(s_ap_ip, sizeof(s_ap_ip), IPSTR, IP2STR(&ip.ip));
    }
    s_bytes_served = 0;
    s_active = true;
    ESP_LOGI(TAG, "upload service up ssid=%s ip=%s", UPLOAD_AP_SSID, s_ap_ip);
    return ESP_OK;
}

esp_err_t radar_upload_exit(void) {
    if (!s_active) return ESP_OK;
    s_active = false;
    if (s_httpd) {
        (void)httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    (void)esp_wifi_set_config(WIFI_IF_AP, &s_saved_ap);   // 恢复 FTM AP 配置
    ESP_LOGI(TAG, "upload service down (served %lu bytes)",
             (unsigned long)s_bytes_served);
    return ESP_OK;
}

bool radar_upload_active(void) { return s_active; }
