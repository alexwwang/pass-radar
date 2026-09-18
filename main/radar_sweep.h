#pragma once

// 旋转扫盘峰值检测与距离估算 —— 纯数学模块,无 ESP-IDF 依赖,可 host test。
//
// v2 增强:
//   - 采样点携带信道标记,支持扫盘期间 1/6/11 信道分集;
//   - 抛物线插值峰值角度(亚采样分辨率);
//   - 峰显著性(峰-P10基线)用于低置信度识别;
//   - 多信道角度投票(环形中值)抑制单频多径假峰。

#include <stdint.h>

// 采样点结构(与 radar_espnow.c 共用)
typedef struct {
    uint32_t timestamp_ms;  // 相对扫盘起始时刻的毫秒偏移
    float    rssi;          // 卡尔曼滤波后的信号强度
    uint8_t  channel;       // 采样时所在 Wi-Fi 信道(0=未标记/旧逻辑)
} radar_sample_t;

// 信号阈值:低于此值视为信号微弱/超出范围,不报告方向。
#define RADAR_RSSI_MIN_VALID (-88.0f)

// 峰显著性门限:峰值低于 (P10 基线 + 该值) 时认为没有可信方向峰
#define RADAR_SWEEP_MIN_PROMINENCE_DB 6.0f

// 自由空间路径损耗模型参数(户外开阔地)
#define RADAR_TX_POWER_REF  (-42.0f)  // 1 米处参考 RSSI(dBm)
#define RADAR_PATH_LOSS_N   (2.2f)    // 户外衰减因子

// 扫盘信道分集:快跳轮询,每个驻留窗口换一个信道(1/6/11 循环)。
// 快跳使每个信道都能覆盖完整 360° 旋转,得到独立方向图用于投票;
// 若改为分段(各管 1/3 时长),每信道只能看到 120° 扇区,无法独立测角。
#define RADAR_SWEEP_CH_A 1
#define RADAR_SWEEP_CH_B 6
#define RADAR_SWEEP_CH_C 11
#define RADAR_SWEEP_HOP_DWELL_MS 100

// 峰显著性等统计算法的样本上限(大于实际最大采样数 100)
#define RADAR_MAX_SAMPLES_CAP 128

// 由滤波后 RSSI 估算距离(米)。rssi 越小距离越远。
float radar_sweep_calc_distance(float rssi);

// 在采样数组中找 RSSI 峰值,映射为 0°~360° 角度(相对于扫盘起始方向)。
// 峰值时间点 / 总时长 × 360 = 检测角度。
// 无采样返回 -1。
int radar_sweep_peak_angle(const radar_sample_t *samples, unsigned count,
                           uint32_t sweep_duration_ms);

// 从采样数组中返回峰值 RSSI(供阈值判断用)。
float radar_sweep_peak_rssi(const radar_sample_t *samples, unsigned count);

// ---- v2 增强 ----

// 抛物线插值峰值角度(亚采样分辨率,返回 float)。
// 对峰两侧样本做抛物线拟合,修正时间→角度映射的量化误差。
// count==0 或 duration==0 返回 -1.0f。
float radar_sweep_peak_angle_interp(const radar_sample_t *samples, unsigned count,
                                    uint32_t sweep_duration_ms);

// 峰显著性(dB):峰值与 P10 分位基线的差。count==0 返回 0。
float radar_sweep_peak_prominence(const radar_sample_t *samples, unsigned count);

// 扫盘信道调度:t_ms 时刻应处于的信道(快跳轮询:CH_A → CH_B → CH_C → CH_A …)。
uint8_t radar_sweep_channel_at(uint32_t t_ms, uint32_t sweep_duration_ms);

// 一次谐波最小二乘拟合角度:对 y = a + b·cosθ + c·sinθ 拟合方向图,
// 峰角 = atan2(c, b)。相比原始峰检测,对快跳造成的稀疏/成簇采样鲁棒。
// out_amplitude_db 输出拟合幅度(峰-基线,dB),可为 NULL。
// out_residual_db 输出拟合 RMS 残差,可为 NULL。
// 样本不足(<6)或法方程退化返回 -1.0f。
float radar_sweep_harmonic_angle_ex(const radar_sample_t *samples, unsigned count,
                                    uint32_t sweep_duration_ms,
                                    float *out_amplitude_db,
                                    float *out_residual_db);
float radar_sweep_harmonic_angle(const radar_sample_t *samples, unsigned count,
                                 uint32_t sweep_duration_ms, float *out_amplitude_db);

// 按信道分组分别做谐波拟合测角,仅保留拟合幅度达标的信道角度。
// out_angles 最多写 max_out 个角度;返回有效角度个数。
unsigned radar_sweep_channel_angles(const radar_sample_t *samples, unsigned count,
                                    uint32_t sweep_duration_ms,
                                    float *out_angles, unsigned max_out);

// 多信道角度投票:环形中值(使环形差平方和最小的候选)。
// out_spread_deg 输出入选角度间的最大环形差,可为 NULL。
// count==0 返回 -1.0f。
float radar_sweep_angle_vote(const float *angles, unsigned count, float *out_spread_deg);
