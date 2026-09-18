#pragma once

// 测距/定向融合引擎 —— 纯数学模块,无 ESP-IDF 依赖,可 host test。
//
// 融合观测源:
//   - 双向 RSSI(本端测对端 + 对端测本端,经 ESP-NOW 回传)
//   - Wi-Fi FTM(802.11mc RTT 时间戳测距)
//   - 双端扫盘角度(环形均值融合)
//   - 环境自适应路径损耗指数
//
// 设计原则:观测量优先于滤波器;所有规则可单测。

#include <stdbool.h>
#include <stdint.h>

// ---- 置信度等级 ----
typedef enum {
    RADAR_CONF_OOR = 0,  // 超界/无链路
    RADAR_CONF_LOW,      // 单链路 RSSI(无可信对端数据)
    RADAR_CONF_MED,      // 双向 RSSI 融合,或 FTM 存疑(疑似反射径)
    RADAR_CONF_HIGH,     // FTM 有效且与双向 RSSI 一致
} radar_confidence_t;

// ---- 环境模型(路径损耗指数 n) ----
typedef enum {
    RADAR_ENV_OUTDOOR = 0,  // n = 2.2 室外开阔
    RADAR_ENV_INDOOR,       // n = 3.0 室内多径
    RADAR_ENV_OBSTRUCTED,   // n = 3.5 有明显遮挡
} radar_env_t;

// ---- 无效值约定 ----
#define RADAR_RSSI_ABSENT   (-127.0f)  // 无对端数据
#define RADAR_DIST_INVALID  (-1.0f)    // 距离不可用
#define RADAR_ANGLE_INVALID (-1.0f)    // 角度不可用

// FTM 与 RSSI 距离分歧超过该比例(且超出 2m 滞回)判为疑似反射径
#define RADAR_FTM_SUSPICIOUS_RATIO 1.8f
#define RADAR_FTM_SUSPICIOUS_MARGIN_M 2.0f
// FTM/RSSI 加权融合中 FTM 的权重
#define RADAR_FTM_FUSION_WEIGHT 0.7f
// 双向 RSSI 差异超过该值(dB)说明链路不对称,不给 HIGH
#define RADAR_SYM_MAX_GAP_DB 8.0f
// 环境检测:RSSI 峰峰抖动超过该值(dB)判为室内
#define RADAR_ENV_P2P_INDOOR_DB 8.0f
// 环境估计窗口(样本数,100ms 喂一次 ≈ 8s)
#define RADAR_ENV_WINDOW 80
// 遮挡判据:长期均值低于该值(dBm)且样本数达标 → 判为有遮挡(n=3.5)
#define RADAR_ENV_OBSTRUCTED_MEAN_DB (-80.0f)
// 遮挡判据最小样本数(累计,2 个窗口 ≈ 16s)
#define RADAR_ENV_MIN_TOTAL 160

// 环境对应的路径损耗指数
float radar_env_path_loss_n(radar_env_t env);

// ---- 双向 RSSI ----

// 双向 RSSI 是否都有效(均非 ABSENT)
bool radar_fusion_sym_valid(float rssi_ab, float rssi_ba);

// 双向 RSSI 对称平均(dB 域算术平均)。
// 对置消除两端天线方向图与发射功率差异。
float radar_fusion_sym_rssi(float rssi_ab, float rssi_ba);

// 对称 RSSI → 距离(米)。tx_ref_sym 为 1m 处对称参考值。
float radar_fusion_sym_distance(float rssi_sym, float tx_ref_sym, radar_env_t env);

// ---- FTM 滤波 ----

// 一轮 burst 的原始 RTT 距离(米)→ 中值滤波。
// out_spread_m 输出四分位距(IQR),可为 NULL。
// count==0 返回 RADAR_DIST_INVALID。
float radar_fusion_ftm_filter(const float *raw_m, unsigned count, float *out_spread_m);

// 反射径判别:ftm_m > rssi_m * ratio 且 ftm_m > rssi_m + margin 时存疑。
bool radar_fusion_ftm_suspicious(float ftm_m, float rssi_m);

// ---- 融合主入口 ----

typedef struct {
    bool valid;
    bool fresh;
    float distance_m;
    float spread_m;
    unsigned valid_bursts;
} radar_fusion_ftm_observation_t;

typedef struct {
    float distance_m;        // 融合距离(无效时 RADAR_DIST_INVALID)
    radar_confidence_t conf; // 置信度
    float sym_rssi_dbm;      // 对称 RSSI(供显示)
    float rssi_dist_m;       // 纯 RSSI 推算距离(供调试/降级)
    bool  ftm_used;          // 本次是否采纳了 FTM
} radar_fusion_result_t;

// FTM 质量门控：有效 burst 不足或 spread 超阈值时禁止 HIGH。
radar_fusion_result_t radar_fusion_update_ftm(
    const radar_fusion_ftm_observation_t *ftm,
    float local_rssi_dbm, float peer_rssi_dbm,
    float tx_ref_sym, radar_env_t env);

// 兼容旧调用方：把有效 FTM 视为新鲜、16 个 burst、零 spread。
radar_fusion_result_t radar_fusion_update(bool ftm_valid, float ftm_m,
                                          float local_rssi_dbm, float peer_rssi_dbm,
                                          float tx_ref_sym, radar_env_t env);

// ---- 角度工具 ----

// 环形角度差(0~180°)
float radar_fusion_angle_diff(float a_deg, float b_deg);

// 环形均值(向量平均),count==0 返回 RADAR_ANGLE_INVALID
float radar_fusion_circular_mean(const float *angles_deg, unsigned count);

// 双端角度融合:一端无效返回另一端;均无效返回 RADAR_ANGLE_INVALID;
// 均有效返回环形均值(分歧大小由调用方用 radar_fusion_angle_diff 评估)。
float radar_fusion_angle_pair(float local_deg, float peer_deg);

// ---- 环境自适应估计 ----

typedef struct {
    float min_rssi;
    float max_rssi;
    float mean_rssi;     // 长期累计均值(遮挡判据,全历史)
    unsigned count;      // 当前窗口内已计数
    unsigned total;      // 累计样本数(mean_rssi 分母)
    radar_env_t env;
} radar_env_est_t;

void radar_env_est_init(radar_env_est_t *est);

// 每 100ms 喂一次滤波后 RSSI,返回当前环境判断。
// 窗口满后按滑动窗口滚动估计。
radar_env_t radar_env_est_update(radar_env_est_t *est, float rssi_dbm);

// ---- 1m 标定 ----

// 1m 处测得的对称 RSSI 即 tx_ref_sym(钳位到 -60~-30dBm 防止误标定)。
float radar_fusion_calib_ref(float sym_rssi_at_1m);
