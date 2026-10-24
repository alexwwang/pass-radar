// main/radar_upload.c —— 独立安装模式的上行迷你服务。见头文件。
//
// Wi-Fi 优先(meta-pass 凭据复用,2026-10-22):enter 先读 NVS "metapass"
// 命名空间的 sta_ssid/sta_pass(设备经历过 meta-pass 配网即存在;刷
// standalone 固件不清 NVS),尝试 STA 连家庭路由器,≤10s 拿到 LAN IP 则
// httpd 直接伺服 —— 手机与设备同一局域网,全程 Wi-Fi,零蜂窝流量。
//
// 三种模式:
//   STA —— 凭据在且连上。手机同局域网,拉取+上传全程 Wi-Fi。
//   PROV —— 无凭据。开放热点 + 配网门户(DNS 劫持 captive portal、扫网、
//           POST /api/wifi);配网成功原地转 STA(httpd 绑 0.0.0.0,手机
//           回路由器后开 http://<LAN IP> 继续)。知情退路:门户页有链接
//           直达 /up 上传页(将经手机数据网络)。
//   AP —— 有凭据但连不上(密码改了等)。热点兜底,屏幕明示走数据网络,
//         门户仍在 http://192.168.4.1/ 供重配凭据。
// 退出:STA 断开+频道回 1;热点模式恢复 FTM AP 配置;DNS 任务停。
#include "radar_upload.h"

#include "radar_capture_batch.h"
#include "radar_identity.h"

#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_random.h"   // scan 列表顺序打散,避免按信号排序泄露环境

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <ctype.h>

#define TAG "radar_upload"

#define UPLOAD_AP_SSID   "passradar-up"
#define UPLOAD_AP_CHANNEL 1
// radar_espnow.c 的 home channel 同为 1(radar_ftm.c FTM_AP_CHANNEL=1);
// 该宏是 radar_espnow.c 私有,此处以字面量保持同值。
#define HOME_CHANNEL     1
#define READ_CHUNK_MAX   4096
#define STA_TIMEOUT_MS   10000

// meta-pass 配网凭据的存放契约(meta_store_net.c):默认 NVS 分区,
// namespace "metapass",key sta_ssid / sta_pass。本服务读同一处,
// 配网门户写同一处 —— 两个固件互通。
#define CRED_NS   "metapass"
#define CRED_SSID "sta_ssid"
#define CRED_PASS "sta_pass"

// 设备 boot 页(上传):与 meta-pass SHELL_HTML 同构 —— 正文极小,模块从 CF
// 托管域名加载(capture-upload.js),升级只部署云端,固件不用重刷。%s = 设备
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

// 配网门户页(照抄 meta-pass PROV_HTML 契约):captive portal 自动弹出或手动
// 开 http://192.168.4.1/。「Scan networks」点选 + 手输兜底;提交后设备切 STA,
// 页面提示看手机屏幕并回路由器等待。动态文本一律 textContent 注入(防 SSID
// 注入)。底部常驻 /up 链接 = 知情走数据网络的退路。
static const char PROV_HTML[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>pass-radar WiFi setup</title></head>"
    "<body style='font-family:sans-serif;max-width:24em;margin:2em auto'>"
    "<h3>pass-radar WiFi setup</h3>"
    "<button onclick='scan()'>Scan networks</button> <span id=hint></span><br><br>"
    "<div id=list></div>"
    "<form onsubmit='event.preventDefault();save()'>"
    "WiFi name (SSID):<br><input id=s required maxlength=32><br><br>"
    "Password:<br><input id=p type=password maxlength=64><br><br>"
    "<button>Connect</button> <b id=st></b></form>"
    "<hr><p><a href='/up'>No Wi-Fi? Upload via mobile data instead (opens upload page on this hotspot).</a></p>"
    "<script>"
    "function scan(){hint.textContent='scanning...';list.innerHTML='';"
    "fetch('/api/scan').then(function(r){return r.json();}).then(function(j){"
    "if(!j.networks||!j.networks.length){"
    "hint.textContent='no networks yet - tap scan again, or type SSID below.';return;}"
    "hint.textContent='pick your network:';"
    "j.networks.forEach(function(n){var b=document.createElement('button');"
    "b.textContent=n.ssid+'  ('+n.rssi+'dBm, '+(n.auth?'secured':'open')+')';"
    "b.style.display='block';b.style.margin='4px 0';"
    "b.onclick=function(){s.value=n.ssid;p.focus();};"
    "list.appendChild(b);});"
    "}).catch(function(){hint.textContent='scan failed; type SSID manually.';});}"
    "function save(){st.textContent='saving...';"
    "fetch('/api/wifi?ssid='+encodeURIComponent(s.value)"
    "+'&pass='+encodeURIComponent(p.value),{method:'POST'})"
    ".then(function(r){return r.text().then(function(t){return [r.ok,t];});})"
    ".then(function(ok){st.textContent=ok[0]"
    "?'Received. Device is connecting - WATCH THE DEVICE SCREEN for progress or the failure reason. Then reconnect your phone to the Wi-Fi and reopen the page address shown on the device.'"
    ":'failed ('+ok[1]+')';});}"
    "</script></body></html>";

static httpd_handle_t s_httpd;
static esp_netif_t *s_ap_netif;
static esp_netif_t *s_sta_netif;
static bool s_active;
static radar_upload_mode_t s_mode = RADAR_UPLOAD_NONE;
static bool s_ap_dirty;          // AP 配置被本服务改过(退出时需恢复 FTM 配置)
static bool s_sta_joined;        // 本服务建立过 STA 连接(退出时需断开+还原频道)
static bool s_join_failed;       // 有凭据但连不上(AP 模式,UI 展示原因口径)
static wifi_config_t s_saved_ap;
static char s_ip[16] = "192.168.4.1";
static char s_ssid[33];          // STA 模式连上的路由器名(UI 展示)
static uint32_t s_bytes_served;

static SemaphoreHandle_t s_sta_sem;
static SemaphoreHandle_t s_prov_sem;
static TaskHandle_t s_prov_task;
static bool s_events_registered;
static volatile bool s_join_in_progress;   // 守卫:补投的 STA_START 不再重复 connect

// ---- 凭据读写(meta-pass 契约,两固件互通) ----

static bool creds_load(char ssid[33], char pass[65]) {
    nvs_handle_t nvs;
    if (nvs_open(CRED_NS, NVS_READONLY, &nvs) != ESP_OK) return false;
    size_t l1 = 33, l2 = 65;
    const bool ok = nvs_get_str(nvs, CRED_SSID, ssid, &l1) == ESP_OK
                 && nvs_get_str(nvs, CRED_PASS, pass, &l2) == ESP_OK
                 && ssid[0] != '\0';
    nvs_close(nvs);
    return ok;
}

static bool creds_save(const char *ssid, const char *pass) {
    nvs_handle_t nvs;
    if (nvs_open(CRED_NS, NVS_READWRITE, &nvs) != ESP_OK) return false;
    const bool ok = nvs_set_str(nvs, CRED_SSID, ssid) == ESP_OK
                 && nvs_set_str(nvs, CRED_PASS, pass) == ESP_OK
                 && nvs_commit(nvs) == ESP_OK;
    nvs_close(nvs);
    return ok;
}

// ---- STA 事件 ----

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data) {
    (void)arg; (void)base; (void)data;
    if (id == WIFI_EVENT_STA_START) {
        if (!s_join_in_progress) (void)esp_wifi_connect();
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

// STA 连接:≤10s 拿到 IP 返回 ESP_OK,否则断开并返回错误。成功时回填
// s_ip/s_ssid。可重入(配网任务与 enter 都可能走),靠调用方串行化。
static esp_err_t sta_join_with(const char *ssid, const char *pass) {
    // STA_START 补投对每个 netif 生命周期只允许一次:胶合层的 start 动作
    // 会 netif_add,而 lwIP 的 netif_add 对已在链表上的同一 struct 直接
    // assert(真机踩雷:反复进出上行模式,二次补投 → "netif already
    // added" 崩溃重启)。首次补投后胶合层已与接口同步,后续进模式不必
    // 再投。
    const bool fresh_netif = !s_sta_netif;
    if (fresh_netif) {
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
    wifi_config_t sta;
    memset(&sta, 0, sizeof(sta));
    memcpy(sta.sta.ssid, ssid, sizeof(sta.sta.ssid) - 1);
    memcpy(sta.sta.password, pass, sizeof(sta.sta.password) - 1);
    sta.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta);
    if (err != ESP_OK) return err;
    xSemaphoreTake(s_sta_sem, 0);   // 清残留
    // 守卫必须先于补投事件置位:STA_START 处理会走到自连 handler,晚置位
    // 会让它拿旧配置抢先 connect,把这里的 set_config 撞成 "cannot set
    // config"(真机踩雷)。守卫期间 handler 跳过 connect,由本函数唯一发起。
    s_join_in_progress = true;
    // STA 接口早于本 netif 启动(radar_espnow 先起 WiFi,STA_START 已在
    // 启动时发完):默认胶合层的 wifi_start(netif up + input 绑定 + rxcb)
    // 从未执行 —— 不补投则首个 RX 包打到 netif->input==NULL 直接空指针
    // 崩(真机踩雷)。补投仅对新建 netif 做一次:胶合层 start 动作会
    // netif_add,重复补投触发 lwIP "netif already added" 断言(真机踩雷)。
    if (fresh_netif) {
        if (esp_event_post(WIFI_EVENT, WIFI_EVENT_STA_START, NULL, 0,
                           pdMS_TO_TICKS(1000)) != ESP_OK) {
            s_join_in_progress = false;
            return ESP_FAIL;
        }
        vTaskDelay(pdMS_TO_TICKS(50));   // 让胶合层先完成 netif up 再 connect
    }
    err = esp_wifi_connect();   // APSTA 模式下 STA 接口已启动
    s_join_in_progress = false;
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
    s_sta_joined = true;
    ESP_LOGI(TAG, "sta joined ssid=%s ip=%s", s_ssid, s_ip);
    return ESP_OK;
}

// ---- 配网:DNS 劫持(captive portal 弹窗信号) ----

#define DNS_PORT       53
#define DNS_TASK_STACK 3072
#define DNS_TASK_PRIO  3

static TaskHandle_t s_dns_task;
static volatile bool s_dns_run;
static int s_dns_sock = -1;

// DNS 查询报文 Question 段终点:12B 头 + 标签序列(0xC0 指针截断)+ 4B
// QTYPE/QCLASS。返回 0 = 非预期格式。
static size_t dns_query_end(const uint8_t *pkt, size_t n) {
    if (n < 12) return 0;
    size_t i = 12;
    for (;;) {
        if (i >= n) return 0;
        const uint8_t len = pkt[i];
        if (len == 0) { i += 1; break; }
        if ((len & 0xC0) == 0xC0) { i += 2; break; }   // 压缩指针
        i += 1 + len;
    }
    if (i + 4 > n) return 0;
    return i + 4;
}

static void dns_task_main(void *arg) {
    (void)arg;
    uint8_t pkt[512];
    struct sockaddr_storage from;
    socklen_t fromlen;
    while (s_dns_run) {
        fromlen = sizeof(from);
        const ssize_t n = recvfrom(s_dns_sock, pkt, sizeof(pkt), 0,
                                   (struct sockaddr *)&from, &fromlen);
        if (n <= 0) continue;
        const size_t end = dns_query_end(pkt, (size_t)n);
        if (end == 0) continue;
        pkt[2] |= 0x80;      // QR=1(响应);RA=1
        pkt[3] |= 0x80;
        pkt[6] = pkt[7] = 0; // ANCOUNT/NSCOUNT/ARCOUNT = 0:无应答记录,
        pkt[8] = pkt[9] = 0; // 手机只能把它当"captive portal"判定信号
        pkt[10] = pkt[11] = 0;
        (void)sendto(s_dns_sock, pkt, end, 0, (struct sockaddr *)&from, fromlen);
    }
    vTaskDelete(NULL);       // 任务自清理
}

static esp_err_t dns_relay_start(void) {
    if (s_dns_sock >= 0) return ESP_OK;
    s_dns_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_dns_sock < 0) return ESP_FAIL;
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(s_dns_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(s_dns_sock);
        s_dns_sock = -1;
        return ESP_FAIL;
    }
    s_dns_run = true;
    if (xTaskCreate(dns_task_main, "up_dns", DNS_TASK_STACK, NULL,
                    DNS_TASK_PRIO, &s_dns_task) != pdPASS) {
        s_dns_run = false;
        close(s_dns_sock);
        s_dns_sock = -1;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "captive portal DNS ready (UDP 53)");
    return ESP_OK;
}

static void dns_relay_stop(void) {
    s_dns_run = false;
    if (s_dns_sock >= 0) {
        shutdown(s_dns_sock, 0);
        close(s_dns_sock);   // 唤醒阻塞 recvfrom,任务见 !s_dns_run 自行退出
        s_dns_sock = -1;
    }
}

// ---- 配网:表单解析(URL decode + 容量拒绝,超限拒绝而非截断) ----

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// 从 query 取键值并 URL 解码到 out(cap 含 NUL)。键缺失/解码超容/键后
// 无 '=' 一律 false。% 后非两位十六进制按字面处理(宽松,SSID 可含 '%')。
static bool query_get(const char *q, const char *key, char *out, size_t cap) {
    const size_t klen = strlen(key);
    const char *p = q;
    while ((p = strstr(p, key)) != NULL) {
        if ((p == q || p[-1] == '&' || p[-1] == '?') && p[klen] == '=') break;
        p += klen;
    }
    if (!p) return false;
    p += klen + 1;
    size_t n = 0;
    while (*p && *p != '&') {
        char c;
        if (*p == '%' && hex_val(p[1]) >= 0 && hex_val(p[2]) >= 0) {
            c = (char)(hex_val(p[1]) * 16 + hex_val(p[2]));
            p += 3;
        } else if (*p == '+') {
            c = ' ';
            p += 1;
        } else {
            c = *p++;
        }
        if (n + 1 >= cap) return false;   // 超容拒绝
        out[n++] = c;
    }
    out[n] = '\0';
    return true;
}

// JSON 字符串转义(" \ 与控制字符;SSID 合法字符集外无其它,上界 4x)。
static void json_escape_ssid(const char *in, char *out, size_t cap) {
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        const char *esc = NULL;
        char tmp[8];
        switch (*p) {
        case '"':  esc = "\\\""; break;
        case '\\': esc = "\\\\"; break;
        default:
            if (*p < 0x20) {
                snprintf(tmp, sizeof(tmp), "\\u%04x", *p);
                esc = tmp;
            }
        }
        const char *s = esc ? esc : (const char[]){ (char)*p, 0 };
        for (const char *q = s; *q; q++) {
            if (n + 1 >= cap) { out[n] = '\0'; return; }
            out[n++] = *q;
        }
    }
    out[n] = '\0';
}

// ---- HTTP 路由 ----

// GET / —— 按模式分流:STA 直接给上传 boot 页;热点模式给配网门户
// (Host 非本机 = captive portal 探测 → 302 弹配置页,照抄 meta-pass)。
static esp_err_t h_root(httpd_req_t *req) {
    if (s_mode == RADAR_UPLOAD_STA) {
        char html[sizeof(SHELL_HTML_FMT) + 16];
        snprintf(html, sizeof(html), SHELL_HTML_FMT, s_ip);
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    }
    char host[64] = {0};
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) == ESP_OK
        && strncmp(host, "192.168.4.1", sizeof("192.168.4.1") - 1) != 0) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
        httpd_resp_set_hdr(req, "Content-Length", "0");
        return httpd_resp_send(req, "", 0);
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, PROV_HTML, HTTPD_RESP_USE_STRLEN);
}

// GET /up —— 上传 boot 页(热点模式下的知情数据网络退路)。
static esp_err_t h_up(httpd_req_t *req) {
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

// GET /api/identity —— v7 设备身份:MAC 派生哈希 + Ed25519 公钥,供注册
// 到云端 KV(first-wins 绑定后可验签,见 capture-sig-contract)。
static esp_err_t h_identity(httpd_req_t *req) {
    uint8_t hash[16], pubkey[32];
    if (radar_identity_get(hash, pubkey) != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "identity unavailable");
    }
    char device[33], key[65];
    for (int i = 0; i < 16; i++) snprintf(device + i * 2, 3, "%02x", hash[i]);
    device[32] = '\0';
    for (int i = 0; i < 32; i++) snprintf(key + i * 2, 3, "%02x", pubkey[i]);
    key[64] = '\0';
    char json[180];
    snprintf(json, sizeof(json),
             "{\"device\":\"%s\",\"pubkey\":\"%s\",\"kind\":\"mac-ed25519\"}",
             device, key);
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

// GET /api/scan → 同步扫一轮的 APSTA 结果 JSON:
// {"networks":[{"ssid":"...","rssi":-52,"auth":true},...]}
// 收尾语义(照抄 meta_store_net.c r8 修复):"扫描完成但记录未取"的残留态会
// 同时卡死下一次扫描与 STA 连接,故 get_ap_records 必须无条件执行。
#define PROV_SCAN_MAX 20

static esp_err_t h_scan(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    esp_wifi_scan_start(NULL, false);
    // 阻塞等扫描完成(全信道 ~1.5-2s)。httpd 在独立任务,页面 fetch 无超时,
    // 配网门户单用途,期间阻塞可接受;手机已连热点,beacon 暂停不断开已建链。
    vTaskDelay(pdMS_TO_TICKS(2000));

    uint16_t count = PROV_SCAN_MAX;
    wifi_ap_record_t *records = malloc((size_t)count * sizeof(wifi_ap_record_t));
    if (!records) {
        return httpd_resp_sendstr(req, "{\"networks\":[]}");
    }
    // 无论扫到多少条(含 0),get_ap_records 成功返回即完成清态。
    const esp_err_t fetch = esp_wifi_scan_get_ap_records(&count, records);
    if (fetch != ESP_OK) {
        ESP_LOGW(TAG, "scan fetch failed: %s", esp_err_to_name(fetch));
        free(records);
        return httpd_resp_sendstr(req, "{\"networks\":[]}");
    }
    if (count == 0) {
        free(records);
        return httpd_resp_sendstr(req, "{\"networks\":[]}");
    }

    // 每条上界:转义 ssid(4x32+1=129)+固定 JSON(~48);外加首尾包装。
    const size_t cap = 32 + (size_t)count * (129 + 48);
    char *json = malloc(cap);
    if (!json) {
        free(records);
        return httpd_resp_sendstr(req, "{\"networks\":[]}");
    }
    size_t len = 0;
    json[0] = '\0';
    const char *head = "{\"networks\":[";
    memcpy(json, head, strlen(head));
    len = strlen(head);
    for (uint16_t i = 0; i < count; i++) {
        // 有界组装:溢出即截断输出(页面把残缺列表按空处理,无危害)。
        char esc[129];
        json_escape_ssid((const char *)records[i].ssid, esc, sizeof(esc));
        char item[200];
        snprintf(item, sizeof(item), "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%s}",
                 i ? "," : "", esc, (int)records[i].rssi,
                 records[i].authmode != WIFI_AUTH_OPEN ? "true" : "false");
        size_t n = strlen(item);
        if (len + n + 2 >= cap) break;
        memcpy(json + len, item, n);
        len += n;
    }
    const char *tail = "]}";
    if (len + 3 < cap) { memcpy(json + len, tail, 3); len += 2; }
    free(records);
    const esp_err_t ret = httpd_resp_send(req, json, len);
    free(json);
    return ret;
}

// 配网任务:接到通知后读 NVS 凭据并连 STA;成功原地转 STA 模式
// (httpd 绑 0.0.0.0 不动,手机回路由器后开新地址即可继续)。
static void prov_task_main(void *arg);

// 信号量必须与任务同生:任务首行就 take,晚了就是 NULL 断言(真机踩雷)。
static void prov_task_ensure(void) {
    if (s_prov_task) return;
    if (!s_prov_sem) s_prov_sem = xSemaphoreCreateBinary();
    if (!s_prov_sem) return;
    if (xTaskCreate(prov_task_main, "up_prov", 4096, NULL, 4,
                    &s_prov_task) != pdPASS) {
        s_prov_task = NULL;            // 门户仍可改凭据,下次再建
    }
}

// POST /api/wifi?ssid=..&pass=.. —— 表单提交:校验→存 NVS→通知配网任务。
// 不在 handler 里动 WiFi(httpd 任务栈与全局状态纪律:handler 只做参数搬运);
// 超限拒绝而非截断(截断会把长 SSID 悄悄改写成连不上的另一个名字)。
// 开放网络(空密码)拒绝:极不安全且极少见(与 meta-pass 同口径)。
static esp_err_t h_wifi(httpd_req_t *req) {
    char arg[340] = {0};
    char ssid[33] = {0};
    char pass[65] = {0};
    if (httpd_req_get_url_query_str(req, arg, sizeof(arg)) != ESP_OK ||
        !query_get(arg, "ssid", ssid, sizeof(ssid)) ||
        !query_get(arg, "pass", pass, sizeof(pass)) ||
        ssid[0] == '\0' || pass[0] == '\0') {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "bad request");
    }
    if (!creds_save(ssid, pass)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
    }
    prov_task_ensure();                // STA 直进模式下任务可能还没建
    if (!s_prov_task) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no task");
    }
    ESP_LOGI(TAG, "provisioning creds for ssid=%s", ssid);
    xSemaphoreGive(s_prov_sem);
    return httpd_resp_sendstr(req, "ok");
}

// 配网任务:接到通知后读 NVS 凭据并连 STA;成功原地转 STA 模式
// (httpd 绑 0.0.0.0 不动,手机回路由器后开新地址即可继续)。
static void prov_task_main(void *arg) {
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_prov_sem, portMAX_DELAY);
        if (!s_active || s_mode == RADAR_UPLOAD_STA) continue;
        char ssid[33], pass[65];
        if (!creds_load(ssid, pass)) {
            ESP_LOGW(TAG, "prov: creds gone after save?!");
            continue;
        }
        ESP_LOGI(TAG, "prov: joining %s ...", ssid);
        if (sta_join_with(ssid, pass) == ESP_OK) {
            s_mode = RADAR_UPLOAD_STA;
            s_join_failed = false;
            ESP_LOGI(TAG, "prov: joined, mode=sta ip=%s", s_ip);
        } else {
            s_join_failed = true;
            ESP_LOGW(TAG, "prov: join failed, staying on hotspot");
        }
    }
}

static esp_err_t start_httpd(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;                 // read handler 栈上 4KB 块缓冲
    cfg.max_uri_handlers = 8;
    cfg.lru_purge_enable = true;
    esp_err_t err = httpd_start(&s_httpd, &cfg);
    if (err != ESP_OK) return err;
    const httpd_uri_t uris[] = {
        { "/", HTTP_GET, h_root, NULL },
        { "/up", HTTP_GET, h_up, NULL },
        { "/api/capture/info", HTTP_GET, h_info, NULL },
        { "/api/identity", HTTP_GET, h_identity, NULL },
        { "/api/capture/read", HTTP_GET, h_read, NULL },
        { "/api/scan", HTTP_GET, h_scan, NULL },
        { "/api/wifi", HTTP_POST, h_wifi, NULL },
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

// ---- 公共接口 ----

const char *radar_upload_ap_ip(void) { return s_ip; }

radar_upload_mode_t radar_upload_mode(void) { return s_mode; }

bool radar_upload_join_failed(void) { return s_join_failed; }

const char *radar_upload_sta_ssid(void) { return s_ssid; }

esp_err_t radar_upload_enter(void) {
    if (s_active) return ESP_OK;
    s_mode = RADAR_UPLOAD_NONE;
    s_ap_dirty = false;
    s_sta_joined = false;
    s_join_failed = false;
    s_ssid[0] = '\0';

    char ssid[33], pass[65];
    const bool have_creds = creds_load(ssid, pass);

    // 1) Wi-Fi 优先:凭据在且连上 → 全程 Wi-Fi。
    if (have_creds && sta_join_with(ssid, pass) == ESP_OK) {
        s_mode = RADAR_UPLOAD_STA;
    } else {
        // 2) 开放热点 + 配网门户。AP 模式(有凭据但连不上)屏幕明示将走
        //    手机数据网络;PROV 模式(无凭据)门户即首页。
        if (have_creds) s_join_failed = true;
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
        s_ap_dirty = true;
        snprintf(s_ip, sizeof(s_ip), "192.168.4.1");
        s_mode = have_creds ? RADAR_UPLOAD_AP : RADAR_UPLOAD_PROV;
        (void)dns_relay_start();               // captive portal 弹窗信号
        prov_task_ensure();
    }

    // v7:进上传模式即懒签批次(全载荷 SHA-256 + Ed25519 写 trailer 扇区,
    // 幂等;无批次秒回)。发生在 upload 任务上下文,不持 LVGL 锁,百毫秒级
    // 延迟可接受。
    (void)radar_capture_batch_sign_if_needed();

    const esp_err_t err = start_httpd();
    if (err != ESP_OK) {
        dns_relay_stop();
        if (s_ap_dirty) (void)esp_wifi_set_config(WIFI_IF_AP, &s_saved_ap);
        s_mode = RADAR_UPLOAD_NONE;
        return err;
    }
    s_bytes_served = 0;
    s_active = true;
    ESP_LOGI(TAG, "upload service up mode=%d ip=%s", (int)s_mode, s_ip);
    return ESP_OK;
}

esp_err_t radar_upload_exit(void) {
    if (!s_active) return ESP_OK;
    s_active = false;
    dns_relay_stop();
    if (s_httpd) {
        (void)httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    if (s_sta_joined) {
        (void)esp_wifi_disconnect();
        (void)esp_wifi_set_channel(HOME_CHANNEL, WIFI_SECOND_CHAN_NONE);
        ESP_LOGI(TAG, "sta left, channel restored to %d", HOME_CHANNEL);
        s_sta_joined = false;
    }
    if (s_ap_dirty) {
        (void)esp_wifi_set_config(WIFI_IF_AP, &s_saved_ap);   // 恢复 FTM AP 配置
        s_ap_dirty = false;
    }
    s_mode = RADAR_UPLOAD_NONE;
    ESP_LOGI(TAG, "upload service down (served %lu bytes)",
             (unsigned long)s_bytes_served);
    return ESP_OK;
}

bool radar_upload_active(void) { return s_active; }
