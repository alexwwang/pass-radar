# pass-radar v3 测试设计与用例

> 目标读者：实现 v3 的工程师与外场测试者
> 日期：2026-09-19
> 配套：[详细设计与实现指南](pass-radar-v3-详细设计与实现指南.md)（一期函数名/常量以该文为准）、[v3.1 远距与遮挡实施指南](pass-radar-v3.1-远距与遮挡实施指南.md)（二期）、[15~100 m 远距与遮挡分析](pass-radar-v3-远距与遮挡场景分析.md)、[精度目标评估](pass-radar-定向测距精度目标评估与v3设计方案.md)
> 阶段标注：里程碑 M0~M3 属于**一期（v3）**；标 **P2** 的用例属于**二期（v3.1 远距与遮挡）**，一期验收不要求它们。
> 用例编号规则：`U-` host 单测、`I-` 模拟器/联调、`D-` 机上自测（单卡或双卡静态）、`F-` 外场统计测试、`R-` 回归。每条用例都写"输入 → 期望"，可直接翻译成 `CHECK()`。表中函数名省略了 `radar_xxx_` 前缀时，以实现指南中的全名为准。

---

## 1. 测试分层与运行方式

| 层 | 运行环境 | 触发方式 | 覆盖 |
| --- | --- | --- | --- |
| U host 单测 | 电脑，`cc -std=c11 -Wall -Wextra -Werror` | `./tools/validate.sh --static` | 所有纯逻辑：融合、等级、扫盘测角、FTM 逻辑、遮挡特征、诊断缓冲 |
| I 模拟器 | passport-sim（加载正式固件） | `tools/sim/radar-screen-test.mjs` | 固件引导与首帧渲染；不验 BLE、无线测量或按键交互 |
| D 机上自测 | 两张卡、室内桌面 | 诊断页 + 日志 | FTM 调通、令牌、CSI 帧、BLE 惰性、内存 |
| F 外场统计 | 室外/室内/遮挡场地 | 人工按矩阵执行 | 精度 p50/p90、离谱率 |
| R 回归 | 每次发版 | 上述子集 | v1/v2 行为不退化 |

### 1.1 host 测试骨架

沿用 `tests/test_radar_fusion.c` 的 `CHECK(cond, fmt, ...)` 宏。新增测试文件需在 `tools/validate.sh` 的 `run_static_checks()` 登记，例如：

```bash
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
    tests/test_radar_obstruct.c main/radar_obstruct.c main/radar_fusion.c main/radar_sweep.c \
    -o "${test_dir}/test_radar_obstruct" -lm
"${test_dir}/test_radar_obstruct"
```

### 1.2 合成数据生成器（`tests/synth.h`，仅测试用）

所有扫盘/寻的算法测试使用同一套生成器，避免每个用例手写数组：

```c
// 生成人体遮挡方向图样本：base_dbm + amp_db·cos(θ − peak_deg) + 噪声(σ) ，
// 可选第二峰（多径）与"提前停转"（stop_ratio<1 时尾部平台）。
typedef struct {
    float base_dbm, amp_db, peak_deg, noise_sigma_db;
    float second_peak_deg, second_amp_db;   // 0 表示无
    float stop_ratio;                       // 1.0 = 匀速转满；0.75 = 4.5 s 转完剩余静止
    float speed_wobble;                     // 0~0.5，角速度正弦扰动幅度
    unsigned rate_hz; uint32_t duration_ms; uint32_t seed;
} synth_sweep_cfg_t;
unsigned synth_sweep(const synth_sweep_cfg_t *c, radar_sample_t *out, unsigned max);

// 生成 FTM session 原始距离：true_m + offset_m + 正向多径偏差(概率 p_nlos, 幅度 U[1,bias_max])
unsigned synth_ftm(float true_m, float offset_m, float p_nlos, float bias_max, unsigned n, float *out, uint32_t seed);

// 生成 RSSI 时间序列：均值 mean + 慢衰落(σ_slow, 周期 T_slow) + 快衰落(σ_fast)
unsigned synth_rssi_series(float mean, float sigma_fast, float sigma_slow, float t_slow_s,
                           unsigned n, float dt_s, float *out, uint32_t seed);
```

随机数用固定种子的 LCG（`seed = seed*1103515245+12345`），保证可重复。

### 1.3 真机测试人力分级

真机项目分为两类：

- **S（Solo，单人必测）**：一人可以完成。第二张卡可以固定放在桌面、三脚架或标记点；需要查看另一张卡时，先固定场景，再通过屏幕录屏、串口日志或测试后读取结果完成检查。S 项目是进入下一阶段的默认验收项。
- **O2（Optional two-person，两人可选）**：需要两个人同时持卡、移动或观察两端状态。O2 用于增强双端一致性证据，不阻塞单人验收；有第二名测试者时再执行。

| 范围 | 单人可完成（S） | 两人协同可选（O2） |
| --- | --- | --- |
| U / I / R | 全部 host、passport-sim 和回归用例 | 无需两人 |
| D 机上自测 | D-01~D-04、D-06~D-20；D-05 可用两张固定卡和事后日志完成 | D-05 同时观察两端 FTM token/计数时更适合两人 |
| F 外场统计 | F-01~F-06、F-08~F-17；一张卡固定，另一张卡由测试者携带即可 | F-07 双端互扫镜像，要求两人分别持卡并同时完成方向动作 |
| 旧版 T 用例 | T1~T5、T7~T12；另一张卡固定即可 | T6 双端同扫 |

单人测试的固定方式必须写入记录，例如“目标卡固定在 15 m 标记点，操作者携带本卡”。不能因为只有一人就把第二张卡拿在手中随意移动，否则距离、方向和遮挡结果不可复现。涉及两张卡同时操作的 D-05/F-07/T6，默认标为 O2；没有第二个人时跳过并记录 `NOT RUN (O2 optional)`，不得把跳过写成 PASS。

### 1.4 单人优先执行顺序

真机资源有限时按以下顺序执行：

1. **安装与安全**：D-14、T12，确认 meta-pass 槽位、启动、LONG2 回退和重启回滚。
2. **单人基础链路**：D-01~D-04、D-07~D-10、D-13、D-15。
3. **单人功能闭环**：D-16~D-20；其中目标卡固定，操作者只移动/举高本端卡。
4. **单人外场统计**：F-01~F-06、F-08~F-17。
5. **可选协同增强**：D-05、F-07、T6，仅在有第二名测试者时执行。

单人执行不降低通过标准；只改变持卡方式和观察方式。两人协同项目只能增加证据，不能替代单人基础链路、安装安全和回滚测试。

---

## 2. host 单测用例

### 2.1 融合规则 `radar_fusion_update_v3`（U-FUS）

| ID | 输入 | 期望 |
| --- | --- | --- |
| U-FUS-01 | 无对端、无 FTM | `conf=OOR`, `distance=INVALID` |
| U-FUS-02 | 单向 RSSI −60，无对端 | `conf=LOW`，`lo<d<hi`，`hi/lo ≈ (1+e)²` |
| U-FUS-03 | 双向 −60/−66，L0，无 FTM | `conf=MED`，`d = sym_distance(−63)`，e=0.15 |
| U-FUS-04 | 双向 −60/−66，L0，FTM 8.0 m 标定/新鲜/spread 0.3，rssi_d≈8 | `conf=HIGH`，`d=8.0`，`lo=7.5,hi=8.5`（±max(0.5,spread)） |
| U-FUS-05 | 同 04 但 L1 | `conf=MED`（HIGH 仅 L0） |
| U-FUS-06 | FTM 30 m，rssi_d 8 m | 反射径分支：`d=min(30, 8·1.3)=10.4`，`hi=30`，`conf=MED`，`ftm_used=true` |
| U-FUS-07 | FTM 6 m，rssi_d 20 m（RSSI 被遮挡衰减） | 遮挡分支：`d=6`，`obstructed=true`，`lo=5,hi=7` |
| U-FUS-08 | FTM 有效但 `calibrated=false` | 不采纳 FTM 显示：`ftm_used=false`，走 RSSI 分支 |
| U-FUS-09 | FTM `fresh=false` | 同 08 |
| U-FUS-10 | L2、双向 RSSI 有效、无 FTM | `upper_bound_only=true`，`conf=LOW`，`hi=d·1.6` |
| U-FUS-11 | 双向差 12 dB，FTM 一致 | `conf` 不得为 HIGH |
| U-FUS-12 | RSSI 为 NAN | `distance=INVALID`，不崩 |
| U-FUS-13 | sym −90，对端 alive | **不** OOR（v3 仅"5 s 无包"才 OOR）：`conf=LOW`，`upper_bound_only=true`，距离用两射线远场段计算 |
| U-FUS-14 | sym −90，对端 !alive | `OOR` |

### 2.2 两射线分段模型 `radar_fusion_sym_distance`（U-PL）

| ID | 输入（tx_ref −42, n 2.2） | 期望 |
| --- | --- | --- |
| U-PL-01 | rssi −42 | 1.0 ±0.05 m |
| U-PL-02 | rssi −64 | 8~12 m（与 v2 一致，折点前不变） |
| U-PL-03 | rssi = rssi_break = −42 − 22·log10(45) ≈ −78.4 | 45 ±0.5 m（连续性） |
| U-PL-04 | rssi −78.4 − 12 | 45·10^(12/40) ≈ 90 m ±2 |
| U-PL-05 | rssi −78.4 − 12，env=INDOOR | 单一 n=3.0 模型（室内不启用两射线）：结果 < 90 m |
| U-PL-06 | 单调性：rssi 从 −40 到 −100 步进 1 dB | 距离严格递增 |

### 2.3 环境等级 `radar_level_classify` / `radar_level_update`（U-LVL）

| ID | 特征 | 期望等级 |
| --- | --- | --- |
| U-LVL-01 | ftm_rate 0.9, iqr 0.5, p2p 4, gap 3, sel 2, alive | L0 |
| U-LVL-02 | ftm_rate 0.5, iqr 1.5, p2p 7, gap 3 | L1 |
| U-LVL-03 | ftm_rate 0.1, mean −83 | L2 |
| U-LVL-04 | p2p 12（其余 L0） | L2 |
| U-LVL-05 | sel 9（其余 L0） | L2 |
| U-LVL-06 | iqr 2.5（其余 L0） | L2 |
| U-LVL-07 | sel=NAN（无 CSI），其余 L0 | L0（NAN 不参与） |
| U-LVL-08 | alive=false | L3 立即（不受滞回约束） |
| U-LVL-09 | 滞回：当前 L0 已 20 s，喂 L1 特征 29 次 | 仍 L0；第 30 次 → L1 |
| U-LVL-10 | 驻留：刚切到 L1 5 s，喂 L0 特征 40 次 | 仍 L1（未满 10 s） |
| U-LVL-11 | L3 → alive 连续 99 次(9.9 s) | 仍 L3；第 100 次 → 回到 L1（默认回中间档，再由特征判） |
| U-LVL-12 | 交替喂 L0/L1 特征各 1 次 ×100 | 等级从不切换（候选计数被打断） |

### 2.4 FTM 纯逻辑（U-FTM）

| ID | 输入 | 期望 |
| --- | --- | --- |
| U-FTM-01 | `ftm_offset_from_calib({3.9,4.1,4.0,4.3})` | 3.05 ±0.1（中值 4.05 − 1） |
| U-FTM-02 | count=2 | NAN |
| U-FTM-03 | 中值 20 m | 钳位 15.0 |
| U-FTM-04 | 中值 0.2 m | −0.8（在 −2~15 范围内，不钳位） |
| U-FTM-04b | 中值 −2.5 m | 钳位 −2.0 |
| U-FTM-05 | `apply_offset(0.5, 3.0)` | 0.1（下限） |
| U-FTM-06 | `low_quantile({5,5.2,9,5.1,12})` | ≈5.05（P25） |
| U-FTM-07 | 令牌状态机：两端初始都 `have_token=false`（模拟丢包）3 s | MAC 小者取回令牌 |
| U-FTM-08 | A 完成 session → A GRANT；B 收到 → B have_token；B 发起 | A 不再发起直到 B 的 GRANT |
| U-FTM-09 | 对端 proto_ver=2 | 令牌恒等于 "MAC 小者" |
| U-FTM-10 | peer_rssi −75 | tick 不发起 |
| U-FTM-11 | 对端 BUSY 位在 500 ms 前收到 | tick 不发起；900 ms 后可发起 |
| U-FTM-12 | `synth_ftm(true 5, offset 3, p_nlos 0.3, bias 6, n 40)` → 每 8 帧一 session 中值 → 减偏移 → 5 窗口 P25 | 最终值与 5 m 误差 ≤0.8 m；直接用中值误差更大（断言 P25 误差 ≤ 中值误差） |

令牌状态机需以纯函数形式实现（`radar_ftm_token_step(state*, event, now_ms)`）放在 `radar_ftm_logic.c`（无 IDF 依赖）才能 host 测试。

### 2.5 扫盘测角（U-SWP）

用 `synth_sweep` 生成，`duration 6000, rate 40`。

| ID | 生成配置 | 期望 |
| --- | --- | --- |
| U-SWP-01 | amp 8, peak 135, σ 2, 匀速 | `harmonic_angle` 误差 ≤3° |
| U-SWP-02 | 同上 σ 4 | 误差 ≤6° |
| U-SWP-03 | amp 3（弱遮挡）σ 3 | `multi_angle` n_used=0 或 spread>45 → 扇区 |
| U-SWP-04 | 第二峰 peak2=300, amp2=6（多径） | RSSI 单源误差可能 >30°；`multi_angle` 4 组 CSI 中第二峰仅出现在 1 组（构造）→ 投票误差 ≤15° |
| U-SWP-05 | stop_ratio 0.75 | `estimate_period_ms` 返回 4300~4700；修正后角度误差 ≤10° |
| U-SWP-06 | speed_wobble 0.3 | 误差 ≤15°（记录数值，作为节拍器效果基线） |
| U-SWP-07 | 样本 40 个（对应丢包 85%） | 仍返回角度或明确 −1，不崩 |
| U-SWP-08 | 5 个样本 | −1 |
| U-SWP-09 | 时间戳相同的相邻样本（40 Hz 同毫秒） | 不被判为非法序列 |
| U-SWP-10 | `from_csi_group` 转换 | 输出个数 = 输入个数，`rssi` 字段 = `p_db[g]` |
| U-SWP-11 | 镜像一致：local 100，peer 275 | 差 5° ≤30 → 融合 ≈ 97.5（peer+180=95 与 100 的环形均值） |
| U-SWP-12 | local 100，peer 200（差 80°） | 不融合，spread ≥ 60 → 扇区 |
| U-SWP-13 | 寻的滑窗：1 s/40 样本，peak 20° | `harmonic_fit` 输出 20 ±8° |

### 2.6 遮挡/远距特征 `radar_obstruct`（U-OBS，**二期 P2**；接口见 v3.1 实施指南 §3）

| ID | 输入 | 期望 |
| --- | --- | --- |
| U-OBS-01 | `kfactor_db({线性功率序列 K≈10 dB 合成})` | 8~12 dB |
| U-OBS-02 | 瑞利序列（K→0） | < 2 dB |
| U-OBS-03 | `excess_loss_db(ftm 6, rssi_d 20, n 2.2)` | 22·log10(20/6) ≈ 11.5 dB |
| U-OBS-04 | ftm 无效 | NAN |
| U-OBS-05 | `classify`: excess 11, K 9, sel 3, p2p_1s 2, p2p_10s 3 | `OBS_ATTENUATING_STATIC`（墙） |
| U-OBS-06 | excess 8, K 2, p2p_1s 8, p2p_10s 9, sel 6 | `OBS_FOLIAGE_DYNAMIC` |
| U-OBS-07 | excess NAN, rssi −86, p2p 4, K 8, level L0 | `OBS_FAR_LOS`（真远） |
| U-OBS-08 | excess −6（FTM 反而更大） | `OBS_REFLECTION` |
| U-OBS-09 | `radar_obstruct_upper_quantile(序列, n, 0.8)` | 返回 P80，且 ≥ 均值；n<10 返回 NAN |
| U-OBS-10 | `apply`: FOLIAGE 时距离用 P80 RSSI 重算 | 距离 < 用均值算的距离 |
| U-OBS-11 | `apply`: ATTENUATING 且 FTM 有效，mean_rssi −78 | 距离 = FTM，`obstructed=true`，返回 `HINT_RAISE_CARD` |
| U-OBS-12 | `apply`: ATTENUATING 且 FTM 无效，level L2 | `upper_bound_only=true`，返回 `HINT_WALK_TEST` |
| U-OBS-13 | `apply`: REFLECTION，spread 12 | spread 改为 90，返回 `HINT_SWEEP_AGAIN` |
| U-OBS-14 | `classify`: 全部特征 NAN/0，level L0 | `OBS_NONE_LOS` 或 `OBS_UNKNOWN`，不崩 |

### 2.7 诊断缓冲（U-DIAG）

| ID | 输入 | 期望 |
| --- | --- | --- |
| U-DIAG-01 | 写 5 行 | `line(0)` 是最后写的；`line(5)` 返回 false |
| U-DIAG-02 | 写 40 行（>32） | `line(31)` 是第 9 行；无越界 |
| U-DIAG-03 | 200 字节长行 | 截断到 95 字符 + `\0` |
| U-DIAG-04 | 计数器 inc 1000 次 | get=1000 |

### 2.8 协议（U-PROTO，扩展现有 `test_radar_protocol.c`）

| ID | 输入 | 期望 |
| --- | --- | --- |
| U-PROTO-01 | proto_ver=3，v3_flags=GRANT | `valid_v2()` true；解析出 GRANT |
| U-PROTO-02 | proto_ver=2，reserved 非零 | 忽略 v3 字段 |
| U-PROTO-03 | `sizeof(radar_packet_t)` | 与 v2 相同（二进制兼容断言） |

---

## 3. 模拟器用例（I）

passport-sim 当前不提供 Wi-Fi/ESP-NOW/FTM/CSI/BLE 射频模型；检测到固件进入 BLE ROM 后会暂停并禁止模拟交互。因此模拟器测试只加载正式固件并验证 **boot 与首帧渲染**。无线数据、距离、方向、FTM、CSI、BLE、遮挡分类及按键交互必须以 host 测试或真机测试为准；缺少射频输入时，正式固件应显示无对端/降级状态，不得卡死或崩溃。

passport-sim currently has no Wi-Fi/ESP-NOW/FTM/CSI/BLE radio model. When the firmware enters the unsupported BLE ROM path, the simulator pauses and disables interaction. The simulator scope is therefore limited to **production-firmware boot and first-frame rendering**; wireless measurements and button interaction are verified by host or device tests.

| ID | 场景 / Scenario | 检查 / Acceptance |
| --- | --- | --- |
| I-01 | 加载正式固件 / Load production firmware | 在限定周期内完成 boot，模拟器不报加载错误 / boots without a load error |
| I-02 | 应用首帧 / Application first frame | 产生 240×320 非纯色显示帧 / emits a non-solid 240×320 frame |
| I-03 | BLE 缺失 / BLE unavailable | 记录为 simulator 已知限制，不作为固件失败 / recorded as a simulator limitation, not a firmware failure |
| I-04 | 按键交互 / Button interaction | 不在 passport-sim 中执行；改由真机或支持射频的测试环境验证 / not run in passport-sim; verify on device or a capable test environment |

---

## 4. 机上自测（D）—— 室内桌面，两张卡

前置：两卡烧同一 v3 固件；一张接 USB 看 `idf.py monitor`（可选）。诊断页入口：主页 **DOWN 双击**。

| ID | 步骤 | 通过标准 | 里程碑 |
| --- | --- | --- | --- |
| D-01 诊断页 | DOWN 双击；UP/DOWN 翻屏 | 两屏均可进入/退出；日志 8 行滚动 | M0 |
| D-02 无跳频 | 扫盘一次，看日志 | 无 `channel switch`；`DIAG_CNT_SWEEP_SAMPLES ≥ 180` | M1 |
| D-03 BLE 惰性 | 开机 30 s 看诊断页 heap；关掉对端 10 s；再开 | Tracking 态 free heap 比 v2 多 ≥60 KB；LOST 后 BLE 起且 `FAR (BLE)` 出现；恢复后 BLE 停 | M1 |
| D-04 FTM 会话 | 两卡 3 m，等 30 s | `FTM_OK ≥ 15`，`STATUS_3 < FTM_OK` | M1 |
| D-05 令牌 | 看两卡诊断页 | 两卡都出现 `FTM ok`，各自 `FTM_INITIATE` 计数相近（±30%） | M1 |
| D-06 令牌恢复 | 遮住一卡天线 5 s 使丢包，再放开 | 3 s 内两卡恢复交替 | M1 |
| D-07 FTM 标定 | 1 m 长按 OK 3 s | 日志 `标定完成 txref=… ftmoff=…`；`ftmoff` 在 −2~15；诊断页 `calibrated=1` | M1 |
| D-08 静态距离 | 标定后 1/3/5 m 各 60 s | FTM 距离 p50 误差 ≤1 m；主页显示 `[HI]` 或 `[MED]` 而非跳变 | M1 |
| D-09 等级稳定 | 3.5 m 静止 2 min | 等级切换 ≤1 次；距离显示区间稳定（点值抖动 <30%） | M1 |
| D-10 CSI 帧 | 扫盘一次 | `DIAG_CNT_CSI_RX` 增长 ≈ ESP-NOW 收包数（±20%）；选择性指标 0~15 dB 合理 | M2 |
| D-11 节拍器 | 扫盘 | 8 声可闻；首拍高音；指针与拍同步 | M2 |
| D-12 寻的 | 结果页 OK | 音调随卡片转向变化；OK 锁定；20 s 超时回主页 | M3 |
| D-13 内存 | 连续 10 min 追踪 + 5 次扫盘 + 2 次寻的 | free heap 最终值与起始差 <5 KB；largest block >40 KB | 每版 |
| D-14 启动器 | 作为子固件安装 | OK LONG2 返回启动器；DOWN 双击不误触 | 每版 |
| D-15 v2 互通 | 一卡 v2、一卡 v3 | v3 卡显示距离；FTM 退化为 MAC 小者单向；不崩 | M1 |
| D-16 类别切换 | 3 m 静止 30 s；一人在两卡中间来回走 30 s；离开 | 静止 `OBS_NONE_LOS`；走动 2~5 s 内变 `FOLIAGE?`；离开 2~5 s 后回 NONE | P2 |
| D-17 FTM 探针 | 室外 25 m 静止 2 min | `probe` 发起 ≥20，成功率 ≥20%；行 2 出现 `FTM probe ok` | P2 |
| D-18 举高 | 室外 40 m，UP 短按后举高 | 行 3 `RAISED +x dB`，x ≥ 2；5 s 后恢复；距离不突变为一半 | P2 |
| D-19 步行 | 隔墙 15 m 出现 `WALK 5m THEN DOWN`；DOWN → 走 5 m → DOWN | 提示 `NEAR: <= 25 m`；上界 ≤25 m 持续 60 s | P2 |
| D-20 共识 | 隔墙（L2）连续扫盘两次 | 第一次扇区 + `SWEEP AGAIN`；第二次差 ≤30° 时变射线 | P2 |

---

## 5. 外场统计测试（F）

### 5.1 统计口径（所有 F 用例通用）

- 每个测点 **≥20 次独立读数**。距离读数：每 3 s 抄一次主页点值；方向读数：每次扫盘一次。
- 指标：p50、p90、**离谱率**（距离误差 >2× 真值或方向误差 >60° 的比例）。
- 真值：距离用激光测距仪或卷尺；方向用手机罗盘或地面标线（8 方向）。
- 人力：默认由 1 位测试者完成，目标卡固定、测试卡由操作者携带；仅 F-07 需要两人同时持卡，属于 O2 可选增强项。若做 F-07，建议两位不同体型测试者各做一半，但不作为单人验收前置条件。
- 记录模板见 §6。

### 5.2 用例矩阵

| ID | 场景 | 测点 | 目标（p50 / p90 / 离谱率） | 里程碑 |
| --- | --- | --- | --- | --- |
| F-01 室外距离阶梯 LOS | 空地，一卡固定、一人持卡胸前 | 1/3/5/8/12/15 m | ≤1 m / ≤1.5 m / <5% | M1 |
| F-02 室外中距 | 同上，一卡固定 | 20/30/45 m | ≤15% / ≤30% / <5%；45 m 处观察两射线折点效果（与 v2 单一 n 对比） | M1 |
| F-03 室外远距 LOS | 同上，见远距文档 | 60/80/100 m | 报出等级 L0/L1 且距离 ≤50% 误差；100 m 不 OOR（1 Mbps） | P2 |
| F-04 室外定向 8 方向 | 15 m，节拍器开 | 8 方向 × 3 轮 | ≤15° / ≤25° / <5% | M2 |
| F-05 节拍器 A/B | 同一人同一方向 | 开/关各 10 次 | 开启后 p50 改善 ≥30% | M2 |
| F-06 故意转快/慢 | 4.5 s 转完 / 8 s 才转完 | 各 10 次 | 修正后误差 ≤15°；日志有 `self-correction` | M2 |
| F-07 双端互扫镜像 | 两人各持卡 15 m | 8 方向 | 两角度和 ≈180° ±30°；融合后 p50 ≤12° | M3 |
| F-08 寻的终态 | 扫盘后进入寻的 | 8 方向 | 锁定朝向误差 p50 ≤10°；耗时 ≤20 s | M3 |
| F-09 室内 LOS | 同房间 3/5/8 m | 距离 + 8 方向 | 距离 ≤2 m / ≤3 m；方向 ≤25° / ≤45°；不出 OOR | M2 |
| F-10 室内隔墙 | 一墙之隔 5~8 m | 距离 + 方向 | 等级 L2；显示 `≤ hi` 且 hi ≥ 真值；方向以扇区显示；离谱率 <15% | M3 |
| F-11 树丛遮挡 | 见远距文档 F-OBS-1 | 30 m，中间 3~5 m 灌木 | 识别 `FOLIAGE?`；P80 修正后误差优于均值法；提示行显示 | P2 |
| F-12 围墙遮挡 | 见远距文档 F-OBS-2 | 30 m，2 m 砖墙 | 识别 `BLOCKED?`；FTM 探针可用时距离 ≤3 m 误差；不可用时 `<=` 上界包含真值 | P2 |
| F-13 金属围栏 | 见远距文档 F-OBS-3 | 30 m | 不给假精度：L2 + 扇区；或识别 `REFLECT?` | P2 |
| F-14 举高提示 | L2 出现时按提示举高卡片 | 30~60 m | RSSI 提升 ≥3 dB 的比例 >50%；等级回升 | P2 |
| F-15 走远到 LOST | 慢走远 | 记录 OOR / FAR(BLE) / 消失距离 | FAR(BLE) 出现距离 ≥ Wi-Fi OOR 距离 ×1.3 | M3 |
| F-16 人群 | 校园/商场人流 | 20 m | 离谱率 <15%；等级 L1/L2 | M3 |
| F-17 连用 | 30 min 混合使用 | — | 无重启/卡屏/乱码；电量下降 ≤25% | 每版 |

### 5.3 A/B 对照

每个 F 用例同时记录诊断页 `v1 距离`（纯 RSSI）作为基线；方向用例记录 `RSSI 单源角` vs `multi_angle`。没有 A/B 数据的用例不算完成。

---

## 6. 记录模板

### 6.1 距离测点

```
日期/地点/天气(风力!)/测试者身高:
场景: F-xx    等级显示: L_    真值 d0 = ___ m    标定: txref ___ ftmoff ___
# | 显示点值 | 区间 lo~hi | conf | FTM 快照 d/spread | v1 距离 | 备注(遮挡提示?)
1 |
...
20|
p50 误差 = ___  p90 误差 = ___  离谱次数 = ___  区间覆盖真值次数 = ___/20
```

### 6.2 方向测点

```
真值方位 θ0 = ___°   节拍器: 开/关   双端: 单/双
# | Angle | +/- | n_used | 射线/扇区 | 对端角 | 寻的锁定角 | 误差
```

### 6.3 遮挡测点（配合远距文档）

```
障碍物类型/厚度/高度/距两端距离:   风力:
obstruct 分类显示:   excess_loss:   K:   sel:   p2p_1s/10s:   hint:
举高前 RSSI ___ → 举高后 ___
```

---

## 7. 回归（R）

| ID | 内容 | 频率 |
| --- | --- | --- |
| R-01 | `./tools/validate.sh --all` | 每次提交 |
| R-02 | D-01~D-09 | 每个里程碑 |
| R-03 | F-01、F-04、F-09 抽样（各 10 次） | 每次发版 |
| R-04 | v2 实测指南 T1~T12 重跑一遍，结果表对比 v2.1 记录 | M1 结束时一次 |

---

## 8. 通过/失败判定与升级

- 任一 U 用例失败 → 阻塞合并。
- D 用例失败 → 阻塞进入下一里程碑。
- F 用例未达目标但离谱率达标 → 记录为"已知差距"，允许发版，并在评估文档目标表中更新实测值。
- F 用例离谱率不达标 → 不允许发版（宁可显示扇区/区间，不给错数）。

---

## 9. 参考资料

### 9.1 仓库内

- `tests/test_radar_fusion.c`、`tests/test_radar_sweep.c` —— `CHECK()` 宏与用例组织方式的参照（§1.1）
- `tools/validate.sh` —— host 测试登记与固件校验入口（§1.1、R-01）
- `tools/sim/radar-screen-test.mjs` —— 使用正式固件执行模拟器启动/按键回归（§3）
- [双卡实测指南（v2.1）](pass-radar-device-test-guide.zh_CN.md) —— T1~T12 回归基线（R-04）
- [AI 硬件开发指南 §13 构建与验收](../hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.zh_CN.md) —— 通用上板验收与内存观察项（D-13）
- [v3 实现指南 §9](pass-radar-v3-详细设计与实现指南.md#9-参考资料)、[v3.1 实施指南 §14](pass-radar-v3.1-远距与遮挡实施指南.md#14-参考资料) —— 被测算法的定义与来源

### 9.2 测试方法与统计

- 分位数（p50/p90 口径）：https://en.wikipedia.org/wiki/Quantile （§5.1）
- 环形均值与环形方差（方向误差的正确平均方式）：https://en.wikipedia.org/wiki/Circular_mean （§5.1、U-SWP-11）
- 线性同余发生器（合成数据固定种子）：https://en.wikipedia.org/wiki/Linear_congruential_generator （§1.2）
- Box–Muller 变换（合成高斯噪声）：https://en.wikipedia.org/wiki/Box%E2%80%93Muller_transform （§1.2、U-OBS-01/02）
- 莱斯/瑞利分布（U-OBS-01/02 合成序列）：https://en.wikipedia.org/wiki/Rice_distribution ，https://en.wikipedia.org/wiki/Rayleigh_fading
- A/B 对照与基线记录的必要性：见 [精度目标评估 §6 验收指标](pass-radar-定向测距精度目标评估与v3设计方案.md)（§5.3）

### 9.3 外场测试的物理依据（阈值与预期值来源）

- ESP-IDF FTM 示例（− 70 dBm 建议、偏移标定）：https://github.com/espressif/esp-idf/blob/master/examples/wifi/ftm/README.md （D-04、D-07、T13）
- Fine Time Measurement for the IoT: A Practical Approach Using ESP32（LOS ±1 m、室内 3~5 m）：https://arxiv.org/html/2401.16517v1 （F-01、F-09 目标值）
- Fundamental limits in RSSI-based DoA estimation（方向误差预期）：https://www.researchgate.net/publication/312569085_Fundamental_limits_in_RSSI-based_direction-of-arrival_estimation （F-04）
- 两射线模型（F-02 的 45 m 折点观察点）：https://en.wikipedia.org/wiki/Two-ray_ground-reflection_model
- Weissberger / ITU-R P.833 植被衰减（F-11 场地选择）：https://en.wikipedia.org/wiki/Weissberger%27s_model ，https://www.itu.int/dms_pubrec/itu-r/rec/p/R-REC-P.833-9-201609-S!!PDF-E.pdf
- ITU-R P.2040 材料穿透损耗（F-12/F-13 场地选择）：https://www.itu.int/dms_pubrec/itu-r/rec/p/R-REC-P.2040-2-202109-S!!PDF-E.pdf
- 单刀刃绕射（F-14 举高预期增益）：https://www.gaussianwaves.com/2019/04/modeling-diffraction-loss-single-knife-edge-diffraction-model/
- ESP32-C3 数据手册（F-03/F-15 量程预期）：https://www.espressif.com/sites/default/files/documentation/esp32-c3_datasheet_en.pdf
- ESP-IDF RF 共存指南（D-03 BLE 惰性与 FTM 成功率关系）：https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-guides/coexist.html
- 堆内存 API（D-13 观察项）：https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-reference/system/mem_alloc.html
