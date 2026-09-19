# pass-radar v3.1（二期）：15~100 m 远距与遮挡 —— 实施指南

> 目标读者：初级固件工程师
> 日期：2026-09-19
> 定位：**独立于一期（v3）的第二期工作**。一期解决 15 m 内的测距/定向与诚实降级；二期解决"两卡很远（15~100 m）或中间有树丛、围墙"时的识别与修正。
> 上游文档：
> - [远距与遮挡场景分析](pass-radar-v3-远距与遮挡场景分析.md) —— 物理背景、特征与修正的"为什么"
> - [v3 详细设计与实现指南](pass-radar-v3-详细设计与实现指南.md) —— 一期模块、函数名、常量的权威来源
> - [测试设计与用例](pass-radar-v3-测试设计与用例.md) —— 本期对应用例 U-OBS-*、D-16~D-20、F-03/F-11~F-14
> 本文只讲"怎么写"。

---

## 0. 启动条件与范围

### 0.1 前置条件（一期必须已交付）

| 一期交付物 | 二期为什么依赖它 |
| --- | --- |
| `radar_fusion_update_v3()` 输出 `dist_lo_m/dist_hi_m/level/obstructed/upper_bound_only/ftm_dist_m/rssi_dist_m` | 二期修正就是就地改写这些字段 |
| 环境等级 `radar_level_t`（L0~L3）与滞回 | 分类决策表的输入之一 |
| 两射线分段模型 `radar_fusion_sym_distance()`（折点 45 m） | 二期"举高模式"只是切换折点 |
| OOR 语义 = "5 s 无包"，RSSI < −88 仅降级 | 否则远距根本到不了 100 m |
| ESP-NOW 速率固定 1 Mbps | 同上 |
| FTM 偏移标定 + P25 窗口 + 令牌 | 二期 FTM 探针复用同一会话流程 |
| `radar_csi_recent_selectivity_db()` | 分类特征 F4 |
| `radar_diag` 日志与计数器、诊断页 | 外场阈值校准全靠它 |
| 双端扫盘镜像判断（`sweep_done` 中的 peer_angle 处理） | 分类特征 F5 |
| 一期 M3 外场验收通过（F-01、F-04、F-09） | 二期在一期基线之上做 A/B |

### 0.2 二期目标（可验证的承诺）

| 场景 | 承诺 |
| --- | --- |
| LOS 45~100 m | 距离 p50 ≤25%，不 OOR；举高后余量 +3 dB 以上 |
| 树丛 3~10 m 深 | 识别率 ≥70%（有风）；P80 修正后距离误差优于均值法；方向以扇区或两次一致射线显示 |
| 砖墙/围墙 ≤25 m | FTM 探针成功时距离误差 ≤3 m 并标 `BLOCKED?` |
| 砖墙/围墙 >25 m、金属栏 | 只显示上界 `<= X m`，上界包含真值 ≥90%；方向扇区 |
| 所有场景 | 屏幕给出下一步提示（举高 / 走 5 m / 再扫一次 / 请对方别动） |

### 0.3 本期改动一览

```
main/
  radar_obstruct.c/.h  新：特征（K 因子、超额损耗、双时标起伏、上分位）、分类、滞回、修正、UI 文案
  radar_espnow.c/.h    改：原始 RSSI 环形缓冲 + 读取接口；HOLD_STILL 位的收发
  radar_ftm.c          改：探针模式（−72~−82 dBm，每 5 s，4 帧）
  radar_fusion.c/.h    改：radar_fusion_set_breakpoint_m()、radar_fusion_env_of_level()
  radar_diag.h         改：新增 DIAG_CNT_FTM_PROBE / DIAG_CNT_FTM_PROBE_OK
  radar_ui.c/.h        改：类别标签、提示行、举高窗口显示、结果页扇区+提示
  main.c               改：特征装配、分类滞回、apply 接入、举高模式、步行测试、重复扫盘共识
tests/
  test_radar_obstruct.c 新（U-OBS-01~14）
```

---

## 1. 实施顺序

| 步 | 内容 | 文件 | 验收 |
| --- | --- | --- | --- |
| P1 | 原始 RSSI 环形缓冲 + 读取接口 | `radar_espnow.c/.h` | 诊断页原始样本数 ≥150；host 无需测试 |
| P2 | `radar_obstruct.c/.h` 特征、分类、滞回、修正、文案（纯逻辑） | 新文件、`tests/test_radar_obstruct.c`、`tools/validate.sh` | `--static` 通过 U-OBS-01~14 |
| P3 | 接入 `main.c`：装配特征 → 分类 → apply → UI | `main.c`、`radar_fusion.c`、`radar_ui.c` | D-16（人在中间来回走 → `FOLIAGE?`） |
| P4 | FTM 探针 | `radar_ftm.c`、`radar_diag.h` | D-17（25 m 处 probe 成功率 ≥20%） |
| P5 | 举高模式 + 步行测试 | `main.c`、`radar_fusion.c`、`radar_ui.c` | D-18、D-19 |
| P6 | 重复扫盘共识 + 对端"请别动" | `main.c`、`radar_espnow.c`、`radar_ui.c` | D-20 |
| P7 | 外场校准阈值并回写 §5 参数表 | `radar_obstruct.c` | F-03、F-11~F-14 |

---

## 2. P1：原始 RSSI 环形缓冲（`radar_espnow.c`）

遮挡判别靠的是"信号有多抖"，而卡尔曼滤波已经把抖动滤掉了，所以必须存**原始整数 RSSI**。

```c
// radar_espnow.h
#define RADAR_RAW_RING 256                 // ≥ 10 s × 20 Hz；扫盘 40 Hz 时覆盖 6.4 s，够用
// 拷出最近的原始 RSSI（dBm）与它们的时间戳（ms，esp_timer 基），按时间从旧到新排列。
// 返回实际个数。只能在 LVGL 任务调（非重入）。
unsigned radar_espnow_raw_history(float *out_dbm, uint32_t *out_ts_ms, unsigned max);
```

```c
// radar_espnow.c
static volatile int8_t   s_raw_ring[RADAR_RAW_RING];
static volatile uint32_t s_raw_ts[RADAR_RAW_RING];
static volatile unsigned s_raw_head;     // 下一个写入位置
static volatile unsigned s_raw_total;    // 累计写入数（判断是否已绕圈）

// 在 espnow_recv_cb 里，紧跟 s_raw_rssi = raw; 之后加：
unsigned h = s_raw_head;
s_raw_ring[h] = (int8_t)raw; s_raw_ts[h] = (uint32_t)(now / 1000);
s_raw_head = (h + 1) % RADAR_RAW_RING; s_raw_total++;

unsigned radar_espnow_raw_history(float *out_dbm, uint32_t *out_ts_ms, unsigned max) {
    unsigned total = s_raw_total, head = s_raw_head;      // 先快照，再拷贝
    unsigned n = total < RADAR_RAW_RING ? total : RADAR_RAW_RING;
    if (n > max) n = max;
    unsigned start = (head + RADAR_RAW_RING - n) % RADAR_RAW_RING;
    for (unsigned i = 0; i < n; ++i) {
        unsigned k = (start + i) % RADAR_RAW_RING;
        out_dbm[i] = (float)s_raw_ring[k];
        out_ts_ms[i] = s_raw_ts[k];
    }
    return n;
}
```

并发说明：Wi-Fi 任务在写、LVGL 任务在读，最多 1 个样本被撕，对统计量无影响，**不要加锁**（回调里不能阻塞）。

---

## 3. P2：`radar_obstruct` 模块（纯逻辑）

### 3.1 头文件（完整）

```c
#pragma once
// 远距/遮挡识别与修正 —— 纯逻辑，无 ESP-IDF 依赖，可 host test。
#include "radar_fusion.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    OBS_NONE_LOS = 0,        // 近距视距，无事
    OBS_FAR_LOS,             // 真的很远，但无遮挡
    OBS_ATTENUATING_STATIC,  // 静态衰减：墙/栅栏/对端背身
    OBS_FOLIAGE_DYNAMIC,     // 动态散射：风中树丛/人流
    OBS_REFLECTION,          // 信号走的是反射路径
    OBS_UNKNOWN
} radar_obstruct_class_t;

typedef enum {
    HINT_NONE = 0,
    HINT_RAISE_CARD,         // "RAISE CARD 3s"
    HINT_WALK_TEST,          // "WALK 5m THEN DOWN"
    HINT_PEER_STAND_STILL,   // "PEER: HOLD STILL"
    HINT_SWEEP_AGAIN         // "SWEEP AGAIN"
} radar_hint_t;

typedef struct {
    float excess_loss_db;    // F1：RSSI 比 FTM 应有值低了多少 dB；无 FTM = NAN
    float kfactor_db;        // F2：Rician K；样本不足 = NAN
    float p2p_1s_db;         // F3：最近 1 s 原始 RSSI 峰峰值
    float p2p_10s_db;        // F3：最近 10 s 各 1 s 均值的峰峰值
    float csi_sel_db;        // F4：无 CSI = NAN
    float mirror_err_deg;    // F5：无双端结果 = NAN
    float mean_rssi_dbm;     // 最近 8 s 原始 RSSI 均值
    radar_level_t level;
} radar_obstruct_features_t;

// ---- 特征 ----
float radar_obstruct_kfactor_db(const float *rssi_dbm, unsigned n);          // n<20 → NAN；瑞利或更差 → -10
float radar_obstruct_excess_loss_db(float d_rssi_m, float d_ftm_m, float path_loss_n); // 任一 ≤0 → NAN
float radar_obstruct_upper_quantile(const float *v, unsigned n, float q);    // n<10 → NAN；不修改输入
void  radar_obstruct_p2p(const float *rssi_dbm, const uint32_t *ts_ms, unsigned n,
                         uint32_t now_ms, float *out_p2p_1s, float *out_p2p_10s);
float radar_obstruct_mean_recent(const float *rssi_dbm, const uint32_t *ts_ms, unsigned n,
                                 uint32_t now_ms, uint32_t window_ms);       // 无样本 → NAN

// ---- 分类（纯函数，决策表见远距文档 §3 F7）----
radar_obstruct_class_t radar_obstruct_classify(const radar_obstruct_features_t *f);

// ---- 分类滞回：新类别连续出现 hold_ticks 次才接受 ----
typedef struct { radar_obstruct_class_t cur, pending; unsigned pending_count; } radar_obstruct_hold_t;
void radar_obstruct_hold_init(radar_obstruct_hold_t *h);
radar_obstruct_class_t radar_obstruct_hold_update(radar_obstruct_hold_t *h,
                                                  radar_obstruct_class_t observed, unsigned hold_ticks);

// ---- 修正（就地改 r；返回建议提示）----
// rssi_10s/n：最近 10 s 原始 RSSI（供 P80）；tx_ref/env：路径损耗参数；
// sweep_spread_deg：可为 NULL；反射类别下会被改为 90。
radar_hint_t radar_obstruct_apply(radar_obstruct_class_t cls, const radar_obstruct_features_t *f,
                                  const float *rssi_10s, unsigned n, float tx_ref, radar_env_t env,
                                  radar_fusion_result_t *r, float *sweep_spread_deg);

// ---- UI 文案（屏幕文字按仓库约定为英文）----
const char *radar_obstruct_class_label(radar_obstruct_class_t c);  // "", "FAR", "BLOCKED?", "FOLIAGE?", "REFLECT?", ""
const char *radar_hint_label(radar_hint_t h);                     // "", "RAISE CARD 3s", "WALK 5m THEN DOWN", "PEER: HOLD STILL", "SWEEP AGAIN"
```

### 3.2 实现（完整）

常量集中在文件顶部；首版值见 §5，**外场 F-OBS 测试后要回来改**。

```c
#include "radar_obstruct.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define OBS_EXCESS_MIN_DB        6.0f
#define OBS_REFLECT_EXCESS_DB   (-3.0f)
#define OBS_K_FOLIAGE_DB         2.0f
#define OBS_K_STATIC_DB          4.0f
#define OBS_P2P1S_DYNAMIC_DB     8.0f
#define OBS_P2P1S_STATIC_DB      5.0f
#define OBS_P2P10S_DYNAMIC_DB    6.0f
#define OBS_SEL_LOS_MAX_DB       5.0f
#define OBS_MIRROR_REFLECT_DEG  45.0f
#define OBS_FAR_RSSI_DBM       (-80.0f)
#define OBS_RAISE_HINT_RSSI    (-75.0f)
#define OBS_UPPER_QUANTILE       0.8f
#define OBS_FOLIAGE_INTERVAL_E   0.5f
#define OBS_BLOCKED_INTERVAL_E   0.6f
#define OBS_FTM_BAND_M           1.5f   // 墙后 FTM 点值的 ± 区间

// ---------- 特征 ----------
float radar_obstruct_kfactor_db(const float *x, unsigned n) {
    if (!x || n < 20) return NAN;
    double mean = 0, m2 = 0;
    for (unsigned i = 0; i < n; ++i) {
        double p = pow(10.0, x[i] / 10.0);          // dBm → 线性功率
        mean += p; m2 += p * p;
    }
    mean /= n; m2 /= n;
    double var = m2 - mean * mean;
    if (mean <= 0) return NAN;
    double gamma = var / (mean * mean);
    if (gamma >= 1.0) return -10.0f;                  // 比瑞利还差：当作 −10 dB
    double s = sqrt(1.0 - gamma);
    double K = s / (1.0 - s + 1e-9);
    return (float)(10.0 * log10(K + 1e-9));
}

float radar_obstruct_excess_loss_db(float d_rssi, float d_ftm, float n) {
    if (!(d_rssi > 0.0f) || !(d_ftm > 0.0f) || !(n > 0.0f)) return NAN;
    return 10.0f * n * log10f(d_rssi / d_ftm);
}

static int cmp_float(const void *a, const void *b) {
    float fa = *(const float *)a, fb = *(const float *)b;
    return (fa > fb) - (fa < fb);
}

float radar_obstruct_upper_quantile(const float *v, unsigned n, float q) {
    if (!v || n < 10 || !(q > 0.0f && q < 1.0f)) return NAN;
    float tmp[256];
    if (n > 256) n = 256;
    memcpy(tmp, v, n * sizeof(float));
    qsort(tmp, n, sizeof(float), cmp_float);
    unsigned idx = (unsigned)(q * (float)(n - 1) + 0.5f);
    return tmp[idx];
}

void radar_obstruct_p2p(const float *x, const uint32_t *ts, unsigned n, uint32_t now,
                        float *p1, float *p10) {
    float mn1 = 1e9f, mx1 = -1e9f; unsigned c1 = 0;
    float bucket_sum[10] = {0}; unsigned bucket_cnt[10] = {0};
    for (unsigned i = 0; i < n; ++i) {
        uint32_t age = now - ts[i];                       // ms，无符号回绕安全
        if (age < 1000) { if (x[i] < mn1) mn1 = x[i]; if (x[i] > mx1) mx1 = x[i]; c1++; }
        if (age < 10000) { unsigned b = age / 1000; bucket_sum[b] += x[i]; bucket_cnt[b]++; }
    }
    if (p1) *p1 = c1 >= 5 ? mx1 - mn1 : NAN;
    float mnb = 1e9f, mxb = -1e9f; unsigned nb = 0;
    for (unsigned b = 0; b < 10; ++b) {
        if (bucket_cnt[b] < 3) continue;
        float m = bucket_sum[b] / (float)bucket_cnt[b];
        if (m < mnb) mnb = m;
        if (m > mxb) mxb = m;
        nb++;
    }
    if (p10) *p10 = nb >= 5 ? mxb - mnb : NAN;
}

float radar_obstruct_mean_recent(const float *x, const uint32_t *ts, unsigned n,
                                 uint32_t now, uint32_t window_ms) {
    double s = 0; unsigned c = 0;
    for (unsigned i = 0; i < n; ++i) if (now - ts[i] < window_ms) { s += x[i]; c++; }
    return c ? (float)(s / c) : NAN;
}

// ---------- 分类 ----------
radar_obstruct_class_t radar_obstruct_classify(const radar_obstruct_features_t *f) {
    if (!f) return OBS_UNKNOWN;
    bool has_ex = !isnan(f->excess_loss_db);
    bool has_k  = !isnan(f->kfactor_db);
    bool has_p1 = !isnan(f->p2p_1s_db), has_p10 = !isnan(f->p2p_10s_db);
    bool has_sel = !isnan(f->csi_sel_db), has_mir = !isnan(f->mirror_err_deg);

    // 1. 反射：FTM 比 RSSI 还远，或双端不镜像
    if ((has_ex && f->excess_loss_db < OBS_REFLECT_EXCESS_DB) ||
        (has_mir && f->mirror_err_deg > OBS_MIRROR_REFLECT_DEG)) return OBS_REFLECTION;
    // 2. 动态散射：短时抖得厉害，且（K 低 或 慢变也大）
    if (has_p1 && f->p2p_1s_db >= OBS_P2P1S_DYNAMIC_DB &&
        ((has_k && f->kfactor_db < OBS_K_FOLIAGE_DB) ||
         (has_p10 && f->p2p_10s_db >= OBS_P2P10S_DYNAMIC_DB))) return OBS_FOLIAGE_DYNAMIC;
    // 3. 静态衰减（有 FTM 证据）
    if (has_ex && f->excess_loss_db > OBS_EXCESS_MIN_DB &&
        has_p1 && f->p2p_1s_db < OBS_P2P1S_STATIC_DB) return OBS_ATTENUATING_STATIC;
    // 4. 静态衰减（无 FTM，弱推断：很远且出奇地稳）
    if (!has_ex && f->level == RADAR_LEVEL_L2_NLOS &&
        has_p1 && f->p2p_1s_db < OBS_P2P1S_STATIC_DB &&
        has_k && f->kfactor_db > OBS_K_STATIC_DB) return OBS_ATTENUATING_STATIC;
    // 5. 真远 LOS
    if (f->mean_rssi_dbm < OBS_FAR_RSSI_DBM &&
        (!has_p1 || f->p2p_1s_db < OBS_P2P1S_DYNAMIC_DB) &&
        (!has_k || f->kfactor_db > OBS_K_STATIC_DB) &&
        (!has_sel || f->csi_sel_db < OBS_SEL_LOS_MAX_DB)) return OBS_FAR_LOS;
    // 6. 近距且没有任何异常
    if (f->level == RADAR_LEVEL_L0_LOS) return OBS_NONE_LOS;
    return OBS_UNKNOWN;
}

void radar_obstruct_hold_init(radar_obstruct_hold_t *h) {
    h->cur = OBS_NONE_LOS; h->pending = OBS_NONE_LOS; h->pending_count = 0;
}
radar_obstruct_class_t radar_obstruct_hold_update(radar_obstruct_hold_t *h,
                                                  radar_obstruct_class_t obs, unsigned hold) {
    if (obs == h->cur) { h->pending_count = 0; return h->cur; }
    if (obs != h->pending) { h->pending = obs; h->pending_count = 1; return h->cur; }
    if (++h->pending_count >= hold) { h->cur = obs; h->pending_count = 0; }
    return h->cur;
}

// ---------- 修正 ----------
radar_hint_t radar_obstruct_apply(radar_obstruct_class_t cls, const radar_obstruct_features_t *f,
                                  const float *rssi_10s, unsigned n, float tx_ref, radar_env_t env,
                                  radar_fusion_result_t *r, float *spread) {
    if (!r || !f) return HINT_NONE;
    if (r->conf == RADAR_CONF_OOR) return HINT_NONE;
    bool ftm_ok = r->ftm_dist_m > 0.0f;                 // 一期 fusion 仅在已标定且新鲜时填此字段
    bool far = f->mean_rssi_dbm < OBS_RAISE_HINT_RSSI;

    switch (cls) {
    case OBS_FOLIAGE_DYNAMIC: {
        // C3：衰落只往下打 → 用 P80 代替均值。修正量 = P80 − 均值（≥0），加到对称 RSSI 上。
        float p80 = radar_obstruct_upper_quantile(rssi_10s, n, OBS_UPPER_QUANTILE);
        float mean = NAN;
        if (n >= 10) { double s = 0; for (unsigned i = 0; i < n; ++i) s += rssi_10s[i]; mean = (float)(s / n); }
        if (!isnan(p80) && !isnan(mean) && !ftm_ok) {
            float delta = p80 - mean; if (delta < 0) delta = 0;
            r->distance_m = radar_fusion_sym_distance(r->sym_rssi_dbm + delta, tx_ref, env);
        }
        r->dist_lo_m = r->distance_m / (1.0f + OBS_FOLIAGE_INTERVAL_E);
        r->dist_hi_m = r->distance_m * (1.0f + OBS_FOLIAGE_INTERVAL_E);
        r->obstructed = true;
        if (r->conf > RADAR_CONF_MED) r->conf = RADAR_CONF_MED;
        return far ? HINT_RAISE_CARD : HINT_NONE;
    }
    case OBS_ATTENUATING_STATIC:
        r->obstructed = true;
        if (ftm_ok) {                                   // C4：FTM 不受衰减影响，直接用
            r->distance_m = r->ftm_dist_m;
            r->dist_lo_m = r->ftm_dist_m - OBS_FTM_BAND_M; if (r->dist_lo_m < 0.1f) r->dist_lo_m = 0.1f;
            r->dist_hi_m = r->ftm_dist_m + OBS_FTM_BAND_M;
            r->upper_bound_only = false;
            if (r->conf > RADAR_CONF_MED) r->conf = RADAR_CONF_MED;
            return far ? HINT_RAISE_CARD : HINT_NONE;
        }
        // C5：没有 FTM 就只能给上界
        r->dist_hi_m = r->rssi_dist_m * (1.0f + OBS_BLOCKED_INTERVAL_E);
        r->distance_m = r->dist_hi_m;
        r->dist_lo_m = 0.0f;
        r->upper_bound_only = true;
        r->conf = RADAR_CONF_LOW;
        return f->level == RADAR_LEVEL_L2_NLOS ? HINT_WALK_TEST : HINT_RAISE_CARD;
    case OBS_REFLECTION:                                // C6
        if (spread) *spread = 90.0f;
        r->distance_m = (ftm_ok && r->ftm_dist_m < r->rssi_dist_m) ? r->ftm_dist_m : r->rssi_dist_m;
        r->dist_lo_m = r->distance_m / 2.0f; r->dist_hi_m = r->distance_m * 2.0f;
        r->obstructed = true;
        r->conf = RADAR_CONF_LOW;
        return HINT_SWEEP_AGAIN;
    case OBS_FAR_LOS:
        return f->mean_rssi_dbm < OBS_FAR_RSSI_DBM ? HINT_RAISE_CARD : HINT_NONE;
    default:
        return HINT_NONE;
    }
}

const char *radar_obstruct_class_label(radar_obstruct_class_t c) {
    switch (c) {
    case OBS_FAR_LOS: return "FAR";
    case OBS_ATTENUATING_STATIC: return "BLOCKED?";
    case OBS_FOLIAGE_DYNAMIC: return "FOLIAGE?";
    case OBS_REFLECTION: return "REFLECT?";
    default: return "";
    }
}
const char *radar_hint_label(radar_hint_t h) {
    switch (h) {
    case HINT_RAISE_CARD: return "RAISE CARD 3s";
    case HINT_WALK_TEST: return "WALK 5m THEN DOWN";
    case HINT_PEER_STAND_STILL: return "PEER: HOLD STILL";
    case HINT_SWEEP_AGAIN: return "SWEEP AGAIN";
    default: return "";
    }
}
```

### 3.3 host 测试

`tests/test_radar_obstruct.c` 实现测试文档 U-OBS-01~14，并在 `tools/validate.sh` 的 `run_static_checks()` 登记：

```bash
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
    tests/test_radar_obstruct.c main/radar_obstruct.c main/radar_fusion.c main/radar_sweep.c \
    -o "${test_dir}/test_radar_obstruct" -lm
"${test_dir}/test_radar_obstruct"
```

合成瑞利/莱斯序列（U-OBS-01/02）：莱斯 = `10·log10(|A + (g1 + j·g2)·σ|²)`，A 为直射幅度，g 为 Box–Muller 高斯，`K = A²/(2σ²)`；瑞利即 A=0。用固定种子 LCG 保证可重复。

---

## 4. P3：接入 `main.c`

### 4.1 新增状态与特征装配

```c
static radar_obstruct_hold_t s_obs_hold;
static radar_obstruct_class_t s_obs_class = OBS_NONE_LOS;
static radar_hint_t s_hint = HINT_NONE;
static radar_obstruct_features_t s_last_features;   // 供诊断页/举高/步行测试
static float s_last_mirror_err_deg = NAN;            // sweep_done 里填
#define OBS_HOLD_TICKS 20                            // 2 s 连续一致才切类别

static radar_obstruct_features_t build_obstruct_features(const radar_fusion_result_t *fr,
                                                         radar_level_t level,
                                                         float *rssi_buf, uint32_t *ts_buf,
                                                         unsigned *out_n) {
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    unsigned n = radar_espnow_raw_history(rssi_buf, ts_buf, RADAR_RAW_RING);
    *out_n = n;
    unsigned k0 = n;                                  // K 因子只用最近 4 s
    for (unsigned i = 0; i < n; ++i) if (now - ts_buf[i] < 4000) { k0 = i; break; }
    radar_obstruct_features_t f = {0};
    f.kfactor_db = k0 < n ? radar_obstruct_kfactor_db(rssi_buf + k0, n - k0) : NAN;
    radar_obstruct_p2p(rssi_buf, ts_buf, n, now, &f.p2p_1s_db, &f.p2p_10s_db);
    f.mean_rssi_dbm = radar_obstruct_mean_recent(rssi_buf, ts_buf, n, now, 8000);
    float n_pl = radar_env_path_loss_n(radar_fusion_env_of_level(level));
    f.excess_loss_db = fr->ftm_dist_m > 0.0f
        ? radar_obstruct_excess_loss_db(fr->rssi_dist_m, fr->ftm_dist_m, n_pl) : NAN;
    f.csi_sel_db = radar_csi_recent_selectivity_db();     // 无 CSI 时返回 NAN
    f.mirror_err_deg = s_last_mirror_err_deg;
    f.level = level;
    return f;
}
```

`radar_fusion_env_of_level()` 是加在 `radar_fusion.c` 的小查表：L0→OUTDOOR(2.2)、L1→INDOOR(3.0)、L2/L3→OBSTRUCTED(3.5)。

### 4.2 `run_fusion()` 末尾

```c
    radar_fusion_result_t fr = radar_fusion_update_v3(&ftm, local, peer, s_tx_ref_sym, level, alive);
    static float rssi_buf[RADAR_RAW_RING]; static uint32_t ts_buf[RADAR_RAW_RING];
    unsigned n = 0;
    radar_obstruct_features_t f = build_obstruct_features(&fr, level, rssi_buf, ts_buf, &n);
    radar_obstruct_class_t observed = radar_obstruct_classify(&f);
    s_obs_class = radar_obstruct_hold_update(&s_obs_hold, observed, OBS_HOLD_TICKS);
    s_hint = radar_obstruct_apply(s_obs_class, &f, rssi_buf, n, s_tx_ref_sym,
                                  radar_fusion_env_of_level(level), &fr, &s_last_vote_spread);
    apply_walk_cap(&fr);          // §6.2
    s_last_features = f;
    return fr;
```

因为用了 `static` 大数组，`run_fusion` 仍然只能从 LVGL 任务调用（一期已如此）。

### 4.3 `sweep_done()` 补一行（在双端镜像判断处）

```c
    s_last_mirror_err_deg = (angle >= 0 && peer_angle >= 0)
        ? radar_fusion_angle_diff(angle, fmodf(peer_angle + 180.0f, 360.0f)) : NAN;
```

### 4.4 UI（`radar_ui.c`）

屏幕文字一律英文（仓库约定，内置字体无 CJK，也无 `≤`，用 `<=`）。主页三行：

```
行 1  DIST  8.2 m (6.9~9.6)  [MED] L1               ← 常规
      DIST <= 19 m  [LOW] L2  BLOCKED?              ← upper_bound_only：只显示 "<= hi"
      DIST 12 m ~ (8~18)  [MED] L1  FOLIAGE?         ← obstructed 且非上界：点值后加 "~"
行 2  RSSI -81  FTM probe ok                         ← 探针成功时
行 3  RAISE CARD 3s / WALK 5m THEN DOWN / SWEEP AGAIN / PEER: HOLD STILL   ← s_hint，空则隐藏
```

`radar_ui_update_tracking()` 增加参数 `const char *class_label, const char *hint_label`；类别标签附在行 1 末尾，提示行为独立 `lv_label`，空串时 `lv_obj_add_flag(LV_OBJ_FLAG_HIDDEN)`。区间用 `snprintf("%.0f~%.0f")`，<10 m 用 `%.1f`。

结果页：`spread >= 45` 时不画射线，画 `lv_arc`（起止角 angle±spread）；底行显示 `radar_hint_label(s_hint)`。

---

## 5. P4：FTM 探针（`radar_ftm.c`）

目的：在 −72~−82 dBm（约 20~35 m）也偶尔试一次 FTM，成功一次就能给出 F1 超额损耗。失败不影响等级判定的成功率。

```c
#define FTM_PROBE_MIN_RSSI (-82)
#define FTM_PROBE_PERIOD_US (5 * 1000 * 1000)
#define FTM_PROBE_FRAMES 4
static bool s_probe_session;
static int64_t s_last_probe_us;

// radar_ftm_tick 中，在"常规发起"判断之后加：
int prssi = radar_espnow_peer_rssi();
bool regular = prssi >= FTM_MIN_PEER_RSSI;
bool probe   = !regular && prssi >= FTM_PROBE_MIN_RSSI && now - s_last_probe_us >= FTM_PROBE_PERIOD_US;
if (!regular && !probe) return;
cfg.frm_count = probe ? FTM_PROBE_FRAMES : FTM_FRAMES_PER_SESSION;
... esp_wifi_ftm_initiate_session(&cfg) ...
if (err == ESP_OK) {
    s_probe_session = probe;
    if (probe) s_last_probe_us = now;
    radar_diag_inc(probe ? DIAG_CNT_FTM_PROBE : DIAG_CNT_FTM_INITIATE);
}

// ftm_event_handler 中：
//   成功：radar_diag_inc(s_probe_session ? DIAG_CNT_FTM_PROBE_OK : DIAG_CNT_FTM_OK);
//         探针成功同样进入 P25 窗口与快照（它就是一次合法测量）
//   失败：若 s_probe_session，不写 s_backoff_until_us（探针失败是预期内的，不要拖慢常规节奏）
//   两种情况都把 s_probe_session 清零
```

`radar_diag.h` 计数器枚举新增 `DIAG_CNT_FTM_PROBE`、`DIAG_CNT_FTM_PROBE_OK`。一期的等级特征 `ftm_success_rate` 只用 `FTM_OK/FTM_INITIATE`，**不含探针**。

---

## 6. P5：举高模式与步行测试（`main.c`、`radar_fusion.c`）

### 6.1 举高模式（远距文档 C7）

举到 2 m 后两射线折点从 45 m 变 130 m；不换折点的话，举高后 RSSI 变强会被 n=4 模型错误地算成"距离大幅变近"。

```c
// radar_fusion.h
void radar_fusion_set_breakpoint_m(float d_break);      // 默认 RADAR_TWO_RAY_BREAK_M(45)
#define RADAR_TWO_RAY_BREAK_RAISED_M 130.0f              // h = 2.0 m

// main.c
static int64_t s_raised_until_ms = 0;
static float   s_raise_rssi_before = NAN;
// on_key，RADAR_PAGE_MAIN，UP 短按：
s_raised_until_ms = esp_timer_get_time() / 1000 + 5000;
s_raise_rssi_before = s_last_features.mean_rssi_dbm;      // 举高前基线，供显示增益
s_track_dist_m = RADAR_DIST_INVALID;                      // 不让两种姿态的读数混进同一个 IIR
// poll_tick 开头：
bool raised = (esp_timer_get_time() / 1000) < s_raised_until_ms;
radar_fusion_set_breakpoint_m(raised ? RADAR_TWO_RAY_BREAK_RAISED_M : RADAR_TWO_RAY_BREAK_M);
// 窗口结束那一拍再把 s_track_dist_m 置 INVALID 一次
// UI：raised 时行 3 显示 "RAISED  +%.0f dB"（最近 1 s 均值 − 基线）
```

### 6.2 步行测试（远距文档 C8）

原理：真 85 m 走 5 m RSSI 几乎不变（<1 dB）；真 30 m 走 5 m 变 >1.5 dB；真 15 m 变 >3 dB。

```c
typedef enum { WALK_IDLE, WALK_ARMED, WALK_DONE } walk_state_t;
static walk_state_t s_walk = WALK_IDLE;
static float s_walk_ref_dbm = NAN;
static int64_t s_walk_armed_ms = 0, s_walk_done_ms = 0, s_walk_cap_until_ms = 0;
#define WALK_DELTA_NEAR_DB 3.0f
#define WALK_CAP_M 25.0f
#define WALK_CAP_HOLD_MS (60 * 1000)
#define WALK_TIMEOUT_MS (60 * 1000)

// on_key，RADAR_PAGE_MAIN，DOWN 短按（仅当 s_hint == HINT_WALK_TEST 或 s_walk == WALK_ARMED）：
if (s_walk == WALK_IDLE) {
    s_walk_ref_dbm = s_last_features.mean_rssi_dbm;          // 8 s 均值做基线
    s_walk = WALK_ARMED; s_walk_armed_ms = now_ms;
    radar_ui_set_hint("WALK 5m TOWARD ARROW, THEN DOWN");
} else if (s_walk == WALK_ARMED) {
    float delta = radar_obstruct_mean_recent(rssi_buf, ts_buf, n, now_ms, 2000) - s_walk_ref_dbm;
    if (delta > WALK_DELTA_NEAR_DB) { s_walk_cap_until_ms = now_ms + WALK_CAP_HOLD_MS; radar_ui_set_hint("NEAR: <= 25 m"); }
    else                            { radar_ui_set_hint("REALLY FAR"); }
    s_walk = WALK_DONE; s_walk_done_ms = now_ms;
}
// poll_tick：ARMED 超过 WALK_TIMEOUT_MS → IDLE；DONE 5 s 后 → IDLE

static void apply_walk_cap(radar_fusion_result_t *r) {
    if (esp_timer_get_time() / 1000 >= s_walk_cap_until_ms) return;
    if (r->dist_hi_m > WALK_CAP_M) r->dist_hi_m = WALK_CAP_M;
    if (r->distance_m > WALK_CAP_M) r->distance_m = WALK_CAP_M;
}
```

按键冲突：步行测试的 DOWN 短按与诊断页入口（DOWN 双击）共用一键；button 组件在双击前也会先发 CLICK。所以只在 `s_hint == HINT_WALK_TEST || s_walk == WALK_ARMED` 时响应 DOWN 单击，其余时间 DOWN 单击无作用、双击进诊断。

---

## 7. P6：重复扫盘共识与"对端别动"

### 7.1 重复扫盘共识（远距文档 C9）

风中树丛的衰落周期与 6 s 扫盘同量级，单次扫盘可能拿到假峰；两次一致才画射线。

```c
static float   s_prev_sweep_angle = RADAR_ANGLE_INVALID;
static int64_t s_prev_sweep_ms = 0;
#define SWEEP_CONSENSUS_WINDOW_MS (60 * 1000)
#define SWEEP_CONSENSUS_DEG 30.0f

// sweep_done()，算完 angle/spread 之后、show_result 之前：
bool need_consensus = (s_obs_class == OBS_FOLIAGE_DYNAMIC || s_obs_class == OBS_REFLECTION ||
                       level == RADAR_LEVEL_L2_NLOS);
if (need_consensus && angle >= 0.0f) {
    bool have_prev = s_prev_sweep_angle >= 0.0f && now_ms - s_prev_sweep_ms < SWEEP_CONSENSUS_WINDOW_MS;
    if (have_prev && radar_fusion_angle_diff(angle, s_prev_sweep_angle) <= SWEEP_CONSENSUS_DEG) {
        float pair[2] = {angle, s_prev_sweep_angle};
        angle = radar_fusion_circular_mean(pair, 2);
        if (spread < 30.0f) spread = 30.0f;
    } else {
        spread = 90.0f;                 // 扇区
        s_hint = HINT_SWEEP_AGAIN;
    }
}
s_prev_sweep_angle = angle; s_prev_sweep_ms = now_ms;
```

### 7.2 对端别动（远距文档 C10）

对端转身/走动会引入 6~15 dB 起伏。复用一期协议的 `RADAR_V3_FLAG_HOMING` 位：本端处于 SWEEP 或 HOMING 且 `level >= L1` 时在 `broadcast_tick` 置位；对端 `parse_v2_packet` 看到该位记录 `s_peer_wants_still_us = now`；对端主页 `poll_tick` 若 `now − s_peer_wants_still_us < 1 s`，提示行显示 `PEER: HOLD STILL`（优先级高于其他提示）。对端自己也在扫盘（双端同扫）时不显示。

---

## 8. 参数表（首版；P7 外场校准后回写）

| 常量 | 值 | 校准数据来源 |
| --- | --- | --- |
| `OBS_EXCESS_MIN_DB` | 6 | F-OBS-2 |
| `OBS_REFLECT_EXCESS_DB` | −3 | F-OBS-3 |
| `OBS_K_FOLIAGE_DB` / `OBS_K_STATIC_DB` | 2 / 4 | F-OBS-1 vs F-OBS-4 的 K 分布 |
| `OBS_P2P1S_DYNAMIC_DB` / `OBS_P2P1S_STATIC_DB` | 8 / 5 | 同上 |
| `OBS_P2P10S_DYNAMIC_DB` | 6 | F-OBS-1（有风/无风） |
| `OBS_SEL_LOS_MAX_DB` | 5 | F-OBS-4 |
| `OBS_MIRROR_REFLECT_DEG` | 45 | F-OBS-3 |
| `OBS_FAR_RSSI_DBM` / `OBS_RAISE_HINT_RSSI` | −80 / −75 | F-03 |
| `OBS_UPPER_QUANTILE` | 0.8 | F-OBS-1 |
| `OBS_FOLIAGE_INTERVAL_E` / `OBS_BLOCKED_INTERVAL_E` | 0.5 / 0.6 | 区间覆盖率 ≥90% |
| `OBS_FTM_BAND_M` | 1.5 | F-OBS-2 |
| `OBS_HOLD_TICKS` | 20（2 s） | — |
| `FTM_PROBE_MIN_RSSI` / `PERIOD` / `FRAMES` | −82 / 5 s / 4 | D-17 |
| `RADAR_TWO_RAY_BREAK_RAISED_M` | 130 | h=2 m |
| `WALK_DELTA_NEAR_DB` / `WALK_CAP_M` | 3 / 25 | D-19 |
| `SWEEP_CONSENSUS_DEG` / `WINDOW` | 30 / 60 s | F-OBS-1 |

---

## 9. 内存与任务

| 项 | 大小 | 备注 |
| --- | --- | --- |
| 原始 RSSI 环形 256×5 B | 1.3 KB | 静态 |
| `run_fusion` 静态工作区 256×8 B | 2 KB | 静态 |
| `upper_quantile` 栈内临时 256×4 B | 1 KB | LVGL 任务栈需 ≥ 6 KB（一期已满足则不变） |
| 无新任务 | — | 全部在 LVGL 任务 100 ms 轮询中完成；单次 `run_fusion` 额外耗时 <2 ms（K 因子 80 次 `pow`） |

---

## 10. 诊断页新增

第 1 屏追加一行：`OBS <label> ex=%.0f K=%.0f p1=%.0f p10=%.0f sel=%.0f mir=%.0f`（NAN 显示 `-`）与 `probe %lu/%lu`（成功/发起）。外场 F-OBS 测试就是抄这一行数字。

---

## 11. 机上用例（新增 D-16~D-20，补入测试文档 §4）

| ID | 步骤 | 通过标准 | 步 |
| --- | --- | --- | --- |
| D-16 类别切换 | 3 m 静止 30 s；再让一人在两卡中间来回走 30 s；离开 | 静止 `OBS_NONE_LOS`；走动期间 2~5 s 内变 `FOLIAGE?`（p1 ≥ 8）；离开后 2~5 s 回 NONE | P3 |
| D-17 探针 | 室外 25 m 静止 2 min | 诊断页 `probe` 发起 ≥20 次，成功率 ≥20%；成功后行 2 出现 `FTM probe ok` | P4 |
| D-18 举高 | 室外 40 m，UP 短按后举高 | 行 3 显示 `RAISED +x dB`，x ≥ 2；5 s 后恢复；距离读数不出现"突然变近一半" | P5 |
| D-19 步行 | 人为制造 `WALK 5m THEN DOWN`（隔墙 15 m）；DOWN → 走 5 m → DOWN | 提示 `NEAR: <= 25 m`；主页上界 ≤25 m 持续 60 s | P5 |
| D-20 共识 | 隔墙（L2）连续扫盘两次 | 第一次结果页扇区 + `SWEEP AGAIN`；第二次与第一次差 ≤30° 时变为射线 | P6 |

外场统计用例见测试文档 F-03、F-11~F-14 与远距文档 §7（F-OBS-1~4）。

---

## 12. 完成定义（DoD）

- `./tools/validate.sh --all` 通过（含 `test_radar_obstruct`）。
- D-16~D-20 通过；F-OBS-1~4 完成并据此回写 §8 参数表。
- 一期回归 R-03（F-01、F-04、F-09 抽样）无退化：近距 LOS 场景下类别恒为 `OBS_NONE_LOS`，距离/方向指标不变。
- 评估文档 §0 目标表中 15~100 m 行按实测更新。

---

## 13. 常见坑

1. **不要用卡尔曼后的 RSSI 算 K 因子或峰峰值**——它已经被平滑，所有遮挡特征会失效。
2. `radar_obstruct_apply` 依赖 `r->ftm_dist_m > 0` 表示"FTM 有效且已标定且新鲜"；若一期 fusion 在未标定时也填了该字段，墙后会误用未标定 FTM。先查一期实现。
3. 类别滞回（2 s）与等级滞回（3 s/10 s）是两套独立机制，不要合并；否则树丛下等级会跟着抖。
4. 举高窗口内外的距离读数不能共用 `s_track_dist_m` IIR，两处都要重置。
5. 步行测试基线用 8 s 均值、终值用 2 s 均值；若两者窗口相同，用户走得快时终值会被走动过程稀释。
6. `HINT_PEER_STAND_STILL` 优先级最高，其次 `SWEEP_AGAIN`，再 `WALK_TEST`，最后 `RAISE_CARD`；提示行只显示一条。
7. 探针失败**不得**写 `s_backoff_until_us`，否则常规 FTM 在 20 m 边界处会被探针的失败反复拖慢。

---

## 14. 参考资料

“§x” 标注主要服务于本文哪一节。一期已列的 IDF/LVGL 通用链接见 [v3 实现指南 §9](pass-radar-v3-详细设计与实现指南.md#9-参考资料)，此处不重复。

### 14.1 仓库内文档

- [远距与遮挡场景分析](pass-radar-v3-远距与遮挡场景分析.md) —— 本文每个特征（F1~F7）与修正（C1~C10）的物理依据与预期数值
- [v3 详细设计与实现指南](pass-radar-v3-详细设计与实现指南.md) —— §0.1 依赖的一期接口（`radar_fusion_update_v3`、等级、两射线、令牌 FTM）
- [AI 硬件开发指南](../hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.zh_CN.md) —— LVGL 锁、按键事件（§6.2 DOWN 单击/双击冲突）、内存约束（§9）
- [测试设计与用例](pass-radar-v3-测试设计与用例.md) —— U-OBS-01~14、D-16~D-20、F-03/F-11~F-14

### 14.2 传播模型（参数与阈值的来源）

- 两射线地面反射模型与折点 `4·h_t·h_r/λ`：https://en.wikipedia.org/wiki/Two-ray_ground-reflection_model （§6.1 举高后折点 130 m）
- Weissberger 植被衰减模型：https://en.wikipedia.org/wiki/Weissberger%27s_model （§3.2 `OBS_FOLIAGE_*`）
- ITU-R P.833 植被衰减：https://www.itu.int/dms_pubrec/itu-r/rec/p/R-REC-P.833-9-201609-S!!PDF-E.pdf
- ITU-R P.2040 建筑材料穿透损耗：https://www.itu.int/dms_pubrec/itu-r/rec/p/R-REC-P.2040-2-202109-S!!PDF-E.pdf （§3.2 `OBS_EXCESS_MIN_DB`）
- 单刀刃绕射损耗：https://www.gaussianwaves.com/2019/04/modeling-diffraction-loss-single-knife-edge-diffraction-model/ （§6.1 举高收益）
- 菲涅尔区：https://en.wikipedia.org/wiki/Fresnel_zone
- 人体遮挡对 2.4 GHz 的影响：https://pmc.ncbi.nlm.nih.gov/articles/PMC6211019/ （§7.2 对端别动）

### 14.3 特征与统计方法

- L. J. Greenstein, D. G. Michelson, V. Erceg, “Moment-method estimation of the Ricean K-factor”, *IEEE Communications Letters*, vol. 3, no. 6, pp. 175–176, 1999，DOI 10.1109/4234.769521：https://ieeexplore.ieee.org/document/769521 （§3.2 `radar_obstruct_kfactor_db`）
- 莱斯衰落与 K 因子定义：https://en.wikipedia.org/wiki/Rice_distribution （§3.3 合成序列）
- 瑞利衰落：https://en.wikipedia.org/wiki/Rayleigh_fading
- Box–Muller 变换（测试用高斯噪声）：https://en.wikipedia.org/wiki/Box%E2%80%93Muller_transform （§3.3）
- 线性同余发生器（固定种子可重复）：https://en.wikipedia.org/wiki/Linear_congruential_generator （§3.3）
- 分位数估计：https://en.wikipedia.org/wiki/Quantile （§3.2 `radar_obstruct_upper_quantile`）
- 对数距离路径损耗模型（超额损耗公式的来源）：https://en.wikipedia.org/wiki/Log-distance_path_loss_model （§3.2 `radar_obstruct_excess_loss_db`）

### 14.4 Wi-Fi 测距在 NLOS 下的行为

- Testing and Evaluation of Wi-Fi RTT Ranging Technology（NLOS 正向偏差）：https://pmc.ncbi.nlm.nih.gov/articles/PMC10007519/ （§3.2 反射判据）
- FUSIC：Accurate WiFi-based Ranging in the Presence of Multipath（RTT/RSSI 一致性等 NLOS 特征）：https://kjiokeng.github.io/assets/pdf/papers/fusic-infocom20.pdf （§3.2 F1）
- Fine Time Measurement for the IoT: A Practical Approach Using ESP32（− 70 dBm 以下成功率陡降）：https://arxiv.org/html/2401.16517v1 （§5 探针门限）
- ESP-IDF FTM 示例 README：https://github.com/espressif/esp-idf/blob/master/examples/wifi/ftm/README.md （§5）

### 14.5 射频灵敏度与速率

- ESP32-C3 数据手册（11b 1 Mbps / 11n MCS0 / LE Coded 接收灵敏度）：https://www.espressif.com/sites/default/files/documentation/esp32-c3_datasheet_en.pdf （§0.2 量程承诺）
- ESP-NOW 速率配置 `esp_now_set_peer_rate_config`：https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-reference/network/esp_now.html （§0.1 前置）
- ESP-IDF RF 共存指南（LOST 态 BLE 哨兵与 Wi-Fi 的时分）：https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-guides/coexist.html
