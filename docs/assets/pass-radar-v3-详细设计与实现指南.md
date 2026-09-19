# pass-radar v3 详细设计与实现指南

> 目标读者：初级固件工程师（会 C、会用 ESP-IDF 编译烧录即可）
> 日期：2026-09-19
> 上游文档：[精度目标评估与 v3 设计方案](pass-radar-定向测距精度目标评估与v3设计方案.md)（"为什么这么做"）
> 本文回答"具体怎么做"：改哪个文件、加哪个函数、参数取多少、按什么顺序做、做完怎么验证。
> 配套：[测试设计与用例](pass-radar-v3-测试设计与用例.md)
> 范围：本文是 **一期（v3）**，目标是 15 m 内的测距/定向与诚实降级。**15~100 m 远距与树丛/围墙遮挡是二期（v3.1）**，在一期 M3 验收后启动，见 [v3.1 远距与遮挡实施指南](pass-radar-v3.1-远距与遮挡实施指南.md)与[远距与遮挡场景分析](pass-radar-v3-远距与遮挡场景分析.md)。一期代码中标注"为二期预留"的字段/接口必须实现，但不实现二期的算法。

---

## 0. 开始前必读

### 0.1 你需要先知道的仓库事实

| 事实 | 位置 | 对你的影响 |
| --- | --- | --- |
| 纯逻辑模块与 IDF 模块分离 | `radar_sweep.c`、`radar_fusion.c`、`radar_kalman.c`、`radar_protocol.c` 不 include 任何 `esp_*.h` | 这些文件能在电脑上用 `cc` 编译测试；**新算法一律先写到这类文件里** |
| host 测试的登记处 | `tools/validate.sh` 的 `run_static_checks()` | 新增 `tests/test_xxx.c` 后必须在这里加一行编译+运行 |
| 编译/烧录 | `./tools/validate.sh --static`（电脑测试）、`--firmware`（出固件） | 每完成一个小步都跑 `--static` |
| LVGL 非线程安全 | 硬件指南 §5.2 | `lv_*` 只能在 `lv_timer` 回调里直接调；按键回调/其他任务必须 `bsp_lvgl_lock()` |
| ESP-NOW 接收回调在 Wi-Fi 任务 | `radar_espnow.c` `espnow_recv_cb` | 回调里只能写 `volatile` 变量/数组，**不能**调 LVGL、不能 `ESP_LOGI` 太多、不能阻塞 |
| 音频阻塞 | `bsp_audio_write` 是阻塞调用 | 必须在独立 FreeRTOS 任务里播放 |
| 无 PSRAM | 硬件指南 §10 | 每个新 buffer 都写进第 6 节的内存表 |
| 按键：OK LONG2(3 s)=回启动器 | `main.c` `on_key` | 不要给任何键的长按分配新功能 |

### 0.2 v3 总体改动一览

```
main/
  radar_espnow.c/.h   改：去跳频、扫盘 40 Hz、样本结构扩展、v3 标志位、CSI 挂接
  radar_ftm.c/.h      改：偏移标定、令牌交替发起、P25 窗口、RSSI 门控
  radar_fusion.c/.h   改：环境等级 L0~L3 + 滞回、区间输出、两射线分段模型、新融合规则
  radar_sweep.c/.h    改：多观测量投票、6 s/256 样本；去掉跳频调度的调用
  radar_ui.c/.h       改：区间/扇区渲染、节拍器页、寻的页、诊断入口 DOWN 双击
  radar_ble.c         改：惰性启动/停止（已有 deinit）
  radar_csi.c/.h      新：CSI 组能量统计
  radar_audio_cue.c/.h 新：节拍器 + 寻的音调
  radar_rf_sched.c/.h 新：Tracking/Sweep/Homing/Calib/Lost 状态机
  radar_diag.c/.h     新：日志环形缓冲 + 计数器
  main.c              改：接线到 rf_sched
tests/
  test_radar_fusion.c 扩、test_radar_sweep.c 扩、
  test_radar_diag.c 新、test_radar_ftm_logic.c 新
（二期 v3.1 新增 radar_obstruct.c/.h 与 test_radar_obstruct.c，不在本文范围）
```

---

## 1. 系统总览

### 1.1 数据流

```
[对端卡]  ──ESP-NOW 心跳(20/40 Hz)──▶  espnow_recv_cb (Wi-Fi 任务)
                                       ├─ 卡尔曼 → s_filtered_rssi
                                       ├─ 扫盘/寻的中：追加 radar_sample_t
                                       └─ 解析 v3 包：对端 RSSI/角度/距离/FTM 令牌/环境等级
[对端卡]  ──Wi-Fi 帧──▶ csi_rx_cb (Wi-Fi 任务)
                          └─ 组能量 → 扫盘/寻的中追加 CSI 样本序列
[对端卡]  ◀─FTM 8 帧─▶ ftm_event_handler (事件任务)
                          └─ 中值/IQR → 减偏移 → 5 会话窗口 P25 → 快照
                     ┌──────────────────────────────────────────┐
100 ms lv_timer ───▶│ rf_sched_tick → 读三种快照 → radar_fusion   │ (LVGL 任务)
                     │ → 区间/等级/置信度 → UI（二期在此插入 radar_obstruct）│
                     └──────────────────────────────────────────┘
```

### 1.2 射频调度状态机（`radar_rf_sched`）

```
            OK 短按                6 s 到                OK 短按(结果页)
 TRACKING ─────────▶ SWEEP ──────────▶ RESULT ──────────────▶ HOMING
    ▲                  │ OK 长按取消      │ OK 短按               │ OK 短按确认 / 20 s 超时
    │◀─────────────────┘◀────────────────┘◀──────────────────────┘
    │  OK 长按(主页)
    ├──────────────▶ CALIB(3 s) ──▶ TRACKING
    │  ESP-NOW 5 s 无包
    └──────────────▶ LOST(BLE 哨兵开) ──包恢复 10 s──▶ TRACKING
```

每个状态的射频/外设配置（`rf_sched_enter(state)` 一次性设置）：

| 状态 | ESP-NOW 周期 | FTM | CSI 采样 | BLE | 音频 |
| --- | --- | --- | --- | --- | --- |
| TRACKING | 50 ms | 令牌交替，1 s | 仅统计频率选择性（不存序列） | 关 | 无 |
| SWEEP | 25 ms | 暂停 | 存序列 | 关 | 节拍器 8 拍 |
| RESULT | 50 ms | 恢复 | 否 | 关 | 无 |
| HOMING | 25 ms | 每 2 s | 存滑窗序列 | 关 | 音调随信号变化 |
| CALIB | 50 ms | 连续（每 500 ms） | 否 | 关 | 结束提示音 |
| LOST | 50 ms | 停 | 否 | **开**（哨兵） | 无 |

---

## 2. 实施顺序（按此顺序做，每步可独立验收）

| 步 | 里程碑 | 做什么 | 验收 |
| --- | --- | --- | --- |
| S1 | M0 | `radar_diag` 日志环形缓冲 + 计数器 + 诊断页入口改 DOWN 双击 + `±`→`+/-` | 机上能看到 FTM status 直方图、样本计数 |
| S2 | M1 | `radar_espnow` 去跳频、协议 v3 标志位 | 扫盘 8 方向不再 OOR；日志无 `channel switch` |
| S3 | M1 | `radar_ble` 惰性启动（LOST 才开） | Tracking 态 `heap_caps_get_free_size` 比 v2 多 ≥60 KB；FTM status=3 次数下降 |
| S4 | M1 | `radar_ftm` 偏移标定 + P25 窗口 + RSSI 门控 | 1/3/5 m 静止：FTM 距离误差 ≤1 m（室内） |
| S5 | M1 | `radar_ftm` 令牌交替发起 | 诊断页两卡都出现 `FTM ok` |
| S6 | M1 | `radar_fusion` 环境等级 + 滞回 + 区间输出 + 两射线分段 | 3.5 m 处显示不再 1~5 m 跳变 |
| S7 | M2 | `radar_espnow` 扫盘 40 Hz/6 s/256 样本；`radar_sweep` 多观测量投票 | host 测试通过；机上样本数 ≥180 |
| S8 | M2 | `radar_audio_cue` 节拍器 | 听到 8 拍；扫盘页指针同步 |
| S9 | M2 | `radar_csi` 组能量 + 接入投票 | 诊断页出现 CSI 帧数与选择性指标 |
| S10 | M3 | `radar_rf_sched` 状态机收口 + HOMING 页 + 音调 | 寻的 20 s 内收敛 |
| — | 二期 | 15~100 m 远距与遮挡（`radar_obstruct`、FTM 探针、举高/步行测试等） | 见 [v3.1 实施指南](pass-radar-v3.1-远距与遮挡实施指南.md)，一期 M3 验收后启动 |

---

## 3. 模块详细设计

### 3.1 `radar_diag` — 机上可观测性（S1）

**目的**：测试者没有电脑也能看到日志和计数。

```c
// radar_diag.h
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define RADAR_DIAG_LINES 32
#define RADAR_DIAG_LINE_LEN 96

typedef enum {
    DIAG_CNT_ESPNOW_RX = 0,   // 收到对端包
    DIAG_CNT_ESPNOW_DROP,     // 序号回退/身份不符丢弃
    DIAG_CNT_FTM_INITIATE,    // 发起次数
    DIAG_CNT_FTM_OK,          // 成功会话
    DIAG_CNT_FTM_STATUS_1,    // status=UNSUPPORTED
    DIAG_CNT_FTM_STATUS_2,    // CONF_REJECTED
    DIAG_CNT_FTM_STATUS_3,    // NO_RESPONSE
    DIAG_CNT_FTM_STATUS_4,    // NO_VALID_MSMT
    DIAG_CNT_FTM_STATUS_OTHER,
    DIAG_CNT_FTM_INITIATE_ERR,// esp_wifi_ftm_initiate_session 返回错误
    DIAG_CNT_CSI_RX,          // CSI 回调次数
    DIAG_CNT_SWEEP_SAMPLES,   // 最近一次扫盘样本数（用 set 而非 inc）
    DIAG_CNT_MAX
} radar_diag_counter_t;

void radar_diag_init(void);                      // 安装 esp_log 钩子
void radar_diag_inc(radar_diag_counter_t c);     // 任意任务可调（原子加）
void radar_diag_set(radar_diag_counter_t c, uint32_t v);
uint32_t radar_diag_get(radar_diag_counter_t c);
// 拷贝第 idx 条最近日志（0=最新）；不存在返回 false
bool radar_diag_line(unsigned idx, char *out, unsigned out_len);
```

**实现要点**：

1. 用 `esp_log_set_vprintf(diag_vprintf)` 挂钩：`diag_vprintf` 先调用原 vprintf（保留 USB 串口输出），再把格式化结果写入环形缓冲。缓冲 `static char s_lines[32][96]`（3 KB），写指针用 `portENTER_CRITICAL` 保护（IDF 的 spinlock `portMUX_TYPE`）。
2. 计数器 `static volatile uint32_t s_cnt[DIAG_CNT_MAX]`，`radar_diag_inc` 用 `__atomic_fetch_add`。
3. **在 `ftm_event_handler` 里按 status 调用 `radar_diag_inc`**，在 `espnow_recv_cb` 里 `radar_diag_inc(DIAG_CNT_ESPNOW_RX)`。
4. 诊断页（`radar_ui.c`）第二屏显示 8 条最新日志（等宽小字体 `lv_font_unscii_8` 或已有像素字体），UP/DOWN 翻页。入口：主页 **DOWN 双击**（`BSP_BTN_DOWN && BSP_BTN_DOUBLE`）。原因：实测 UP 长按被误读，任何"长按"都要避开。
5. `±` 替换：`radar_ui.c` 中所有 `"±"` 改为 `"+/-"`（Montserrat 内置字体无该字形）。

### 3.2 `radar_espnow` — 去跳频、协议 v3、样本结构（S2、S7）

**3.2.1 去跳频**：删除 `broadcast_tick()` 中的 `hop_tick(now)` 调用，`radar_espnow_stop_sweep()` 中恢复信道的代码保留（防御性）。`radar_sweep_channel_at()` 函数保留（有 host 测试），但不再被固件调用。

**3.2.2 协议 v3**：不改变包长度（保持与 v2 二进制兼容），把 `v2_reserved[2]` 改名启用：

```c
// radar_protocol.h
#define RADAR_PROTO_VER 3
// v3: v2_reserved[0] → v3_flags, v2_reserved[1] → env_level
#define RADAR_V3_FLAG_FTM_GRANT   0x01  // "轮到你发起 FTM"（令牌）
#define RADAR_V3_FLAG_FTM_BUSY    0x02  // 我正在一个 session 中
#define RADAR_V3_FLAG_HOMING      0x04  // 我在寻的模式（请你保持静止）
#define RADAR_V3_FLAG_OBSTRUCTED  0x08  // 我判定链路被遮挡
#define RADAR_V3_FLAG_FTM_CALIB   0x10  // 我在标定，请连续响应
typedef struct __attribute__((packed)) { ... uint8_t v3_flags; uint8_t env_level; } radar_packet_t;
```

`radar_protocol_valid_v2()` 接受 `proto_ver >= 2`（v2 对端仍可互通，只是没有令牌，此时回退为"MAC 小者单向发起"）。

**3.2.3 样本结构扩展**（`radar_sweep.h`）：

```c
typedef struct {
    uint32_t timestamp_ms;   // 相对扫盘/寻的窗口起点
    float    rssi;           // 卡尔曼后
    float    rssi_raw;       // 原始整数 RSSI（供环境统计）
    uint8_t  channel;        // 保留兼容，v3 恒为 1
    uint8_t  source;         // RADAR_SRC_RSSI=0 / RADAR_SRC_CSI_G0..G3=1..4
    uint8_t  reserved[2];
} radar_sample_t;            // 16 B
#define RADAR_MAX_SAMPLES 256        // RSSI 序列（40 Hz × 6 s = 240）
#define RADAR_MAX_CSI_SAMPLES 256    // CSI 序列，每条样本含 4 组能量（见 3.4）
#define RADAR_SWEEP_DURATION_MS 6000
```

RSSI 序列 4 KB；CSI 序列在 `radar_csi.c` 内独立存放（见 3.4），4 组 × 256 × 4 B = 4 KB。

**3.2.4 扫盘提速**：`radar_espnow_set_period_ms(uint32_t ms)`：`esp_timer_stop` → `esp_timer_start_periodic(s_broadcast_timer, ms*1000)`。SWEEP/HOMING 进入时设 25，退出设 50。

**3.2.5 ESP-NOW 速率固定为 1 Mbps**（一期就做：保证量程不被默认速率限制，也是二期的前置）：`esp_now_add_peer` 后调用

```c
esp_now_rate_config_t rc = { .phymode = WIFI_PHY_MODE_11B, .rate = WIFI_PHY_RATE_1M_L, .ersu = false, .dcm = false };
esp_now_set_peer_rate_config(BROADCAST_MAC, &rc);
```

（IDF 5.5 API；若编译报未声明，检查 `esp_now.h` 版本并回退到 `esp_wifi_config_espnow_rate(WIFI_IF_AP, WIFI_PHY_RATE_1M_L)`。）

### 3.3 `radar_ftm` — 偏移标定、P25、令牌（S4、S5）

**3.3.1 常量调整**

```c
#define FTM_FRAMES_PER_SESSION 8        // 原 16
#define FTM_SESSION_TIMEOUT_US (600 * 1000)   // 原 1500 ms
#define FTM_PERIOD_US (1000 * 1000)     // 原 2 s
#define FTM_MIN_PEER_RSSI (-72)         // 低于此不发起
#define FTM_HISTORY 5                   // P25 窗口
#define FTM_OFFSET_MIN_M (-2.0f)
#define FTM_OFFSET_MAX_M (15.0f)
#define RADAR_NVS_KEY_FTMOFF "ftmoff"
```

**3.3.2 偏移标定**（纯逻辑放 `radar_fusion.c`，便于 host 测试）：

```c
// radar_fusion.h
// 标定期间收集到的 N 个 session 中值（米）→ 偏移 = 中值(all) − 1.0，钳位。
// count < 3 返回 NAN（表示标定失败）。
float radar_fusion_ftm_offset_from_calib(const float *session_medians_m, unsigned count);
// 应用偏移：raw − offset，下限 0.1 m
float radar_fusion_ftm_apply_offset(float raw_m, float offset_m);
```

流程：`start_calib()` 期间 `rf_sched` 进入 CALIB，`radar_ftm` 改为每 500 ms 发起（忽略令牌，两端都发，包上带 `FTM_CALIB` 位让对端不抢），收集 `≥3` 个成功 session 的中值。3 s 结束时同时保存 `txref` 与 `ftmoff`。**未标定（NVS 无 `ftmoff`）时 `radar_ftm_snapshot_t.calibrated=false`**，融合不采纳 FTM 显示，仅用于一致性校验。

**3.3.3 P25 窗口**（纯逻辑）：

```c
// 最近 FTM_HISTORY 个 session 距离（已减偏移）→ 25 分位（多径偏差恒为正，低分位≈直射径）
float radar_fusion_ftm_low_quantile(const float *hist_m, unsigned count);  // count 1..5
```

`ftm_event_handler` 成功时：`d = apply_offset(median(raw))` → push 到环形 `s_hist[5]` → `snapshot.distance_m = low_quantile(s_hist)`，`snapshot.spread_m = IQR(本 session)`，`snapshot.hist_spread_m = max(hist) − min(hist)`。

**3.3.4 令牌交替发起**

规则（两端对称，每 100 ms `radar_ftm_tick` 执行一次）：

```
状态：s_have_token (bool), s_token_since_us
初始：have_token = (本端 AP MAC < 对端 AP MAC)          // 与 v2 一致
每次收到对端包（espnow 传入）：
   若对端 v3_flags 含 FTM_GRANT 且 !s_have_token → s_have_token = true, s_token_since = now
   若对端 v3_flags 含 FTM_BUSY → s_peer_busy_until = now + 800 ms
tick：
   若 s_have_token 且 !s_session_busy 且 now ≥ s_peer_busy_until 且 peer_rssi ≥ −72
        且 now − s_last_session ≥ 1 s → 发起 session；发起成功后 set 本端 FTM_BUSY 位
   session 结束（report 事件，无论成败）：
        s_have_token = false；本端 v3_flags |= FTM_GRANT 持续 1 s（由 espnow 广播）
   看门狗：若两端都不持令牌 3 s（本端 !have_token 且 3 s 内没见到对端 BUSY/GRANT）
        → MAC 小者自动取回令牌
```

对端是 v2（无 v3 位）时：`s_have_token` 恒等于 "MAC 小者"，退化为 v2 行为。

**3.3.5 快照结构增加字段**

```c
typedef struct {
    bool valid, calibrated;
    wifi_ftm_status_t status;
    float distance_m;       // P25(窗口)，已减偏移
    float last_session_m;   // 本 session 中值（诊断）
    float spread_m;         // 本 session IQR
    float hist_spread_m;    // 窗口 max−min
    uint8_t valid_bursts, hist_count;
    int rssi; int64_t timestamp_us;
} radar_ftm_snapshot_t;
```

### 3.4 `radar_csi` — 子载波组能量（S9）

**关键事实（不知道会做错）**：ESP32 的 CSI 幅度经过 AGC 归一化，`Σ|H_k|²` 对所有包近似常数，**不含绝对功率信息**。绝对功率必须用同帧 `rx_ctrl.rssi` 恢复：

```
E_total = Σ_k |H_k|²  (k 遍历可用子载波)
E_g     = Σ_{k∈组g} |H_k|²
P_g(dBm) = rssi + 10·log10(E_g / E_total) + 10·log10(组数)    // 组 g 的等效功率
```

因此 CSI 提供的新信息是**频率选择性**（各组相对能量），而不是新的总功率。

```c
// radar_csi.h
#define RADAR_CSI_GROUPS 4
typedef struct {
    uint32_t timestamp_ms;
    float p_db[RADAR_CSI_GROUPS];   // 各组等效功率 dBm
    float selectivity_db;           // max(p_db) − min(p_db)
} radar_csi_sample_t;

esp_err_t radar_csi_init(void);                    // esp_wifi_set_csi_config + set_csi_rx_cb + set_csi(true)
void radar_csi_set_peer_mac(const uint8_t mac[6]); // 只统计该源 MAC（对端 AP MAC 或 STA MAC 都要接受）
void radar_csi_start_window(int64_t t0_us);        // 开始存序列（SWEEP/HOMING）
unsigned radar_csi_stop_window(radar_csi_sample_t *out, unsigned max);
float radar_csi_recent_selectivity_db(void);       // 最近 1 s 平均选择性（TRACKING 态供遮挡判别）
```

**回调实现**（Wi-Fi 任务，禁止阻塞/打印）：

```c
static void csi_rx_cb(void *ctx, wifi_csi_info_t *info) {
    if (!info || !info->buf || memcmp(info->mac, s_peer_mac, 6) != 0) return;
    radar_diag_inc(DIAG_CNT_CSI_RX);
    const int8_t *b = info->buf;
    int n = info->len / 2;                       // 复数个数
    int start = info->first_word_invalid ? 2 : 0;// C3：前 4 字节无效 = 前 2 个复数
    // HT20 LLTF：buf 顺序 0..31 正频，32..63 负频；可用子载波 ±1..±26
    float e[RADAR_CSI_GROUPS] = {0}, et = 0;
    for (int k = start; k < n && k < 64; ++k) {
        int sc = k < 32 ? k : k - 64;            // 映射到 −32..31
        int a = sc < 0 ? -sc : sc;
        if (a < 1 || a > 26) continue;           // 去 DC 与保护带
        float im = b[2*k], re = b[2*k+1];
        float p = re*re + im*im;
        int g = (a - 1) * RADAR_CSI_GROUPS / 26; // 0..3，按 |sc| 分 4 组
        e[g] += p; et += p;
    }
    if (et <= 0) return;
    radar_csi_sample_t s;
    s.timestamp_ms = (uint32_t)((esp_timer_get_time() - s_t0_us) / 1000);
    float mn = 1e9f, mx = -1e9f;
    for (int g = 0; g < RADAR_CSI_GROUPS; ++g) {
        s.p_db[g] = info->rx_ctrl.rssi + 10.0f*log10f(e[g]/et) + 10.0f*log10f(RADAR_CSI_GROUPS);
        if (s.p_db[g] < mn) mn = s.p_db[g];
        if (s.p_db[g] > mx) mx = s.p_db[g];
    }
    s.selectivity_db = mx - mn;
    push_recent(&s);                              // 1 s 滑窗（TRACKING 用）
    if (s_window_open && s_count < RADAR_MAX_CSI_SAMPLES) s_buf[s_count++] = s;
}
```

配置：

```c
wifi_csi_config_t cfg = { .lltf_en = true, .htltf_en = false, .stbc_htltf2_en = false,
                          .ltf_merge_en = false, .channel_filter_en = false, .manu_scale = false };
```

`sdkconfig.defaults` 增加 `CONFIG_ESP_WIFI_CSI_ENABLED=y`。CSI 回调对**所有**收到的 Wi-Fi 帧触发（含路由器信标），所以 MAC 过滤是必须的；对端发 ESP-NOW 用的是 AP 接口，源 MAC = 对端 AP MAC。

### 3.5 `radar_sweep` — 多观测量投票、6 s（S7、S9）

**不变**：`radar_sweep_harmonic_fit()`、`radar_sweep_angle_vote()`、`radar_sweep_estimate_period_ms()` 均复用。

**新增**：

```c
// 把 CSI 组序列转换成 radar_sample_t 序列（复用谐波拟合）。返回写入个数。
unsigned radar_sweep_from_csi_group(const radar_csi_sample_t *csi, unsigned n, unsigned group,
                                    radar_sample_t *out, unsigned max_out);

// 多观测量测角：对 RSSI 序列 + 4 个 CSI 组序列各做谐波拟合，
// 仅保留 幅度 ≥ min_amp_db 且 残差 ≤ max_res_db 的角度，按幅度加权后环形中值投票。
// out_spread_deg：入选角度最大环形差；out_n_used：入选个数。失败返回 -1。
float radar_sweep_multi_angle(const radar_sample_t *rssi, unsigned n_rssi,
                              const radar_csi_sample_t *csi, unsigned n_csi,
                              uint32_t duration_ms, float min_amp_db, float max_res_db,
                              float *out_spread_deg, unsigned *out_n_used);
```

`sweep_done()` 的新流程：

```
count = stop_sweep(); n_csi = radar_csi_stop_window()
duration = radar_sweep_effective_duration_ms(rssi 样本)      // 自校正照旧
angle = radar_sweep_multi_angle(..., min_amp 4 dB, max_res 3 dB, &spread, &n_used)
若 angle < 0 且 prominence ≥ 6 dB → 回退 peak_angle_interp，spread = 45
对端镜像：peer_angle 有效且 |angle − (peer_angle+180)| ≤ 30 → 环形均值，spread 取 max
显著性：n_used == 0 或 spread > 45 → 扇区模式
```

### 3.6 `radar_fusion` — 环境等级、区间、两射线、融合规则（S6）

**3.6.1 环境等级（替换 `radar_env_t` 的使用方式，保留枚举供 n 查表）**

```c
typedef enum { RADAR_LEVEL_L0_LOS = 0, RADAR_LEVEL_L1_INDOOR, RADAR_LEVEL_L2_NLOS, RADAR_LEVEL_L3_LOST } radar_level_t;

typedef struct {
    float ftm_success_rate;   // 最近 10 次发起的成功率 0..1（无发起 = NAN）
    float ftm_iqr_m;          // 最近成功 session IQR（无 = NAN）
    float rssi_p2p_db;        // 最近 8 s 滤波 RSSI 峰峰值
    float sym_gap_db;         // |本端−对端| RSSI
    float csi_selectivity_db; // 最近 1 s 平均选择性（无 = NAN）
    float mean_rssi_dbm;      // 最近 8 s 均值
    bool  peer_alive;
} radar_level_features_t;

typedef struct {
    radar_level_t level;
    int64_t since_ms;         // 进入当前等级的时刻
    int pending_level; int pending_count;  // 连续 N 次判到同一新等级才切换
} radar_level_est_t;

void radar_level_init(radar_level_est_t *e);
// 每 100 ms 调一次。滞回：候选等级连续出现 ≥ 30 次(3 s) 且当前等级已驻留 ≥ 10 s 才切换；
// 例外：peer_alive=false 立即 → L3；L3 恢复 → 需 peer_alive 连续 10 s，恢复后先进入 L1，
// 再由特征按常规滞回判到 L0/L2。
radar_level_t radar_level_update(radar_level_est_t *e, const radar_level_features_t *f, int64_t now_ms);
```

判据（`radar_level_classify()`，纯函数，单独可测）：

```
L3: !peer_alive
L0: ftm_success_rate ≥ 0.7 && ftm_iqr < 1.0 && rssi_p2p < 6 && sym_gap < 6 && (isnan(sel) || sel < 4)
L2: (ftm_success_rate < 0.3 && mean_rssi < −80) || rssi_p2p > 10 || ftm_iqr > 2.0 || sel > 8
L1: 其余
```

**3.6.2 路径损耗：两射线分段模型**（替换 `radar_fusion_sym_distance` 内部）

```c
#define RADAR_TWO_RAY_BREAK_M 45.0f   // 4·h²/λ, h=1.2 m, λ=0.1228 m
#define RADAR_FAR_N 4.0f
// rssi_sym → 距离：d ≤ 45 m 用 n_env；超过后 40 dB/十倍距
float radar_fusion_sym_distance(float rssi_sym, float tx_ref_sym, radar_env_t env) {
    float n = radar_env_path_loss_n(env);
    float rssi_break = tx_ref_sym - 10.0f * n * log10f(RADAR_TWO_RAY_BREAK_M);
    if (rssi_sym >= rssi_break) return powf(10.0f, (tx_ref_sym - rssi_sym) / (10.0f * n));
    return RADAR_TWO_RAY_BREAK_M * powf(10.0f, (rssi_break - rssi_sym) / (10.0f * RADAR_FAR_N));
}
```

**3.6.3 结果结构增加区间与等级**

```c
typedef struct {
    float distance_m, dist_lo_m, dist_hi_m;   // 点值与区间
    radar_confidence_t conf; radar_level_t level;
    float sym_rssi_dbm, rssi_dist_m, ftm_dist_m;
    bool ftm_used, obstructed, upper_bound_only;   // upper_bound_only: 只能说"不超过 hi"
} radar_fusion_result_t;
```

**3.6.4 融合主规则 `radar_fusion_update_v3()`**（伪代码；每行都要有对应单测，见测试文档 U-FUS）

签名：`radar_fusion_result_t radar_fusion_update_v3(const radar_fusion_ftm_observation_t *ftm, float local_rssi, float peer_rssi, float tx_ref_sym, radar_level_t level, bool peer_alive);`

**与 v2 的关键差别**：`RADAR_RSSI_MIN_VALID`（−88 dBm）不再触发 OOR。OOR 只由 `peer_alive=false`（5 s 无包）触发；RSSI 低于 −88 时仍给出距离，但强制 `conf=LOW`、`upper_bound_only=true`。（这条语义一期就定下，二期的远距量程依赖它。）扫盘结果页同理：`peak_rssi < −88` 不再显示 OUT OF RANGE，而是显示扇区并标 `weak`。

```
if !peer_alive: return OOR
rssi_d = sym_distance(sym, tx_ref, env_of(level))
e = {L0: 0.15, L1: 0.35, L2: 0.6}[level]
if ftm.valid && ftm.calibrated && ftm.fresh:
    if |ftm.d − rssi_d| ≤ max(2, 0.5·rssi_d):            # 一致
        d = ftm.d; lo = d − max(0.5, ftm.spread); hi = d + max(0.5, ftm.spread); conf = HIGH(L0)/MED(其他)
    elif ftm.d > rssi_d·1.8 + 2:                           # FTM 偏大 → 反射径
        d = min(ftm.d, rssi_d·1.3); lo = rssi_d/(1+e); hi = ftm.d; conf = MED
    else:                                                  # FTM 偏小 → RSSI 被遮挡衰减（二期会在此基础上细分）
        d = ftm.d; lo = ftm.d − 1; hi = ftm.d + 1; conf = MED; obstructed = true
elif sym 有效:
    d = rssi_d; lo = d/(1+e); hi = d·(1+e); conf = MED(L0/L1) / LOW(L2)
    if level == L2: upper_bound_only = true                # 显示 "≤ hi m"
elif 单向 RSSI: 同上, conf = LOW
else: OOR
if sym < −88: conf = LOW; upper_bound_only = true      # 弱信号：只承诺上界
```

**3.6.5 为二期预留的字段**：`radar_fusion_result_t` 中的 `obstructed`、`upper_bound_only`、`ftm_dist_m` 一期就要按 3.6.3/3.6.4 填好（二期的 `radar_obstruct_apply` 会读写它们）。一期 UI 对 `obstructed=true` 只需在点值后加 `~`。

### 3.7 `radar_audio_cue` — 节拍器与寻的音调（S8、S10）

```c
// radar_audio_cue.h
esp_err_t radar_audio_cue_init(void);        // 创建任务（栈 4 KB，优先级 4）；bsp_audio_init 幂等
void radar_audio_cue_metronome(unsigned beats, uint32_t interval_ms);  // 异步：beats 次"嗒"
void radar_audio_cue_homing_start(void);     // 开始寻的音：周期 200 ms 短音，频率由 set_level 决定
void radar_audio_cue_homing_set_level(float level_0_1);  // 0=冷(400 Hz) 1=热(1600 Hz)
void radar_audio_cue_stop(void);
void radar_audio_cue_beep_ok(void);          // 标定完成/确认提示
```

实现骨架（照 `official-main/main/demo_audio.c` 模式）：

```c
typedef struct { uint8_t kind; unsigned beats; uint32_t interval_ms; } cue_cmd_t;
static QueueHandle_t s_q;                     // 深度 4
static volatile float s_level;                // 寻的等级
static void task(void *arg) {
    int16_t buf[320];                         // 20 ms @16 kHz
    bsp_audio_set_format(16000, 16, 1); bsp_audio_set_volume(70);
    for (;;) {
        cue_cmd_t c;
        if (xQueueReceive(s_q, &c, pdMS_TO_TICKS(50)) != pdTRUE) continue;
        if (c.kind == CUE_METRONOME) {
            for (unsigned b = 0; b < c.beats && !s_stop; ++b) {
                gen_square(buf, 320, b == 0 ? 1200 : 800);   // 第 1 拍高音
                bsp_audio_write(buf, sizeof buf);             // 20 ms "嗒"
                vTaskDelay(pdMS_TO_TICKS(c.interval_ms - 20));
            }
        } else if (c.kind == CUE_HOMING) {
            while (!s_stop) {
                gen_square(buf, 320, 400 + (int)(1200 * s_level));
                bsp_audio_write(buf, sizeof buf);
                vTaskDelay(pdMS_TO_TICKS(180));
            }
        }
    }
}
```

节拍器参数：6 s 扫盘 → `beats=8, interval=750 ms`，第 0 拍在扫盘开始瞬间。**扫盘页 UI 指针**：`radar_ui_update_sweep_progress(progress)` 里画一根从中心出发、角度 = progress×360° 的指针（`lv_line` 两点），与节拍同源（都由 `s_sweep_start_ms` 计算），用户"跟着指针转"。

音频注意：只在 SWEEP/HOMING/CALIB 结束时用；播放期间不写 NVS（硬件指南 §8.1 杂音问题）；任务不得触碰 LVGL。

### 3.8 `radar_rf_sched` — 状态机（S10）

```c
typedef enum { RF_TRACKING, RF_SWEEP, RF_RESULT, RF_HOMING, RF_CALIB, RF_LOST } rf_state_t;
void rf_sched_init(void);
void rf_sched_request(rf_state_t s);          // 由按键/定时器调用（LVGL 锁内）
rf_state_t rf_sched_state(void);
void rf_sched_tick(int64_t now_ms);           // 100 ms，由 poll_tick 调用；内部处理超时/LOST 检测
```

`enter(s)` 表驱动实现，避免散落的 pause/resume：

```c
static const struct { uint32_t espnow_ms; bool ftm; bool csi_window; bool ble; } CFG[] = {
    [RF_TRACKING] = {50, true,  false, false},
    [RF_SWEEP]    = {25, false, true,  false},
    [RF_RESULT]   = {50, true,  false, false},
    [RF_HOMING]   = {25, true,  true,  false},
    [RF_CALIB]    = {50, true,  false, false},
    [RF_LOST]     = {50, false, false, true },
};
static void enter(rf_state_t s) {
    radar_espnow_set_period_ms(CFG[s].espnow_ms);
    radar_ftm_pause(!CFG[s].ftm);
    if (CFG[s].csi_window) radar_csi_start_window(now); else (void)radar_csi_stop_window(NULL, 0);
    if (CFG[s].ble) radar_ble_init(); else radar_ble_deinit();   // 惰性；两者都幂等
    s_state = s; s_since_ms = now;
}
```

LOST 判定：`!radar_espnow_peer_alive()` 持续 5 s → LOST；LOST 中 `peer_alive` 连续 10 s → TRACKING。`radar_ble_init()` 约需 300~500 ms，在 LVGL 任务内调用会卡 UI —— **放到一个一次性工作任务里执行**，或在 `enter` 里只置标志，由低优先级任务完成。

### 3.9 `radar_ui` — 新页面与渲染（S1、S7、S10）

| 页面 | 新增元素 | 数据来源 |
| --- | --- | --- |
| MAIN | `DIST 8.2 m (6.9~9.6) [MED] L1`；`upper_bound_only` 时 `DIST <= 12 m [LOW] L2`；`obstructed` 时点值后加 `~`（屏幕文字按仓库约定为英文，且内置字体无 `≤`，用 `<=`） | `radar_fusion_result_t` 的 lo/hi/level/upper_bound_only/obstructed |
| SWEEP | 匀速旋转指针 + 拍数 `3/8` | progress |
| RESULT | 射线（spread ≤ 30）或扇区（`lv_arc` 角度 ±spread）；`Angle 187 +/-9` | angle、spread |
| HOMING | 大箭头（还要转多少）+ 信号条 + `OK 确认` | 1 s 滑窗谐波拟合角 − 当前朝向估计 |
| DEBUG | 第 1 屏：v1/v2 距离、FTM 快照全部字段、令牌状态、CSI 选择性、level；第 2 屏：日志 8 行 | `radar_debug_info_t` 扩展 + `radar_diag_line` |

寻的页"还要转多少"的算法：HOMING 进入时以扫盘结果 θ₀ 为初值；每 100 ms 对最近 1 s（40 样本）RSSI+CSI 做谐波拟合得到 θ_now（相对本窗口起点的相位），把窗口起点定义为"用户当前朝向"，则**箭头 = θ_now**（0° 表示已对准）。用户按 OK 时记录并发送 `angle=0 相对朝向`；UI 显示 `LOCKED`。

### 3.10 `main.c` 接线

- `app_main`：`radar_diag_init()` 最先调用（越早捕获日志越好）→ 其余不变 → `radar_csi_init()` 在 `radar_espnow_init()` 之后 → **删除** `radar_ble_init()` 的无条件调用（改由 rf_sched） → `radar_audio_cue_init()` → `rf_sched_init()`。
- `poll_tick`：开头 `rf_sched_tick(now)`；`run_fusion()` 组装 `radar_level_features_t` 并调用 `radar_level_update`，再调 `radar_fusion_update_v3(&ftm, local, peer, tx_ref, level, alive)`；结果直接交 UI（二期在这一行之后插入 `radar_obstruct_*`，一期不动）。
- 按键：`RADAR_PAGE_RESULT` 下 OK 短按 → `rf_sched_request(RF_HOMING)`；HOMING 下 OK 短按 → 确认并回 MAIN；`DOWN DOUBLE` → 诊断页。

---

## 4. 参数总表

| 常量 | 值 | 范围 | 依据 |
| --- | --- | --- | --- |
| `RADAR_SWEEP_DURATION_MS` | 6000 | 4000~8000 | 更慢更匀；样本 ≥ 200 |
| 扫盘 ESP-NOW 周期 | 25 ms | 20~50 | 40 Hz；空口占用 <5% |
| `FTM_FRAMES_PER_SESSION` | 8 | 4~16 | 每会话 <300 ms |
| `FTM_MIN_PEER_RSSI` | −72 dBm | −75~−65 | 文献：< −70 失败率陡增 |
| `FTM_HISTORY` | 5 | 3~8 | P25 需 ≥4 个样本才有意义 |
| FTM 偏移钳位 | −2~15 m | — | 超出说明标定环境错误 |
| `RADAR_TWO_RAY_BREAK_M` | 45 | 40~50 | h=1.2 m |
| `RADAR_FAR_N` | 4.0 | 3.5~4.5 | 两射线远场 |
| 等级驻留 / 候选连续 | 10 s / 3 s | — | 防跳变 |
| 区间因子 e | L0 0.15 / L1 0.35 / L2 0.6 | — | 3.1 节误差预算 |
| 多观测量投票 `min_amp_db` / `max_res_db` | 4 / 3 dB | — | 低于 4 dB 峰不可信 |
| 镜像一致门限 | 30° | 20~45 | — |
| 扇区门限 spread | 45° | — | — |
| 节拍 | 8 拍 × 750 ms | — | 每拍 45° |
| 寻的超时 | 20 s | — | — |
| LOST 进入/退出 | 5 s / 10 s | — | — |
| 卡尔曼 q/r | 0.15 / 4.0 | 不改 | v1 已验证 |

---

## 5. NVS 与协议

| NVS key（命名空间 `radar`） | 类型 | 含义 |
| --- | --- | --- |
| `txref` | float blob | 1 m 对称 RSSI 参考（v2 已有） |
| `ftmoff` | float blob | FTM 偏移（米），新 |
| `calibts` | uint32 | 最近标定时间戳（诊断） |

协议：v3 复用 v2 包长；`v3_flags`/`env_level` 见 3.2.2。对端 `proto_ver==2` 时忽略这两个字节。

---

## 6. 内存、任务与优先级

| 项 | 大小 | 备注 |
| --- | --- | --- |
| RSSI 样本 256×16 B | 4 KB | 静态 |
| CSI 样本 256×24 B | 6 KB | 静态 |
| 诊断日志 32×96 B | 3 KB | 静态 |
| 音频任务栈 + PCM | 4 KB + 640 B | — |
| NimBLE | ~70~100 KB | 仅 LOST 时分配；`radar_ble_deinit` 必须真正释放（`nimble_port_deinit`） |
| 任务：`radar_cue`（prio 4）、`radar_ble_boot`（prio 3，一次性） | — | Wi-Fi 任务 prio 23、LVGL 任务由 esp_lvgl_port 管 |

每个里程碑结束在诊断页记录 `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)` 与 `heap_caps_get_largest_free_block`，写入 CHANGELOG。

---

## 7. 常见坑

1. **`esp_wifi_set_config(WIFI_IF_AP, ...)` 会重启 SoftAP**：只在 `radar_ftm_init` 调一次；不要在 tick 里反复调。
2. **`esp_wifi_ftm_get_report()` 的所有权**：成功或失败都要调用一次（`NULL,0` 释放），现有代码已处理，改动时保持。
3. **CSI 回调对所有帧触发**，不过滤 MAC 会被路由器信标淹没；且它运行在 Wi-Fi 任务里，`log10f` 可以、`ESP_LOGI` 不可以。
4. **AGC 归一化**：不要用 `Σ|H|²` 当功率（见 3.4）。
5. **令牌死锁**：两端都以为对方持有令牌 → 看门狗 3 s 由 MAC 小者取回；单测 U-FTM-07 必须覆盖。
6. **`radar_ble_init()` 耗时**：放工作任务；`deinit` 后再 `init` 要验证第二次成功（硬件指南 §14 "Wi-Fi/BLE 第二次进入失败"）。
7. **音频与 NVS 同时进行会有杂音**：标定结束提示音在 `nvs_commit` 之后再播。
8. **LVGL 锁**：`rf_sched_request` 从按键回调调用时已在锁内；从 `poll_tick` 调用时也在 LVGL 上下文；不要从 Wi-Fi 任务调用它。
9. **样本时间戳单调性检查**（`sweep_done` 已有）：40 Hz 下相邻样本可能同一毫秒，把 `<=` 改为 `<` 会误判；保持 `samples[i].timestamp_ms < samples[i-1].timestamp_ms` 才算异常。
10. **两射线模型**在室内不成立（无远场地面反射），仅当 level==L0 且 rssi 低于 45 m 折点时启用；L1/L2 保持单一 n。

---

## 8. 每个里程碑的完成定义（DoD）

- 代码：`./tools/validate.sh --static` 通过（含新增 host 测试）；`--firmware` 通过；`git diff` 只含本步范围。
- 文档：本文件对应小节若有参数改动则同步；CHANGELOG 记录 free heap。
- 实机：完成测试文档中标注为该里程碑的用例，并把结果表贴进 PR。

---

## 9. 参考资料

按本文各节用到的顺序分类；标注 “§x” 表示主要服务于哪一节。项目固定 ESP-IDF **5.5.3**、LVGL **9.5.0**（`dependencies.lock`），查阅 API 时以对应版本为准。

### 9.1 仓库内文档（优先读）

- [AI 硬件开发指南](../hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.zh_CN.md) —— LVGL 锁、无 PSRAM 内存约束、音频 close/open 与杂音（§8.1）、按键/长按约定（§0.1、§3.7、§6、§7）
- [产品规格](../hardware-design/specifications.zh_CN.md)
- [pass-radar 设计文档（v1/v2）](pass-radar-design.zh_CN.md) —— 现有线程模型与数据流
- [双卡实测指南与 v2.1 结果](pass-radar-device-test-guide.zh_CN.md) —— 一期要修复的现象来源
- [精度目标评估与 v3 设计方案](pass-radar-定向测距精度目标评估与v3设计方案.md) —— 每个设计决定的理由
- `official-main/main/demo_audio.c`、`components/bsp/include/bsp_audio.h` —— 音频任务写法的参照（§3.7）

### 9.2 ESP-IDF 5.5 API 与指南（ESP32-C3）

- Wi-Fi API 参考（`esp_wifi_set_csi*`、`esp_wifi_ftm_*`、`esp_wifi_set_channel`、`wifi_ftm_report_entry_t`）：https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-reference/network/esp_wifi.html （§3.3、§3.4）
- Wi-Fi 驱动指南（FTM Initiator/Responder、APSTA 信道约束）：https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-guides/wifi.html （§3.2、§3.3）
- Wi-Fi 厂商特性（CSI 数据布局、LLTF/HT-LTF 子载波、C3 `first_word_invalid`）：https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-guides/wifi-driver/wifi-vendor-features.html （§3.4）
- FTM 示例（偏移标定 `-o`、− 70 dBm 建议、状态码）：https://github.com/espressif/esp-idf/tree/master/examples/wifi/ftm （§3.3）
- CSI 示例与 ESP-CSI 项目：https://github.com/espressif/esp-idf/tree/master/examples/wifi/wifi_csi ，https://github.com/espressif/esp-csi （§3.4）
- ESP-NOW API（`esp_now_set_peer_rate_config`、接收回调上下文）：https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-reference/network/esp_now.html （§3.2）
- RF 共存（Wi-Fi/BLE 时分、Coded PHY 时隙选项）：https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-guides/coexist.html （§1.2、§3.8）
- NimBLE Host（`nimble_port_init/deinit`，惰性启停）：https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-reference/bluetooth/nimble/index.html （§3.8）
- BLE 特性支持表（C3 无 AoA/AoD）：https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-guides/ble/ble-feature-support-status.html
- 日志库（`esp_log_set_vprintf`）：https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-reference/system/log.html （§3.1）
- ESP Timer（`esp_timer_start_periodic`、回调上下文）：https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-reference/system/esp_timer.html （§3.2）
- 事件循环（`WIFI_EVENT_FTM_REPORT` 处理任务）：https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-reference/system/esp_event.html （§3.3）
- NVS（`nvs_set_blob/nvs_get_blob`）：https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-reference/storage/nvs_flash.html （§5）
- 堆内存（`heap_caps_get_free_size`、`heap_caps_get_largest_free_block`）：https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-reference/system/mem_alloc.html （§6）
- FreeRTOS（任务/队列/临界区）：https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-reference/system/freertos.html （§3.1、§3.7）
- I2S IRAM-safe（音频与 Flash 写并发）：https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32c3/api-reference/peripherals/i2s.html#iram-safe （§3.7）

### 9.3 LVGL 9.5 与组件

- LVGL 9.5 文档首页：https://lvgl.io/docs/open/9.5/
- Arc 控件（扇区渲染）：https://lvgl.io/docs/open/9.5/widgets/arc （§3.9）
- Line 控件（扫盘指针/射线）：https://lvgl.io/docs/open/9.5/widgets/line （§3.9）
- 内置字体与字形范围（Montserrat 无 `±`/`≤`/CJK）：https://lvgl.io/docs/open/9.5/main-modules/fonts/built_in_fonts （§3.1、§3.9）
- esp_lvgl_port（LVGL 任务与锁）：https://github.com/espressif/esp-bsp/tree/master/components/esp_lvgl_port
- espressif/button（CLICK/DOUBLE/LONG 事件语义）：https://components.espressif.com/components/espressif/button

### 9.4 算法与信号处理

- 对数距离路径损耗模型：https://en.wikipedia.org/wiki/Log-distance_path_loss_model （§3.6）
- 两射线地面反射模型与折点：https://en.wikipedia.org/wiki/Two-ray_ground-reflection_model （§3.6.2）
- 环形均值/环形统计：https://en.wikipedia.org/wiki/Circular_mean （§3.5）
- 抛物线（二次）峰值插值：J. O. Smith III, *Spectral Audio Signal Processing*, https://ccrma.stanford.edu/~jos/sasp/Quadratic_Interpolation_Spectral_Peaks.html （§3.5 回退路径）
- 一次谐波最小二乘拟合（线性最小二乘与法方程）：https://en.wikipedia.org/wiki/Linear_least_squares （§3.5）
- 一维卡尔曼滤波：https://en.wikipedia.org/wiki/Kalman_filter （现有 `radar_kalman`）
- 中值与四分位距（鲁棒统计）：https://en.wikipedia.org/wiki/Interquartile_range （§3.3）
- Fine Time Measurement for the IoT: A Practical Approach Using ESP32（FTM 偏移标定、LOS ±1 m）：https://arxiv.org/html/2401.16517v1 （§3.3）
- Benchmarking Wi-Fi FTM for IoT Devices：https://arxiv.org/pdf/2303.03766 （§3.3）
- ESPARGOS（ESP32 CSI 幅度 AGC 归一化与相位 STO/CFO/SFO 问题）：https://arxiv.org/html/2502.09405v1 （§3.4）
- Fundamental limits in RSSI-based direction-of-arrival estimation：https://www.researchgate.net/publication/312569085_Fundamental_limits_in_RSSI-based_direction-of-arrival_estimation （§3.5）
- ARREST：RSSI-based mobile sensing and tracking（旋转天线测向）：https://anrg.usc.edu/www/wp-content/uploads/2019/03/ARREST-FINAL.pdf （§3.5）
- 人体遮挡对 2.4 GHz 的影响：https://pmc.ncbi.nlm.nih.gov/articles/PMC6211019/ （§3.5 峰显著性门限依据）

### 9.5 硬件数据手册

- ESP32-C3 数据手册（射频灵敏度、GPIO、FTM/BLE 能力）：https://www.espressif.com/sites/default/files/documentation/esp32-c3_datasheet_en.pdf
- ESP32-C3 技术参考手册：https://www.espressif.com/sites/default/files/documentation/esp32-c3_technical_reference_manual_en.pdf
- Espressif 确认 C3 硬件不支持 AoA/AoD：https://github.com/espressif/esp-idf/issues/15953
