# pass-radar v3 模型参数更新程序设计（基于采集数据）

> 日期 / Date: 2026-09-23
> 英文对等文档 / English peer: [pass-radar-v3-model-parameter-update-program.md](pass-radar-v3-model-parameter-update-program.md)
> 上游文档 / Upstream:
> - [slot2 批次采集与离线模型方案](pass-radar-v3-slot2-batch-capture-and-offline-model-plan.zh_CN.md)（数据采集与批次存储）
> - [最小条件矩阵与 Capture 编辑器](pass-radar-v3-minimal-condition-matrix-and-capture-editor.zh_CN.md)（场景标签定义）
> - [定向测距精度目标评估与 v3 设计方案](pass-radar-定向测距精度目标评估与v3设计方案.md)（精度目标与物理边界）
> - [Capture 内存方案](pass-radar-v3-capture-memory-debug-and-safe-allocation-plan.zh_CN.md)（数据链路格式）
>
> 本文回答 / This document answers:
> 1. 采集到的数据如何变成固件里的一组新参数（**程序方案**）；
> 2. 每个参数的更新公式从哪里来（**算法推导**），与 `radar_fusion.c` / `radar_sweep.c` / `radar_ftm.c` 中的实现一一对应；
> 3. 为什么这样拟合在逻辑上是自洽的（**一致性检查**），包括现有代码里两处与物理模型不一致的地方。

---

## ELI5 通俗摘要（不熟悉统计的读者先读这段）

把卡片想象成一把**用无线电猜距离的尺子**：信号越弱说明越远（RSSI），信号往返越久也说明越远（FTM），转一圈找信号最强的方向（扫盘）。出厂公式里的几个数字（比如"每远 10 倍信号弱多少"）是通用拍出来的，搬到你家就不一定准。

**这份文档做的事**：到已知距离的地方测一批数据 → 让电脑找出"最贴合这批数据的那几个数字" → 打包发回卡片。相当于给尺子重新对表。

**三条纪律**：

1. 卡片只收结果、不算数——算错了也不能在卡片上现场算；
2. 真距离必须用尺量（`measured_distance_m`），采集时按键填的 `target_m` 只是标签；
3. 新数字先"考试"（留出集验证），考不过就继续用出厂值。

**术语表**（按出现顺序，都是本文用到的行话）：

| 术语 | 人话 |
| --- | --- |
| RSSI | 信号强度读数，越远越弱 |
| FTM | 信号往返计时测距；较稳定但有固定零点偏差 |
| tx_ref_sym | "1 米处信号应该多强"的参考值 |
| n（路径损耗指数） | 信号每远 10 倍衰减的档位（室外约 2.2，室内 3.0 以上） |
| P25 | 一组数排序后 1/4 位置的值——宁可少报、不远报（偏保守） |
| IQR | 中间一半数据的宽度，看"这组数稳不稳" |
| median / MAD | 中位数 / 典型偏差；厚尾数据比均值/方差稳 |
| Huber 加权最小二乘 | 拟合直线时让离谱点"少说话"的算法 |
| 截距 / 斜率 | 直线的起点高度 / 倾斜度；此处 = 1 m 参考 / −10n |
| d_break（两射线折点） | 超过这个距离地面反射开始捣乱，公式换一段 |
| 混淆矩阵 | "实际是什么"对上"设备说是什么"的对错表 |
| macro-F1 | 每类错误一样重要的打分，防常见类淹没稀有类 |
| 留出集 | 考试用没教过的题，防死记硬背 |
| 置信区间宽度 | 估计值的"可能范围"，越窄越可信 |
| Wilson 95% 下界 | "至少这么准"的保守保证线（95% 把握） |

---

## 0. 结论摘要

1. **参数更新在电脑上完成，固件只消费结果。** 拟合、验证、版本管理都在 PC 端 Python 工具 `tools/model_fit.py` 完成；产出是一个带版本号和 CRC 的 `radar_model_params_t` 二进制块，通过现有 Debug 页串口通道或 NVS 预置写入固件。卡片上不做任何拟合（MVP 原则见上游文档 §6）。
2. **程序分五个阶段**：导出解析 → 数据清洗 → 分条件拟合 → 交叉验证 → 参数包生成。每个条件的参数独立拟合，最后合并成一张按 `(environment, link)` 索引的参数表。
3. **可更新的参数共 9 组**（对应固件常量的精确清单见 §3）：FTM 偏移 `ftm_offset`、RSSI 1m 参考 `tx_ref_sym`、路径损耗指数 `n(env)`、远场指数 `n_far` 与两射线折点 `d_break`、FTM 可信度门限（IQR/成功率）、遮挡分类阈值（K 因子/超量损耗/1s 峰峰）、扫盘谐波幅度门限、卡尔曼 Q/R、置信度分级边界。
4. **推导中发现两处现有实现与物理模型不一致，必须用数据拟合而不是沿用常量**：
   - `RADAR_TWO_RAY_BREAK_M = 45.0f`：按两射线公式 `d_break = 4π·h_t·h_r/λ`，1.2 m 持卡高度下折点约 **145 m**，45 m 对应的高度组合（h≈0.7 m）与实际持卡方式不符。折点必须从采集数据回归得出；
   - 采集记录里的 `ftm_distance_m` 是**偏移修正 + 5 点 P25 滤波后**的值，不能直接拿来做偏移拟合；偏移重拟合必须依赖 1 m 标定会话的原始 session median（该路径上 offset 已被置 0，见 §4.1.4）。
5. **拟合方法全部是鲁棒统计**：距离模型用按条件分组的加权最小二乘（Huber 损失），方向模型用环形统计与自相关，全部参数在留出集上验证，验证不过不上固件。

---

## 1. 系统位置与数据流

```text
┌─────────── 卡片（固件，已实现）───────────┐
│ Capture 页：30 s 采集 → 编码(≤4096 B)      │
│   → CRC → 压缩(miniz) → slot2 批次区追加    │
│ 长按 DOWN：批次导出（BATCH_* CSV 日志）     │
└──────────────────┬───────────────────┘
                   │ USB 串口日志
                   ▼
┌─────────── PC（tools/model_fit.py，本文）──┐
│ ① 解析 monitor-usb.py 日志 → DataFrame     │
│ ② 清洗（CRC 校验/样本数对账/标签完整性）    │
│ ③ 按 (environment, link) 分组拟合           │
│ ④ 留出集交叉验证 → 参数包 + 拟合报告        │
└──────────────────┬───────────────────┘
                   │ radar_model_params_t（版本号+CRC）
                   ▼
┌─────────── 卡片（固件，新增消费端）────────┐
│ 启动时加载参数包 → 替换默认常量             │
│ 版本不匹配/CRC 错 → 忽略并保持编译期默认值  │
└────────────────────────────────────────┘
```

数据来源只有一种：`tools/monitor-usb.py --output /tmp/pass-radar-usb-monitor.log` 产生的原始日志。工具不做任何"重新连接设备"的假设，输入永远是日志文件。

### 1.1 输入格式（与固件 emit_csv 精确对齐）

固件在 SAVE/EXPORT 时输出的 CSV（`radar_capture.c: emit_csv`）：

```text
BATCH_CAPTURE_META capture_id=… firmware=… boot_sequence=… reset_reason=… target_m=… records=… crc32=… generation=… compressed=…/… ftm_raw_median_m=… ftm_raw_iqr_m=… ftm_raw_sessions=… ftm_raw_bursts=…

（`ftm_raw_*` 四个字段自 FORMAT_VERSION 4 起存在，见 §10；无数据时 `ftm_raw_sessions=0`。）
BATCH_CAPTURE_CSV target_m,elapsed_ms,local_rssi,peer_rssi,peer_alive,ftm_valid,ftm_status,ftm_distance_m,ftm_spread_m,ftm_session_count,ble_ready,ble_peer_alive,ble_rssi,ble_age_ms,csi_frames,env_level
BATCH_CAPTURE_CSV <行…>
```

批次头部行（`radar_capture.c: BATCH_CAPTURE_CONDITION`）携带本次 run 的场景标签：

```text
BATCH_CAPTURE_CONDITION batch=… run=… scene=<name> schema=… env=… link=… obs=…
  space=… room=… size=… material=… furniture=… boundary=… height=… pose=…
```

模型工具把「一个 CONDITION 块之后、下一个 CONDITION 之前的所有 CSV 行」视为一个 run，并以 CONDITION 里的 target/env/link/上下文标签作为该 run 的条件变量。`MODEL_CANDIDATE` 行仅用于快速人工审阅，不作为数据源。

---

## 2. 观测模型反演（算法推导的出发点）

参数可辨识的前提是：**固件的每一个预测量都能写成"参数 × 观测量"的显式函数**。下面把固件当前的距离/方向链路逐段反演成拟合方程，符号与代码一一对应。

### 2.1 RSSI 距离模型（radar_fusion.c: radar_fusion_sym_distance）

固件预测：

```text
d̂ = 10^((tx_ref_sym − rssi_sym)/(10·n))          (近场, d̂ ≤ d_break)
rssi_break = tx_ref_sym − 10·n·log10(d_break)
d̂ = d_break · 10^((rssi_break − rssi_sym)/(10·n_far))   (远场, 室外)
```

反演（两边取对数）：

```text
log10(d_true) = (tx_ref_sym − rssi_sym)/(10·n)
⇔ rssi_sym = tx_ref_sym − 10·n·log10(d_true)
```

这是一个关于 `(tx_ref_sym, n)` 的**线性回归**：令 `y = rssi_sym`，`x = log10(d_true)`，则 `y = a + b·x`，其中 `a = tx_ref_sym`，`b = −10·n`。每个 `(environment, link)` 条件单独拟合一条直线，这就是"按条件分层"的数学形式。

**关键点：d_true 从哪来？** 权威真值是**实测距离 `measured_distance_m`**（卷尺/激光测距，随 sidecar 提交并通过时间对齐校验，见补救设计 §4.1）。`target_m` 只是采集前的预设**标签**（LABEL ONLY），正式拟合不得当真值使用。因此 RSSI 模型的拟合目标是实测距离，不是 FTM 输出——FTM 是另一个待标定的观测量，不能既当老师又当学生。

### 2.2 FTM 观测模型（radar_ftm.c）

一次 FTM session 产生 `valid` 个 RTT 原始距离，固件链路：

```text
raw_i = rtt_i · 1.49896e-4 m/ps
median = median(raw_1..raw_valid)          (radar_fusion_ftm_filter)
adj = max(0.1, median − ftm_offset)         (radar_fusion_ftm_apply_offset)
dftm = P25(history5 ∪ {adj})                (radar_fusion_ftm_low_quantile)
```

偏移的物理意义：RTT 时间戳包含收发链路的固定处理时延，等效为一个**加性常数** `b0`：

```text
median = d_true + b0 + ε ,  ε ~ 分布未知（厚尾，多径正偏）
ftm_offset = b0
```

所以偏移拟合就是估计 `b0 = median − d_true` 的鲁棒中心。FORMAT_VERSION 4 起，每个 run 的 header 聚合了原始 session median（§4.1.4、§10），是 P1 的规范数据源；其余距离的 FTM 观测用于评估偏移修正后的残差与 IQR 门限，不直接参与 `b0` 的中心估计。

### 2.3 远场分段与两射线折点

两射线地面反射模型的经典结论：直射径与反射径相消的破坏性干涉距离

```text
d_break = 4π·h_t·h_r / λ ,  λ ≈ 0.1224 m (2.449 GHz 信道 6)
```

持卡高度 1.2 m × 1.2 m 时 `d_break ≈ 148 m`；**但天线的有效相位中心不是卡片几何中心**，且人体持握会降低有效高度，所以真实折点应在 60–150 m 之间。固件当前写死 `45 m`，对应的等效高度组合 `h_t·h_r ≈ 0.44 m²`（约 0.66 m × 0.66 m），明显低于实际使用姿态。

**结论：`d_break` 不可以从理论上拍定，必须从数据估计。** 方法见 §4.2.2（分段回归搜索折点）。同理 `n_far`（当前 4.0）也由远场段回归给出。

### 2.4 方向模型（radar_sweep.c + main.c sweep_done）

扫盘把 360° 均匀映射到 `sweep_duration`，对每个信道分组拟合一次谐波：

```text
rssi(θ) = a + b·cosθ + c·sinθ ,  θ = 2π·t/duration
峰值角 = atan2(c, b)
```

可拟合参数与观测量：

| 量 | 来源 | 用途 |
| --- | --- | --- |
| 峰值角 vs 真方位 | 采集时记录的方位标签（PC 端录入，见 §6.3） | 系统偏置 `bias` 与残差 σ |
| 信道间投票分歧 spread | `radar_sweep_angle_vote` | 多径强度指标 → 遮挡分类特征 |
| 自相关周期 | `radar_sweep_estimate_period_ms` 输入样本 | 旋转周期自校正参数（提前停转检测窗口） |

方向模型要拟合的是**每个条件下的角误差分布**（bias 与 p50/p90 误差），以及误差与谐波幅度 `amp_db`、残差 `residual_db` 的关系——后者决定 `SWEEP_MAX_RMS_RESIDUAL_DB`（当前 5.0）和 `2·amp < 6 dB` 拒绝门限是否合适。

### 2.5 遮挡/环境分类（radar_obstruct.c, radar_fusion.c radar_level_classify）

分类器是固定决策树，参数是树上的阈值：

```text
L2 NLOS : ftm_rate < 0.3 且 mean_rssi < −80 | iqr > 2.0 | p2p > 10 | sel > 8
L0 LOS  : ftm_rate ≥ 0.7 且 iqr < 1.0 且 p2p < 6 且 sym_gap < 6 且 sel < 4
OBS_*   : excess_loss = 10·n·log10(d_rssi/d_ftm)，K 因子、p2p_1s/p2p_10s、mirror_err
```

采集数据同时带**用户标注**（`link` 条件标签）和**固件观测**（`observed_level/observed_obstruction`），两者对比就是混淆矩阵，混淆矩阵的错分格子直接指出哪个阈值需要移动。拟合方法见 §4.3。

### 2.6 卡尔曼滤波（radar_kalman.c）

```text
p ← p + q ;  k = p/(p+r) ;  x ← x + k(z − x) ;  p ← (1−k)p
```

`q=0.15, r=4.0` 是编译期常量。它不产生系统性偏差，只影响时延与方差折中：r 越大越平滑但跟踪越慢。用采集的 raw RSSI 序列做**离线重放**，扫描 `(q, r)` 网格最小化 `|x_t − z_{t+1}|` 的预测误差（或等效地匹配 1 s 窗口真实方差），见 §4.4。

---

## 3. 可更新参数清单（与固件常量一一对应）

| # | 参数 | 固件现状 | 来源代码 | 拟合方法 | 默认安全值 |
| --- | --- | --- | --- | --- | --- |
| P1 | `ftm_offset` | NVS 标定值 | `radar_ftm.c: s_offset_m` | 1 m 标定会话 median−1.0 的鲁棒中心（§4.1.4） | 不变（保持机内标定） |
| P2 | `tx_ref_sym` | NVS 标定值 | `main.c: s_tx_ref_sym` | 1 m 会话对称 RSSI 截尾均值（复算校验） | 不变 |
| P3 | `n` (env=OUTDOOR/INDOOR/OBSTRUCTED) | 2.2 / 3.0 / 3.5 | `radar_fusion.c: radar_env_path_loss_n` | 每条件 RSSI↔log10(d) 回归斜率（§4.2.1） | 编译期值 |
| P4 | `tx_ref_per_env`（新增：每环境 1m 参考） | 单一 tx_ref | §4.2.1 截距 | 每条件回归截距 | = P2 |
| P5 | `d_break` | 45.0 | `RADAR_TWO_RAY_BREAK_M` | 室外远场分段回归搜索（§4.2.2） | 45.0 |
| P6 | `n_far` | 4.0 | `RADAR_FAR_N` | 折点以上段回归斜率 | 4.0 |
| P7 | FTM 门限（IQR 1.0/2.0，rate 0.3/0.7，suspicious 1.8×+2m） | 编译期 | `radar_level_classify`, `radar_fusion_ftm_suspicious` | FTM 有效/无效与误差分位数对齐（§4.3.2） | 编译期值 |
| P8 | 遮挡阈值（K、excess、p2p_1s/10s、sel） | 编译期 | `radar_obstruct.c`, `radar_level_classify` | 混淆矩阵驱动的阈值搜索（§4.3.1） | 编译期值 |
| P9 | 扫盘偏置 `sweep_bias_deg` + 门限（residual 5.0，amp 6.0） | 偏置未实现 / 门限编译期 | `SWEEP_MAX_RMS_RESIDUAL_DB` 等 | 先估环形中值偏置（D1），再做角误差 vs (amp,residual) 分桶/ROC（§4.5） | 偏置 0° / 门限编译期值 |
| P10 | 卡尔曼 (q, r) | 0.15 / 4.0 | `radar_kalman.c` | raw 序列离线重放网格搜索（§4.4） | 0.15 / 4.0 |

P1/P2 属于**机内标定**，PC 工具只做复算校验（用导出数据验证机内值是否合理），不直接改写；P3–P10 属于**模型参数包**，由 PC 工具产出。

### 3.1 参数包格式（新增 `radar_model_params.h`）

```c
#define RADAR_MODEL_PARAMS_MAGIC   0x524D5031u  /* RMP1 */
#define RADAR_MODEL_PARAMS_VERSION 1u

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint32_t params_crc32;      // 除本字段外全块的 CRC32
    uint32_t fitted_at_unix;    // 拟合时间
    char     source_firmware[16]; // 采集固件版本
    uint32_t device_pair_hash32;  // D1：设备对标识(MAC 对散列)，sweep_bias_deg 归属
    // P3/P4: per-env 路径损耗(索引 = radar_env_t)
    float    path_loss_n[3];
    float    tx_ref_per_env[3];
    // P5/P6
    float    two_ray_break_m;
    float    far_path_loss_n;
    // P7
    float    ftm_iqr_good_m, ftm_iqr_bad_m;
    float    ftm_rate_low, ftm_rate_good;
    float    ftm_suspicious_ratio, ftm_suspicious_margin_m;
    // P8
    float    obs_k_foliage_db, obs_k_static_db;
    float    obs_excess_min_db, obs_excess_reflect_db;
    float    obs_p2p1s_dynamic_db, obs_p2p1s_static_db;
    float    obs_sel_los_max_db;
    float    level_mean_rssi_obstructed_db;
    // P9
    float    sweep_bias_deg;    // D1：设备对级角度系统偏置
    float    sweep_max_rms_residual_db;
    float    sweep_min_prominence_db;
    // P10
    float    kalman_q, kalman_r;
    uint8_t  reserved[112];     // 补齐到 256 B 整块（含 sweep_bias_deg 与设备对标识）
} radar_model_params_t;
```

加载规则：CRC 通过且 `version` 兼容才应用；任何字段越界（如 `n` 超出 [1.6, 5.0]）整体拒绝，回退编译期默认。越界区间表与参数包一起存档在文档 §7。

---

## 4. 各参数的拟合算法推导

统一记号：一个 run = 一次 30 s 采集，含条件标签 `(env, link, obs, space, …)` 与 `N≈100` 条记录（有效采样节拍 300 ms，见 §4.1）；数据集 D = 所有有效 run；条件组 `G(env, link)` 是 D 的划分。

### 4.1 数据清洗（拟合前必须全部通过）

1. **完整性对账**：期望 `records = 100`（30 s 窗口 ÷ 300 ms 有效节拍——250 ms 采样门限被 100 ms poll 栅格量化成 300 ms，固件上限 128），`95~102` 视为完整，超出范围的 run 标记 `SUSPECT`；
2. **CRC**：`BATCH_CAPTURE_META` 的 `crc32` 与解码后重算一致，不一致整 run 拒绝；
3. **条件标签合法**：`scene_id` 能被 `radar_capture_condition_scene_id` 反解，上下文字段在合法域内；
4. **样本时序**：`elapsed_ms` 单调、间隔 ≈300 ms（合法范围 250~450 ms；出现 >500 ms 间隔记一次丢拍）；
5. **FTM 字段语义**：`ftm_valid=0` 时 `ftm_distance_m` 无意义（固件保持旧值/无效值），拟合时必须剔除；
6. **重复 run**：同条件同距离多次 run 全部保留，进入方差分析；离群 run 由拟合器按 1.5×IQR 判定后**降权**而非丢弃（保留方差信息）。

### 4.1.4 FTM 偏移（P1）的正确数据源

固件采集路径上 `ftm_distance_m` 是 `apply_offset(median, s_offset_m)` 之后、再经 5 点历史 P25 的值：

```text
capture 里的 ftm = P25(hist ∪ {median − offset})
```

用这个量反推 offset 会把 P25 与旧 offset 全部耦合进去，**逻辑上不可辨识**。

自 FORMAT_VERSION 4 起（实施见 §10），每个 run 的 capture header 记录 run 级原始 session 统计：offset/P25 之前各 session median 的 median、IQR、完成的 session 数与有效 burst 总数。P1 拟合直接使用：

```text
b0_run     = ftm_raw_median_m − measured_distance_m   （每 run 一个样本；真值来自 sidecar 实测）
ftm_offset = median(b0_run)                           （跨 run 鲁棒中心）
```

无 `measured_distance_m` 实测真值的 run 不参与 P1；`target_m` 只作分组标签（R-03，见补救设计 §4.1）。

跨 run MAD > 0.5 m 时报告标定不可信。早先设想的「offset=0 条件下 1 m 采集」近似流程废止，拟合报告不再出现 `offset_estimate=approximate`。

一致性校验：`b0` 理论上是与距离无关的加性常数。若拟合发现 `b0_run` 随距离系统性漂移（>0.3 m/十倍距），说明存在距离相关处理时延，报告中标记该异常并回退为仅信任 1 m run。

### 4.2 RSSI 距离模型（P3/P4/P5/P6）

#### 4.2.1 每条件线性回归

对条件组 `G`（如 `env=INDOOR, link=LOS`）：

```text
样本: (x_i, y_i) = (log10(target_m_i), sym_rssi_i)
     sym_rssi = 0.5·(local_rssi + peer_rssi)   (双向有效时)
     sym_rssi = local_rssi                     (仅单向, 降权 0.5)
模型: y_i = a_G + b_G·x_i + ε_i
     a_G = tx_ref_per_env[env] 的组内修正
     b_G = −10·n_G  ⇒  n_G = −b_G/10
```

估计用 **Huber 加权最小二乘**（δ = 1.345·MAD），因为厚尾多径样本不能用普通 OLS：

- 单向样本（peer_rssi 无效）权重 ×0.5：单向 RSSI 含本端天线方向图偏差，是对称平均的有偏版本；
- 权重还包括 FTM 无关性：`ftm_valid=1` 的样本不降权（RSSI 与 FTM 独立）；
- 输出 `n̂_G = −b̂_G/10` 及其置信区间，n 超出 [1.6, 5.0] 时拒绝该条件组的拟合（数据有系统性问题）。

> **置信宽度算例（决定 D7 距离点要求）**：`n̂` 的 95% 区间宽度 ≈ `2·t·σ̂/(10·√Sxx)`。8 个距离点（1/3/5/8/10/15/20/30 m，log10 跨度 1.48）在残差 σ̂=1.5 dB 时宽度 ≈0.58、σ̂=3 dB 时 ≈1.16；5 个点挤在 1~8 m（跨度 0.90）宽度 ≈0.91·σ̂。所以**每条件 ≥5 个距离点且 log10 跨度 ≥1.4**（如 1/3/5/10/20/30 m）是门限有机会通过的下限；室内 1~8 m 跨度不足属预期，室内 P3 回退编译期 n。

`tx_ref_per_env` 的融合：回归截距 `â_G` 是「该条件下 1 m 对称 RSSI」的估计。同一 `env` 下不同 `link` 的截距差异反映遮挡对 1 m 参考的偏移，取 `env` 内按样本数加权的截距均值作为 `tx_ref_per_env[env]`；机内 NVS 标定值（P2）仍优先——参数包的值只在 NVS 无标定时使用。

#### 4.2.2 两射线折点与远场指数（仅室外 LOS）

对 `env=OUTDOOR, link=LOS` 的数据做**分段回归**：

```text
模型: y = a + b1·x                         (x ≤ log10(d_break))
      y = c + b2·x                        (x >  log10(d_break))
      且要求 c = a + (b1−b2)·log10(d_break) (折点连续)
```

在 d_break ∈ [30, 200] m 网格上搜索使总残差平方和最小的折点：

- 最优点 `d̂_break` → P5；远场段斜率 `b̂2` → `n_far = −b̂2/10` → P6；
- 需要 `d > 45 m` 的采集点支撑（采集矩阵的远场扩展档 60/80/100 m，属 v3.1 远距阶段，见 slot2 批次采集计划 §3.1；未采集前 P5/P6 保持编译期值）；
- 如果数据只覆盖到 30 m，折点拟合自由度不足，**保持编译期值并在报告中标记 `d_break=not-identifiable`**，不允许用近场数据外推折点。

### 4.3 分类阈值（P7/P8）

#### 4.3.1 遮挡分类：混淆矩阵驱动的阈值搜索

每个样本同时有：

- 用户真值标签：`link`（LOS/静态衰减/动态衰减/多径）与 `obs`（NONE/HUMAN_STATIC/HUMAN_DYNAMIC/FOLIAGE/WALL/METAL）；
- 固件特征：从 run 记录可重算 `ftm_rate`（run 内 `ftm_valid` 占比）、`ftm_iqr`（run 内 FTM 距离 IQR）、`rssi_p2p`（run 内 RSSI 峰峰）、`k_factor`/`p2p_1s`/`p2p_10s`（由 raw 序列重放 `radar_obstruct.c` 函数得到）、`excess_loss = 10·n·log10(d_rssi/d_ftm)`；
- 固件判定：`observed_level` / `observed_obstruction`。

流程：

1. 构建混淆矩阵 M[user_label, observed]；
2. 对每个待调阈值 t（如 `obs_p2p1s_dynamic_db`），沿其维度扫描候选值，最大化 macro-F1（各类等权，避免多数类淹没动态遮挡）；
3. 约束：阈值必须保持决策树的**单调序**（如 `OBS_EXCESS_MIN_DB < OBS_REFLECT_EXCESS_DB` 的语义关系），扫描空间按约束剪枝；
4. 若某两类在任何阈值下都不可分（F1 < 0.5），在报告中标记为"特征不足"，提出需要新增特征（见 §5），而不是硬凑阈值。

#### 4.3.2 FTM 质量门限（rate/IQR）

目标：门限应使「FTM 有效且用于融合」的样本距离误差最小。方法：

- 以 `d_ftm − target_m` 为误差，计算误差绝对值的 P50/P90；
- 把 `ftm_rate`、`ftm_iqr` 作为二维特征，找使得 P90 误差 ≤ 目标值（室外 1.5 m / 室内 3 m，来自精度目标文档 §3.1）的最大门限开度——门限越松样本越多方差越小，越紧偏差越小，取 P90 约束下的最松点；
- `suspicious` 比例门限（1.8×+2 m）用「FTM 反射径误报率」直接校准：统计 `d_ftm > d_rssi·1.8+2` 但 `target` 与 `d_rssi` 一致（差 <1 m）的样本占比，若占比 <1% 则门限过紧，按比例放松。

### 4.4 卡尔曼参数（P10）

raw RSSI 环形缓冲（`radar_espnow_raw_history`）在采集里逐条落盘（`local_rssi` 即 raw）。离线重放：

```text
对每组 (q, r) ∈ 网格:
    x_t = kalman(x_{t−1}, z_t)
    评估: (a) 一步预测误差 Σ|x_t − z_{t+1}|
          (b) 稳态输出方差 Var(x)
          (c) 阶跃跟踪时延（对 run 内人工标记的遮挡事件）
选择: 最小化 a，约束 b ≤ 1.5×基线方差、c ≤ 1 s
```

注意约束的意义：只优化预测误差会得到无限平滑（b、c 发散），所以 b、c 是硬约束而不是目标项。

### 4.5 方向模型（P9 与角误差评估）

方向数据的真值来自**采集时的方位标注**（PC 端录入，格式见 §6.3）。对每个带方位标签的 run：

```text
偏置 bias = e 的环形中值（跨 run 汇总，D1；per-设备对）
误差 e' = 环形差(e, bias)                    （先校正系统偏置，再统计残差）
按 (amp_db, residual_db, spread_deg, env) 分桶统计 e' 的 P50/P90
```

输出：

1. 各桶 P50/P90 表 → 判定当前 `residual ≤ 5.0` 与 `2·amp ≥ 6` 门限下哪些桶仍在产出可信方向，哪些桶应该拒绝（拒绝率 vs 误报率的 ROC 曲线，选 Youden 最优点）；
2. 若发现 `spread_deg` 与角误差强相关（相关系数 >0.5），把 spread 升级为角度置信度的直接输入（当前只在 >20°/>45° 时降档，可改为连续映射）；
3. `sweep_bias_deg`（环形中值偏置）写入参数包——天线/人体不对称是系统项，必须先校正再谈门限（80% 目标评审 D1）。偏置按**设备对**标定，换设备对必须重估。

### 4.6 交叉验证与准入

- 数据集按 run 划分训练/留出集（80/20，**按场地分层**，同一场地的 run 不跨集，防止环境记忆泄漏）；
- 每个参数包必须通过：
  1. 留出集 RSSI 距离误差 P50 不劣于编译期默认参数（按条件分组比较）；
  2. `n̂_G` 置信区间不跨 0，宽度三级门限：≤0.4 直接采用（标 `n_ci_tight`）；(0.4, 0.8] 采用但标 `n_ci_wide` 并注明外推风险；>0.8 拒绝、回退编译期值（宽度算例见 §4.2.1 注）；
  3. 分类阈值变化不使任何一类 F1 下降超过 0.05；
  4. FTM offset 估计的 MAD ≤0.5 m；
- 任一不通过：该参数保持默认值并只在报告中标 FAIL，**参数包整体仍可发布但只包含通过的参数**（部分应用优于全有全无）；
- **80% 验证口径（D3/D4，与 [80% 目标评审 §7](pass-radar-v3-accuracy-goal-80pct-review.zh_CN.md) 分级判据一致）**：`model_fit.py verify` 按分级判据（T1~T4/A1~A2）输出每场景覆盖率与 Wilson 95% 下界；**分母 = 全部有效测量请求（拒绝与降级输出计入分母）**；**测量值 = run 显示序列稳定段中值（动态场景 3 s 滑窗）**；下界 ≥0.80 才记达标，否则该场景承诺自动降档。

---

## 5. 现有采集数据未覆盖、拟合需要的增量需求

推导过程中发现以下观测量对拟合有价值但当前 capture 格式没有，列入**下一版 FORMAT_VERSION 需求**（不阻塞本期）：

| 需求 | 用途 | 优先级 |
| --- | --- | --- |
| FTM 原始 session 统计（median/IQR/bursts） | P1 偏移的严格可辨识 | ✅ 已交付（FORMAT_VERSION 4，run 级聚合，§10） |
| 方位真值标签（采集前 UI 录入 8 方位） | P9 与角误差评估 | 高（PC 端录入可替代） |
| run 内 `d_rssi` 融合中间量 | excess_loss 复算免重放 | 低 |
| 扫盘样本子集（谐波拟合输入） | P9 复算免重放 | 低（可用 SWEEP_DONE 日志替代） |

---

## 6. PC 工具程序设计（tools/model_fit.py）

### 6.1 命令行

```bash
# 解析 USB 监控日志 → dataset.parquet
python3 tools/model_fit.py parse /tmp/pass-radar-usb-monitor.log -o dataset/

# 数据质量报告（每个 run 一行：对账/CRC/标签/样本数）
python3 tools/model_fit.py report dataset/

# 拟合全部参数 → params_v1.bin + fit_report.html
python3 tools/model_fit.py fit dataset/ -o build/params_v1.bin

# 用留出集单独验证一个参数包
python3 tools/model_fit.py verify dataset/ build/params_v1.bin
```

依赖：Python 3.10+，`numpy`/`scipy`/`pandas`（解析与拟合），`jinja2`（报告）。无 GPU、无网络。

### 6.2 模块划分

```text
tools/model_fit/
  parser.py      # BATCH_*/CAPTURE 日志 → run DataFrame（正则与固件 emit_csv 严格同步）
  clean.py       # §4.1 清洗规则，输出 SUSPECT 标记
  features.py    # 重放 radar_obstruct/radar_fusion 特征（纯 numpy 移植，与 C 一一对应）
  fit_rssi.py    # §4.2 回归（Huber, 分段折点搜索）
  fit_ftm.py     # §4.1.4 / §4.3.2
  fit_class.py   # §4.3 阈值搜索
  fit_kalman.py  # §4.4 网格重放
  fit_sweep.py   # §4.5
  pack.py        # 生成 radar_model_params_t（CRC + 版本 + 越界校验）
  report.py      # 拟合报告（混淆矩阵/残差图/参数对比表）
```

特征重放与 C 代码的一致性由**对拍测试**保证：同一组合成输入分别进 C（host test）与 Python，输出差 ≤1e-4。

### 6.3 方位真值录入

`parse` 子命令支持辅助标注文件（手写 YAML 或采集时口述 PC 记录）：

```yaml
batch: 493512032
runs:
  - run: 3
    bearing_deg: 45           # 对端相对本端的真实方位（0°=磁北或约定参考）
    measured_distance_m: 5.0  # 卷尺/激光实测真值（target_m 仅是预设标签）
    window_ms: [0, 38000]     # 该实测值覆盖的采集时间窗，用于与记录对齐
```

实测距离的对齐校验（时间窗重叠、缺锚拒绝）以补救设计 §4.1 的 sidecar 规范为准（`DISTANCE_SIDECAR_*`）。

没有方位标注的 run 只参与距离/分类拟合，不参与方向拟合。

### 6.4 参数包交付

`fit` 产出两个文件：

1. `build/params_vN.bin`——`radar_model_params_t` 原始块；
2. `fit_report.html`——每个参数的：拟合值/默认值/置信区间、留出集对比、失败项与原因。

固件侧消费（新增，约 100 行 C）：Debug 页新增 `PARAMS` 行显示当前参数包版本与 `fitted_at`；`radar_model_params_apply()` 在 `radar_fusion`/`radar_sweep`/`radar_obstruct`/`radar_kalman` 初始化前调用，把 getter（如 `radar_env_path_loss_n`）改为查参数包。所有 getter 保持"未加载参数包时返回编译期默认"的语义。

---

## 7. 参数越界保护表（固件加载时的硬校验）

| 参数 | 合法区间 | 越界行为 |
| --- | --- | --- |
| path_loss_n[*] | [1.6, 5.0] | 整包拒绝 |
| tx_ref_per_env[*] | [−60, −30] dBm | 整包拒绝 |
| two_ray_break_m | [30, 300] | 整包拒绝 |
| far_path_loss_n | [3.0, 6.0] | 整包拒绝 |
| ftm_rate_low / good | [0.05, 0.5] / [0.5, 0.95] 且 low < good | 整包拒绝 |
| ftm_iqr_good/bad | [0.3, 2.0] / [1.0, 4.0] 且 good < bad | 整包拒绝 |
| kalman_q | [0.01, 1.0] | 整包拒绝 |
| kalman_r | [1.0, 20.0] | 整包拒绝 |
| sweep 门限 | residual [2, 10], prominence [2, 15] | 整包拒绝 |
| sweep_bias_deg | [−30°, 30°] | 整包拒绝 |
| 观测阈值组 | 各自在编译期值 ±50% 内 | 整包拒绝 |

---

## 8. 一致性检查记录（推导中确认/发现的问题）

| # | 检查项 | 结论 |
| --- | --- | --- |
| 1 | RSSI 模型对 (tx_ref, n) 线性可辨识 | ✅ 对数反演后是标准线性回归 |
| 2 | FTM offset 可辨识性 | ⚠️ capture 记录的 ftm 已过 offset+P25，不可直接反推；需 `ftm_raw_session_m`（§4.1.4） |
| 3 | `d_break=45` 与两射线公式一致性 | ❌ 1.2 m 持卡高应 ≈145 m；必须数据估计，禁止沿用理论常量 |
| 4 | 方向模型的时间→角度映射 | ✅ 已有周期自校正；拟合对象是误差分布与门限，不是映射本身 |
| 5 | 分类器特征可从 capture 重放 | ✅ p2p_1s/p2p_10s/k_factor 均只依赖 raw RSSI 序列 |
| 6 | 单向 RSSI 样本的处理 | ✅ 降权 0.5，不剔除（保留量程边缘信息） |
| 7 | 用户 target_m 作为真值 | ✅ 已修正：真值 = `measured_distance_m` sidecar（补救设计 §4.1），`target_m` 仅作标签（LABEL ONLY），无实测真值的 run 不进正式拟合 |
| 8 | 训练/留出划分 | ✅ 按场地分层，防同场地泄漏 |

---

## 9. 实施顺序

1. **固化数据格式**：按 §10 实施 FORMAT_VERSION 4（run 级 FTM 原始 session 聚合），同步 batch/export/host 测试；
2. **PC 解析与清洗**：`model_fit.py parse/report` 对真实 USB 日志跑通（用已有测试批次数据验收）；
3. **第一批拟合**：P3/P4（每条件 n 与 tx_ref）+ P1 复算校验——这两项数据需求最低，先出报告；
4. **室外远场采集**：60/80/100 m 档补齐后拟合 P5/P6；
5. **分类阈值**：混淆矩阵达标（每类 ≥8 个有效 run）后拟合 P7/P8；
6. **方向与卡尔曼**：方位标注流程跑通后拟合 P9/P10；
7. **固件参数包消费端**：`radar_model_params.h/.c` + Debug 页显示 + host 测试；
8. **回归验收**：按 [测试设计与用例](pass-radar-v3-测试设计与用例.md) 的 F- 系列用例在新参数包下复测。

每一步的产出物（数据集、报告、参数包）按 `docs/assets/fit-history/` 归档，参数包文件名带版本与日期。

---

## 10. FORMAT_VERSION 4 实施方案：FTM 原始 session 记录

### 10.1 方案选择：run 级聚合，不做记录级字段

P1 需要的是 offset 修正与 P25 之前的 session median（§2.2）。两个候选：

| 方案 | 改动面 | 代价 |
| --- | --- | --- |
| A. 记录级字段（逐 tick 存 raw median + bursts） | codec +2 字段，worst record 32→35B，MAX_PAYLOAD 4096→4480 | payload 4480 对齐到 8192，flash 每 capture 12288→16384（翻倍），批次余量吃紧；内存/格式全面重验 |
| B. run 级聚合写入 capture header | header 从 reserved[40] 划出 12B；codec/内存/flash 全部不变 | run 内逐 session 序列不可恢复（只存 median/IQR/count/bursts） |

§4.1.4 的拟合消费量是 run 级 `b0_run`，方案 A 存的逐 tick 重复值对 P1 拟合没有增量信息。**选定方案 B**，同时保留 v3 批次读取兼容。

### 10.2 数据格式增量

batch capture header（总长 112B 不变，从 reserved 划分）：

```text
float    ftm_raw_median_m;   // run 内各 session 原始 median 的 median；无数据时 sessions=0
float    ftm_raw_iqr_m;      // session median 的 IQR
uint16_t ftm_raw_sessions;   // 完成session数（0=无原始数据；v3兼容读出即0）
uint16_t ftm_raw_bursts;     // 有效 burst 总数
```

- `RADAR_CAPTURE_BATCH_FORMAT_VERSION` 3→4；读取侧接受 v3 与 v4（v3 的 reserved 全零 → sessions=0 → 自动视为无原始数据），已保存的 v3 记录不会被丢弃；
- NVS fallback 头 `capture_storage_header_t` 同步 +12B（76→88），`CAPTURE_STORAGE_VERSION` 1→2（旧 blob 走既有 invalid_header 拒绝路径，行为与当前一致）；
- 导出 META 行新增 `ftm_raw_median_m=… ftm_raw_iqr_m=… ftm_raw_sessions=… ftm_raw_bursts=…`；逐行 CSV 不变。

### 10.3 采集时序语义

- `radar_ftm.c` 成功路径在 `radar_fusion_ftm_filter` 之后、`apply_offset` 之前记录 `median` 与 `valid`，并以完成时的 session 计数为 `raw_session_id`；
- capture tick 以 `raw_session_id` 去重追加；COUNTDOWN/SETTLE 期间完成的 session 同样计入（同一静态距离下仍是有效 b0 样本）；
- `radar_capture_start` 快照当前 id，只聚合本 run 内新完成的 session；
- 容量上界按代码常量推导：38 s 窗口 ÷ FTM_PERIOD_US 2 s = 最坏 19 个完成 session（超时失败循环 3.5 s/次反而更慢），v4 聚合容量 16 **不满足**该上界——静态验证报告 §2.2 确认存在溢出静默丢数据风险，补救设计（FORMAT_VERSION 5）将容量提升到 32 并增加 `OVERFLOW` 标志；
- probe session 与 regular session 混合计入是 v4 的已知缺陷，v5 起 P1 只使用 regular session，PC 端不得按 bursts 加权（v4 的 bursts 仅为求和诊断值，权重语义未定义）。

### 10.4 代码修改位置清单

| 文件 | 修改 |
| --- | --- |
| `main/radar_ftm.h` / `radar_ftm.c` | 新增 `radar_ftm_raw_session_id / radar_ftm_last_raw_session_m / radar_ftm_last_raw_bursts`；成功路径记录原始 median/bursts |
| `main/radar_capture.h` | observation 结构 +`ftm_raw_session_id / ftm_raw_session_m / ftm_raw_bursts` |
| `main/main.c` | Capture 页 observation 填充新字段 |
| `main/radar_capture_session_agg.h/.c`（新增） | 纯 C 聚合模块：按 id 去重追加，输出 median/IQR/count/bursts |
| `main/radar_capture.c` | start/init 复位聚合；tick 追加；save 时写入 NVS 头与 batch meta；export 映射与 META 行扩展；NVS 头 76→88、版本 2 |
| `main/radar_capture_batch.h/.c` | capture header 新 4 字段、meta 结构透传、FORMAT_VERSION 4、header_valid 接受 v3\|v4 |
| `main/radar_capture_budget.h` | STORAGE_HEADER_BYTES 76→88 |
| `tests/test_radar_capture_session_agg.c`（新增） | 聚合单测：0/1/奇偶个数、去重、溢出、IQR 边界 |
| `tests/test_radar_capture_codec.c` | 仅 budget 断言 76→88（codec 本身零改动） |
| `tests/test_radar_capture_batch.c` | header 字段透传、v3/v4 兼容、CRC 兼容断言 |

明确不改动：`radar_capture_codec.h/.c`（记录布局、MAX_PAYLOAD=4096、worst=32B）、flash span 12288、批次容量 160、records 缓冲 2560、UI、场景矩阵、编辑器。

### 10.5 内存与容量复核（改动后）

```text
scratch = 88 + 4096 = 4184；+2048 余量 = 6232 < 8192 实测连续块下限 ✓
records = 2560 不变 ✓
flash 每 capture = 4096 + align(≤4096) + commit 4096 = 12288 不变 ✓
批次空间 = 2,740,224 / 12288 ≈ 222，受 160 上限约束 ✓
聚合模块静态内存 ≈ 120B BSS
```

### 10.6 验收

host 全套 + 静态检查 + 正式编译 + sim boot；真机 META 行出现 `ftm_raw_sessions≥1` 且 `ftm_raw_median_m` 非负即确认链路打通。
