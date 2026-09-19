# pass-radar 定向/测距精度目标评估与 v3 设计方案

> 目标读者：pass-radar 维护者
> 日期：2026-09-19
> 输入文档：
> - [定位增强方案调研与设计](pass-radar-定位增强方案调研与设计.md)（v2 调研）
> - [pass-radar 设计文档](pass-radar-design.zh_CN.md)（v1/v2 实现说明）
> - [AI 硬件开发指南](../hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.zh_CN.md)、[产品规格](../hardware-design/specifications.zh_CN.md)、[硬件设计目录说明](../hardware-design/README.zh_CN.md)
> - [双卡实测指南及结果](pass-radar-device-test-guide.zh_CN.md)（v2.1 真机结果）
> - 当前代码：`main/radar_ftm.c`、`radar_fusion.h`、`radar_sweep.h`、`radar_ble.h`、`radar_protocol.h`
>
> 评估问题：在该硬件上，"各种环境下方向误差 ≤15°、测距误差 ≤1m" 是否可达？若不可达，最接近的可达目标是什么，如何实现？
>
> **配套文档（v3 系列）**：
> - **一期 v3（≤ 15 m）**
>   - [v3 详细设计与实现指南](pass-radar-v3-详细设计与实现指南.md) —— 模块接口、伪代码、参数表、实施顺序（面向初级工程师）
> - **二期 v3.1（15~100 m 远距与树丛/围墙遮挡，一期 M3 验收后启动）**
>   - [远距与遮挡场景分析](pass-radar-v3-远距与遮挡场景分析.md) —— 两射线折点、遮挡损耗、识别与修正算法（"为什么"）
>   - [v3.1 远距与遮挡实施指南](pass-radar-v3.1-远距与遮挡实施指南.md) —— 逐函数代码、接入点、参数表、DoD（"怎么做"）
> - **两期共用**
>   - [v3 测试设计与用例](pass-radar-v3-测试设计与用例.md) —— host 单测、机上自测、外场统计矩阵；标 P2 的用例属二期

---

## 0. 结论摘要

**"各种环境下 ±15° / ±1m" 在当前硬件上不可达。** 这不是滤波或算法成熟度问题，而是由四条硬件边界共同决定的物理极限：

1. **单 PCB 天线**：没有相位差可测，任何真测角（AoA/AoD）都不存在；方向只能靠"人体遮挡 + 旋转"这种间接观测量。
2. **无 IMU / 无磁力计**：旋转角度只能用 "时间 ÷ 总时长 × 360°" 推算，用户转速不均匀直接变成角度误差（±10°~30°），这是定向误差预算里最大且与射频无关的一项。
3. **20/40 MHz 带宽的 2.4 GHz 射频**：FTM 时延分辨率对应 7.5~15 m 的距离尺度，室内墙面/家具反射与直射径在时域上不可分离，多径偏差（恒为正向）无法在物理层消除。
4. **无可用扩展接口**：GPIO 0–10、18–21 已全部占用（11–17 为 Flash），透明壳无外露接口，无法外挂 UWB 模块、天线开关或 IMU；只能纯软件。

在这些约束下，经量化误差预算（第 3 节）与文献对照，**最接近的可达目标**如下（p50 = 中位误差，p90 = 90% 分位误差）：

| 环境 | 方向 | 距离 | 达标情况 |
| --- | --- | --- | --- |
| 室外开阔、视距、≤15 m | p50 ≈ 10°，p90 ≈ 20° | p50 ≈ 0.8 m，p90 ≈ 1.5 m（FTM 主导） | **中位数达标**；p90 不达标 |
| 室外、15~30 m | p50 ≈ 15°，p90 ≈ 30° | ±15%~30%（RSSI 主导，FTM 间歇） | 方向勉强、距离不达标 |
| 室内同房间、视距、≤8 m | p50 ≈ 20°，p90 ≈ 40° | p50 ≈ 1.5 m，p90 ≈ 3 m | 均不达标，但"可用" |
| 室内隔墙 / 非视距 | 仅报 ±45° 扇区（象限） | 仅报"近 <3 / 中 3~8 / 远 >8 m"三档 | 不达标，改为定性输出 |
| >30 m | 不可用 | BLE 哨兵：有/无 | — |

因此建议把产品目标改写为：**"室外视距 15 m 内：方向中位误差 ≤15°、距离中位误差 ≤1 m；室内自动降级为扇区 + 距离档，并在屏幕上诚实标注置信度。"** 这是本文 v3 设计要兑现的承诺。

另一个必须先说的结论：**v2.1 真机测试表明当前管线并未按设计工作**（FTM 从未生效、角度不变、室内距离 1~5 m 跳变、扫盘中误报 OUT OF RANGE）。因此 v3 的第一阶段不是"再加观测量"，而是**先把已有观测量调通并在机上可诊断**（第 5 节）。

---

## 1. 现状核对

### 1.1 v2 已实现的内容（代码核对）

| 模块 | 已实现 | 备注 |
| --- | --- | --- |
| `radar_espnow.c` | APSTA 模式；ESP-NOW 20 Hz 广播；v2 协议（对端 RSSI 回传、AP MAC、BLE identity、扫盘同步）；扫盘期 1/6/11 快跳（250 ms 驻留） | 快跳发生在 APSTA 模式下，SoftAP 固定信道 1 |
| `radar_ftm.c` | 隐藏 SoftAP + FTM responder；MAC 小者作 initiator；每 2 s 一次 16 帧 session（`burst_period=0`）；中值 + IQR；BLE 共存"踢一脚"补丁 | **没有 FTM 固定偏移（cm0）标定**；只有单向 initiator |
| `radar_fusion.c` | 双向 RSSI 对称平均；FTM/RSSI 加权 0.7/0.3；反射径判别（>1.8× 且 >+2 m）；环境 n 自适应（2.2/3.0/3.5）；置信度 HIGH/MED/LOW/OOR；1 m RSSI 标定 | 环境切换无滞回 |
| `radar_sweep.c` | 抛物线插值；峰显著性；一次谐波最小二乘拟合；旋转周期自校正；按信道分组测角 + 环形中值投票 | 纯逻辑，有 host 测试 |
| `radar_ble.c` | NimBLE 扩展广播（Coded PHY）+ 约 10% 被动扫描；哨兵 | 与 Wi-Fi 常驻共存 |

### 1.2 v2.1 真机结果（摘自实测指南结果表）

| 现象 | 对设计的含义 |
| --- | --- |
| 标定后从未出现 `HI`，屏幕上**从未出现 `FTM` 字样** | FTM 观测量从未进入融合；v2 精度提升的主来源缺失 |
| 3.5 m 处距离在 1~5 m 之间跳动；室内"数字跳动厉害" | 纯 RSSI 路径 + 环境 n 在 2.2/3.0 间切换（无滞回）→ 距离 ×1.5 级跳变 |
| 雷达红点永远在正上方，转身无任何方位变化 | 角度结果恒为 0/无效；扫盘管线输出未到达 UI 或恒为无效值 |
| 扫盘结果 `OUT OF RANGE`，实际两卡 <10 m | 扫盘期跳频导致有效样本过少（头文件注释已承认 100 ms 驻留下即出现此问题，250 ms 只是缓解） |
| 无法进入诊断页；不知道日志在哪 | 缺少机上可观测性；用户侧无法采集根因证据 |
| `±` 显示为方框 | 字体缺字形，UI 小问题 |

这些现象与第 5 节的根因假设一一对应。

---

## 2. 硬件边界核对（对照硬件设计文档）

只列与定向/测距直接相关的事实，来源为硬件开发指南与产品规格。

| 项目 | 事实 | 对方案的约束 |
| --- | --- | --- |
| MCU | ESP32-C3，160 MHz RISC-V，400 KB SRAM，**无 PSRAM**，8 MB Flash | Wi-Fi + NimBLE + LVGL + 音频同时常驻时内部 RAM 紧张；CSI 缓冲必须极小 |
| 射频 | 单 2.4 GHz 射频、单 PCB 天线（卡片顶部边缘）、Wi-Fi b/g/n（HT20/HT40）、BLE 5.0；Wi-Fi/BLE 时分共存 | 无 AoA/AoD、无 Channel Sounding（Espressif 已确认 C3 硬件不支持）；BLE 活动直接挤占 ESP-NOW/FTM 时隙 |
| FTM | Initiator（STA）与 Responder（SoftAP）**均支持** | 需 APSTA；SoftAP 与 STA 必须同信道；APSTA 下 `esp_wifi_set_channel` 有限制 |
| CSI | `esp_wifi_set_csi` 支持；HT20 下 LLTF 64 子载波 / HT-LTF 56 子载波；C3 上 `first_word_invalid`（首 4 字节无效） | 单天线 CSI **相位**受 STO/CFO/SFO 污染，不可直接做 ToF/相位测角；**幅度**可用作宽带能量与频率分集 |
| 惯性传感 | **无 IMU、无磁力计** | 旋转角必须由时间推算，或由用户交互（寻的模式）替代 |
| 输入 | UP/DOWN/OK 三键（ADC）、独立电源键 | OK 长按已被启动器返回占用（LONG2）；诊断页入口需避开长按 |
| 输出 | 240×320 TFT；**ES8311 扬声器**（16 kHz PCM）；麦克风 | 扬声器可做**节拍器**与**寻的音调反馈**——这是本方案新利用的硬件能力 |
| 扩展 | 所有 GPIO（0–10、18–21）已占用；I2C0 被 ES8311/CW2017 共享；无外露接口 | **不可能外挂 UWB/天线开关/IMU**；硬件升级只能改板 |
| 电池 | 520 mAh | 持续 Wi-Fi 20 Hz + FTM + 屏幕约 150~200 mA，可用 2.5~3 h，可接受 |

结论：能用的物理观测量只有四种——**接收功率（RSSI，Wi-Fi 与 BLE）、往返时延（FTM）、子载波幅度（CSI 幅度）、以及人的动作（旋转/寻的）**。v3 设计围绕如何榨干这四种观测量、并用扬声器/屏幕约束"人"这一最大噪声源展开。

---

## 3. 物理极限与误差预算

### 3.1 测距

**RSSI 路径损耗模型的灵敏度。** `d = 10^((P₀ − r)/(10n))`，对 RSSI 误差 Δr 的相对距离误差为 `Δd/d = ln10 / (10n) · Δr`。n = 2.2 时每 1 dB 误差 ≈ 10.5% 距离误差。要在距离 d 处做到 ±1 m，允许的 RSSI 总误差为：

| d | 允许 Δr（n=2.2） |
| --- | --- |
| 3 m | 3.2 dB |
| 5 m | 1.9 dB |
| 10 m | 0.95 dB |
| 20 m | 0.48 dB |

而实测可达的 RSSI 残差（对称平均 + 卡尔曼后，仍含人体/握姿/天线方向图残余）室外约 ±2~3 dB，室内 ±4~6 dB；再叠加 n 的不确定性（室内 n 在 2~4 间变化，`Δd/d = ln(d)/n · Δn`，10 m 处 Δn=0.5 即 ~38%）。**RSSI 只能在室外 ≤3~4 m、室内 ≤2 m 范围内满足 ±1 m**，超出后只能提供比例误差（±15%~50%）。这与文献中"RSSI 测距典型误差 30%~70%"一致。

**FTM（802.11mc RTT）。** 20 MHz 采样周期 50 ns ≈ 15 m，基带做亚采样插值后单次 RTT 抖动为若干 ns 级；ESP32 上的实测文献（arXiv:2401.16517，"FTM for IoT: a practical approach using ESP32"）给出：室外视距、**做过零距离偏移（cm0）标定后**约 ±1 m；室内未做多径补偿时 3~5 m 偏差；RSSI 低于 −70 dBm 后失败率显著上升；HT40 并不比 HT20 更稳，Espressif 也不推荐为 FTM 切 HT40。要点：

- **偏移标定是前提**：每台设备的收发链路固定时延不同，未标定时 FTM 原始距离常系统性偏大数米。当前代码没有 FTM 偏移标定——这可能直接导致 FTM 距离被反射径规则（>1.8× RSSI 距离）判为"可疑"而永远不采纳。
- **多径偏差恒为正**：反射径只会让 RTT 变长。因此对多次 session 取**低分位（P25）而非中值**，是无需额外硬件的直射径估计器。
- **可用量程**：19.5 dBm 发射、1 m 参考 −42 dBm、n=2.2 时 −70 dBm 对应约 18 m。FTM 主导区间就是 ≤15~18 m。

**结论：距离 ±1 m 仅在室外视距 ≤15 m 且完成 FTM 偏移标定时可达（中位数）；室内因正向多径偏差退化到 1.5~3 m；隔墙不可达。**

### 3.2 定向

定向链的误差来源与量级（1σ，室外 / 室内）：

| 误差源 | 室外视距 | 室内 | 说明与可控手段 |
| --- | --- | --- | --- |
| 旋转速度不均（无 IMU，时间→角度） | ±15~30° 无引导；**±8~12° 有节拍器 + 自校正** | 同左 | 与射频无关，最大项；只能靠交互约束用户 |
| 峰位估计噪声（谐波拟合） | ±3~4° | ±5° | σ_θ ≈ (σ_rssi/A)·√(2/N)；σ=3 dB，A=8 dB，N=70 时 ≈ 3.6° |
| 多径扭曲方向图 | ±5° | **±15~40°** | 反射径可强于直射径；频率分集（CSI 子载波、BLE 三信道）能部分抑制 |
| 人体/天线方向图不对称（天线在卡顶部，持握偏心） | ±5~10° 系统偏差 | 同左 | 双端互测取镜像可部分抵消；也可按用户存一次偏置 |
| 峰显著性不足（遮挡深度 5~15 dB 因人而异） | 偶发大错 | 常见 | 显著性 <6 dB 时改报扇区 |

方和根合成：室外 ≈ 14°（1σ）→ p50 ≈ 9~10°、p90 ≈ 20~22°；室内 ≈ 28° → p50 ≈ 20°、p90 ≈ 40°+。文献中单天线旋转/人体遮挡测向的结果（室外 5~15° 中位、室内 15~30° 中位、p90 >45°）与此一致。

**结论：方向 ±15° 在室外视距可作为中位指标达成，p90 不可达；室内只能保证象限级。** 若引入"寻的模式"（用户在实时反馈下转到信号最强朝向，答案即用户当前朝向），旋转映射误差项消失，室外 p50 可压到 ≈8°，室内 ≈20°，但代价是交互时间从 4 s 变为 10~20 s。

### 3.3 各方案的天花板（对比）

| 方案 | 方向 | 距离 | 在本硬件上 |
| --- | --- | --- | --- |
| RSSI 峰（v1） | ±20~40° | ±50%~100% | 现状 |
| 谐波拟合 + 双向 RSSI + FTM（v2 设计） | ±10~15°（室外）| ±1~2 m（室外） | 已写，未调通 |
| **v3（本文）**：FTM 偏移标定 + 低分位 + 节拍器/寻的 + CSI 幅度分集 + 诚实降级 | 室外 p50 ≤15°；室内扇区 | 室外 p50 ≤1 m；室内档位 | 纯软件可达 |
| BLE 5.1 AoA / 6.0 CS、UWB、IMU | 3~5° / 厘米级 / 消除旋转误差 | 0.1~0.5 m | **需改板**，非本期 |

---

## 4. v3 设计方案

### 4.1 设计原则

1. **先诚实、再精确**：任何环境下都输出带区间和置信度的结果；不达标时降级为扇区/档位，而不是给一个看似精确的错数。
2. **观测量优先，单信道优先**：不再做扫盘期 Wi-Fi 跳频（它是当前 OUT OF RANGE 的直接原因，且与 APSTA/FTM 冲突）；频率分集改由 **CSI 子载波幅度**（同信道内 52 子载波、无需切频）提供。
3. **把"人"当作传感器的一部分来约束**：用扬声器节拍器约束转速，用寻的模式让用户自己完成最后 15° 的收敛。
4. **射频时隙有主次**：ESP-NOW 心跳 > FTM > CSI（被动，随 ESP-NOW 收包） > BLE。BLE 仅在 Wi-Fi 失联后启动，不与 FTM 常驻共存。
5. **每一步在机上可诊断**：日志环形缓冲页 + 观测量计数器，测试者无需接电脑。

### 4.2 总体架构

```
┌──────────────────────────────────────────────────────────────┐
│ UI：Tracking（距离区间+置信度）│ Sweep（节拍器）│ Homing（寻的）│ Diag │
├──────────────────────────────────────────────────────────────┤
│ 融合引擎 radar_fusion（纯逻辑，host test）                        │
│  · 距离：FTM(P25 of last 5, 偏移标定) 主 → 对称 RSSI 副 → 档位    │
│  · 方向：谐波拟合(时间) ⊕ CSI 能量拟合 ⊕ 对端镜像 → 环形均值/扇区    │
│  · 环境分级：LOS/室内/NLOS（FTM spread、RSSI p2p、FTM-RSSI 一致性）│
│  · 输出：点值 + 区间 + 置信度                                     │
├──────────────┬──────────────┬──────────────┬──────────────────┤
│ ESP-NOW      │ FTM          │ CSI(幅度)     │ BLE 哨兵(按需)    │
│ 20 Hz 心跳   │ 双向交替 1 s │ 随收包被动    │ Wi-Fi 失联>5 s 起 │
│ 扫盘 40 Hz   │ 扫盘期暂停   │ 扫盘期主用    │ 扫盘期关闭        │
└──────────────┴──────────────┴──────────────┴──────────────────┘
        射频调度器 radar_rf_sched：单信道(1)、HT20、APSTA 常驻
```

### 4.3 测距链

**D1 — FTM 偏移标定（新增，最高优先级）**
- 标定流程复用现有"两卡 1 m 长按 OK 3 s"：同时记录 `tx_ref_sym`（RSSI）与 `ftm_offset_m = median(FTM_raw) − 1.0`，写入 NVS `radar` 命名空间。
- 偏移是设备**对**相关的（两端链路时延之和），同一对卡标一次即可；换卡重标。
- 未标定时 FTM 只用于一致性校验，不用于显示；UI 提示"未标定"。

**D2 — 双向交替 FTM**
- 现状仅 MAC 小者发起。改为**按秒交替**：偶数秒 A 发起，奇数秒 B 发起；双方都拿到一手 FTM，不再依赖对端通过 ESP-NOW 回传 `dist_dm`。
- 每 session 8 帧（而非 16），`burst_period=0`，超时 600 ms；空口占用更短，减少与心跳冲突。
- 门控：仅在对端 RSSI ≥ −72 dBm 时发起；低于则不发起（省电，也避免无意义失败刷 backoff）。

**D3 — 低分位直射径估计**
- 保留最近 5 个成功 session 的中值，取其 **P25** 作为 FTM 距离；单 session 内仍用中值 + IQR。
- IQR > 2 m 或 session 间散布 > 3 m → 判定"多径严重"，环境等级下调。

**D4 — RSSI 副链（保留 v2 对称平均，修正两点）**
- 环境 n 切换加**滞回与最小驻留（10 s）**，禁止 2.2↔3.0 反复跳；一次追踪会话内 n 只允许单向缓慢变化（每 10 s 最多 ±0.2）。
- 显示值用**距离区间**：`[d/(1+e), d·(1+e)]`，e 由环境等级给出（室外 15%、室内 35%、NLOS 不显示点值）。

**D5 — 融合规则（替代 v2 的 0.7/0.3 固定权重）**

```
if FTM 有效 且 已标定 且 |FTM − RSSI_d| ≤ max(2 m, 0.5·RSSI_d):
    d = FTM(P25)                     ; 区间 ±max(0.5 m, IQR)        ; HIGH
elif FTM 有效 且 已标定 且 FTM > RSSI_d·1.8 + 2 m:   # 疑似反射径
    d = min(FTM, RSSI_d·1.3)         ; 区间 [RSSI_d/1.35, FTM]     ; MED
elif 双向 RSSI 有效:
    d = RSSI_d                       ; 区间按环境 e                 ; MED/LOW
else:
    档位输出（近/中/远）或 OOR
```

### 4.4 定向链

**A1 — 扫盘：单信道、40 Hz、6 s、节拍器引导**
- 扫盘期间双方 ESP-NOW 提速到 40 Hz（25 ms），时长 4 s → **6 s**（更慢的旋转更均匀，样本 ~240，谐波拟合 σ_θ 下降到 ~2°）。样本上限从 100 提到 256（8 B × 256 = 2 KB）。
- **不跳频**、暂停 FTM、关闭 BLE，射频只做收发心跳 + CSI。
- 扬声器输出**节拍器**：每 45° 一声"嗒"（6 s / 8 = 0.75 s 一拍），屏幕同步画一个匀速旋转的指针，要求用户"每一拍转到下一格"。这把旋转不均匀从 ±15~30° 压到 ±8~12°（第 3.2 节的最大误差项）。音频在独立任务中产生（`bsp_audio_write` 阻塞，禁止在 LVGL/按键回调中调用），PCM 为 16 kHz 单声道短促方波，缓冲 <4 KB。
- 保留 v2 的旋转周期自校正作为二级修正。

**A2 — CSI 幅度方向图（替代跳频分集）**
- 开启 `esp_wifi_set_csi`（仅 LLTF，`first_word_invalid` 跳过首 4 字节），回调中只对来自对端 MAC 的帧计算：宽带能量 `E = Σ|H_k|²`（dB）以及 4 个子载波组（每 13 子载波）的组能量。**不缓存 CSI 原始数据**，仅存 5 个 float，与同帧 RSSI 一起写入样本（`radar_sample_t` 扩展为 `rssi, csi_e, csi_g[4]`，每样本 28 B，256 样本 ≈ 7 KB）。
- 对 `rssi`、`csi_e`、4 个 `csi_g` 各自做谐波拟合得到最多 6 个角度，按拟合幅度加权做**环形中值投票**（复用 `radar_sweep_angle_vote`）。子载波组间相干带宽室内约 2~5 MHz，4 组彼此近似独立衰落——这就是无需跳频的频率分集。
- CSI 相位**不使用**（单天线 STO/CFO/SFO 无法校正）。

**A3 — 双端互扫镜像一致性**
- 保留 v2 的"对端跟随扫盘"。融合时：若 |θ_local − (θ_peer + 180°)| ≤ 30° → 两者环形均值，置信度 +1 级；>60° → 报扇区。
- 对端未持握（平放桌上）时其人体遮挡消失、拟合幅度 <4 dB，自动不参与投票。

**A4 — 寻的模式（Homing，新增）**
- 扫盘给出粗方向后，按 OK 进入寻的：屏幕显示"转向箭头 + 信号条"，扬声器音调随最近 1 s 的 `csi_e`/RSSI 滑动均值升高（Geiger 式冷/热反馈）。用户小幅左右摆动找最大值并按 OK 确认，**确认时的朝向即为答案**。
- 后台持续对最近 2 s 样本做谐波拟合，箭头指向"相对当前朝向还要转多少"；这消除了时间→角度映射误差，仅剩峰宽/多径项。
- 室内推荐默认走"扫盘 → 寻的"两步；室外可只扫盘。

**A5 — 显著性与扇区降级**
- 拟合幅度 <6 dB 或残差 >3 dB 或投票散布 >45° → 不画射线，画 ±45° 扇区并显示"方向不确定，建议寻的"。

### 4.5 环境分级（驱动一切降级）

| 等级 | 判据（任一满足即降级） | 距离输出 | 方向输出 |
| --- | --- | --- | --- |
| L0 室外视距 | FTM 成功率 >70%、IQR <1 m、双向 RSSI 差 <6 dB、RSSI p2p <6 dB | 点值 ±0.5~1 m | 射线 ±15° |
| L1 室内视距 | FTM IQR 1~2 m 或 RSSI p2p 6~10 dB | 点值 + 区间 ±35% | 射线 ±30° 或扇区 |
| L2 非视距 | FTM 失败/可疑或 RSSI p2p >10 dB 或长期均值 < −80 dBm | 三档（近/中/远） | 扇区 ±45° |
| L3 失联 | ESP-NOW 5 s 无包 | BLE 哨兵有/无 | 无 |

分级变化需要 10 s 最小驻留 + 滞回，避免 v2 的跳变。

### 4.6 射频时隙调度（单信道 1、HT20、APSTA 常驻）

```
Tracking（每 1 s 周期）
 0 ms ─┬─ ESP-NOW 心跳 20 Hz（双方，各 ~1 ms 空口）
       ├─ 100~700 ms：FTM session（本秒发起方；8 帧；对端 RSSI ≥ −72 时）
       └─ 全程：CSI 被动统计（随心跳收包）
 BLE：关闭（Wi-Fi 存活时）。Wi-Fi 失联 >5 s → 启动 NimBLE 哨兵；恢复后 10 s 内关闭。

Sweep（6 s）
       ├─ ESP-NOW 40 Hz（双方）+ CSI
       ├─ FTM 暂停、BLE 关闭、不跳频
       └─ 扬声器节拍器 8 拍

Homing（≤20 s）
       ├─ ESP-NOW 40 Hz + CSI，1 s 滑窗谐波拟合
       └─ FTM 每 2 s 一次（保持距离刷新）
```

不做扫盘期跳频的理由：(1) APSTA 下 SoftAP 固定在信道 1，STA 跳到 6/11 后 SoftAP 信标与 FTM responder 均失效；(2) 双端跳频依赖 ESP-NOW 对时，每次切换 50~100 ms 空洞，在 20 Hz 下每驻留仅剩 2~3 个样本；(3) 它是实测 "扫盘 OUT OF RANGE" 的直接原因。频率分集由 CSI 子载波组承担。

### 4.7 资源预算（无 PSRAM）

| 项目 | 估算 | 说明 |
| --- | --- | --- |
| 扫盘样本 256 × 28 B | ~7 KB | 静态数组 |
| CSI 回调工作区 | <1 KB | 仅算能量，不缓存原始 CSI |
| 节拍器/寻的音频任务 | 栈 4 KB + PCM 缓冲 2~4 KB | 复用 official-main 音频 demo 模式 |
| 日志环形缓冲（诊断页） | 4 KB | 32 条 × 128 B |
| NimBLE host+controller | ~70~100 KB | **仅在 L3 失联时按需启动**，与 FTM 不共存，释放内存 |
| 现有 LVGL 24 KB + LCD DMA 9.6 KB + Wi-Fi 驱动 | 不变 | — |

BLE 改为按需启动后，Tracking 态可用堆比 v2 更宽松；需在 build 后记录静态 RAM 与运行时 `largest free block`。

### 4.8 模块改动清单

| 文件 | 改动 |
| --- | --- |
| `main/radar_ftm.c` | 新增偏移标定与 NVS 读写；双向按秒交替发起；8 帧/600 ms 超时；RSSI ≥ −72 门控；最近 5 次 P25；移除 BLE "踢一脚"补丁（BLE 不再常驻） |
| `main/radar_fusion.c/.h` | 环境分级 L0~L3 + 滞回/驻留；区间输出；新融合规则；`radar_fusion_ftm_offset()`；host 测试补齐 |
| `main/radar_sweep.c/.h` | 去掉跳频调度（保留函数但 Tracking/Sweep 不调用）；样本结构增加 CSI 字段；多观测量投票；6 s/256 样本 |
| 新增 `main/radar_csi.c` | `esp_wifi_set_csi` 配置、对端 MAC 过滤、能量统计、Sweep/Homing 采样接口 |
| 新增 `main/radar_audio_cue.c` | 节拍器与寻的音调（独立任务，PCM 生成，`bsp_audio_*`） |
| 新增 `main/radar_rf_sched.c` | Tracking/Sweep/Homing 状态机与时隙分配；BLE 按需启停 |
| `main/radar_ble.c` | 改为惰性初始化/反初始化（已有 `radar_ble_deinit`） |
| `main/radar_espnow.c` | 扫盘 40 Hz；去跳频；ESP-NOW 心跳携带"本秒 FTM 发起方"位 |
| `main/radar_ui.c` | 距离区间/扇区渲染；节拍器指针；Homing 页；诊断页入口改为 **DOWN 双击**（避开 UP 长按 = 启动器返回冲突）；补 `±` 字形或改用 `+/-` |
| 新增 `main/radar_diag.c` | 日志环形缓冲 + 计数器（FTM 发起/成功/status 直方图、CSI 帧数、丢包率） |
| `sdkconfig.defaults` | `CONFIG_ESP_WIFI_CSI_ENABLED=y`；保留 `CONFIG_ESP_COEX_SW_COEXIST_ENABLE` |
| `tests/` | 融合规则、环境分级滞回、P25、多观测量投票、FTM 偏移的 host 单测 |

---

## 5. 先修复：v2.1 真机失效的根因假设与诊断步骤

以下假设按可能性排序，每条给出机上可验证的方法（依赖第 4.8 节的诊断页；在其落地前用 `idf.py monitor`）。

| # | 现象 | 根因假设 | 验证方法 | 对策 |
| --- | --- | --- | --- | --- |
| H1 | 从未出现 `FTM`/`HI` | FTM 原始距离含数米固定偏移，被 `radar_fusion_ftm_suspicious`（>1.8× 且 >+2 m）判为反射径，`ftm_used=false`，UI 不显示 `FTM` | 日志搜 `FTM ok:`：若有成功日志但 UI 无 `FTM`，即为 H1 | D1 偏移标定 |
| H2 | 同上 | FTM session 根本不成功：BLE Coded PHY 扫描窗口挤占（代码里的 coex "踢一脚"即为此症状的补丁），status=3 NO_RESPONSE | 日志搜 `FTM session status=`、`FTM initiate failed`；临时关闭 BLE 对比成功率 | BLE 按需启动（4.6） |
| H3 | 同上 / 扫盘后更差 | 扫盘跳频后 STA 未回到信道 1，或对端 SoftAP 在 APSTA 下随 STA 切频，FTM 目标信道错位 | 扫盘前后各打印 `esp_wifi_get_channel`；对比扫盘前/后 FTM 成功率 | 去跳频 |
| H4 | 扫盘 `OUT OF RANGE`（<10 m）、红点恒在正上方 | 250 ms 驻留 + 切换空洞 → 各信道样本 <6，谐波拟合返回 −1，回退峰检测又因样本稀疏峰值 < −88 dBm 门限 → OOR；UI 角度默认 0° | 扫盘结束打印 `count`、各信道样本数、拟合幅度/残差 | 去跳频、40 Hz、6 s |
| H5 | 3.5 m 处 1~5 m 跳变 | 纯 RSSI 路径 + 环境 n 在 2.2/3.0 间无滞回切换（3.5 m 在 n=2.2 与 3.0 下读数差 1.6×）+ 卡尔曼 r=4 对 ±5 dB 抖动仍不足 | 诊断页同时显示 `env`、`n`、`rssi_sym`、`d_rssi` 三秒内变化 | D4 滞回 + 区间显示 |
| H6 | 无法进入诊断页、`±` 方框 | UP 双击被识别为长按/被启动器钩子拦截；Montserrat 字体无 `±` | 按键页看事件序列 | 入口改 DOWN 双击；文案用 `+/-` |

**验收门（进入第 6 节路线图 M2 前必须满足）**：室内 3~5 m 静止两卡，60 s 内 FTM 成功率 ≥50%、日志有 `FTM ok`、UI 出现 `FTM`；扫盘 8 方向不出 OOR、角度随方向变化。

---

## 6. 路线图与验收指标

| 阶段 | 内容 | 出口指标（真机） |
| --- | --- | --- |
| **M0 可观测（3~5 天）** | 诊断页 + 日志环形缓冲 + 计数器；诊断入口修复；`±` 修复 | 测试者无需电脑即可读到 FTM status、样本数、env |
| **M1 调通（1~2 周）** | 去跳频；BLE 按需；FTM 偏移标定 + 双向交替 + P25；环境滞回；区间显示 | 室内 5 m：FTM 成功率 ≥50%，距离 p50 ≤2 m；扫盘不再 OOR，角度随方向变化 |
| **M2 定向增强（2 周）** | 40 Hz/6 s 扫盘；节拍器；CSI 能量方向图；多观测量投票；扇区降级 | 室外 8 方向 × 3 轮：方向 p50 ≤15°、p90 ≤25°；室内 p50 ≤25° |
| **M3 寻的与融合（1~2 周）** | Homing 模式；双端镜像一致性；环境分级驱动 UI | 室外 15 m 内：方向 p50 ≤12°，距离 p50 ≤1 m、p90 ≤1.5 m；室内自动扇区/档位，无"离谱数字" |
| **M4 打磨（可选）** | BLE 哨兵阈值实测；功耗；HT40 A/B（预期无收益，验证后关闭） | 10 min 连用无重启；哨兵 50~100 m 可见 |

**统计口径**：每个环境 × 距离/方向点采 ≥20 次独立读数（重启或走开重来算一次），报告 p50/p90 与"离谱率"（距离误差 >2× 或方向误差 >60° 的比例，目标室外 <5%、室内 <15%）。测试矩阵沿用实测指南 T1~T12，新增：
- T13 FTM 偏移标定前后对比（1/3/5/8 m）；
- T14 节拍器开/关对比（同一人同一方向 10 次）；
- T15 寻的模式终态误差；
- T16 CSI 能量 vs RSSI 单独拟合的方向误差 A/B（室内）。

---

## 7. 若必须在所有环境达到 ±15°/±1m：硬件路线（非本期）

当前板卡无任何可用扩展口，以下都需要改板，仅作为决策参考：

| 方案 | 收益 | 代价 |
| --- | --- | --- |
| 加 6 轴 IMU + 磁力计（I2C0 挂载，~1 美元） | 消除定向最大误差项（旋转不均），可给绝对方位；方向 p50 可到 ≤10°（室外） | 改板；I2C0 已由 ES8311/CW2017 共享，需核对地址与总线负载 |
| 换 BLE 6.0 Channel Sounding 芯片（nRF54L 系列）作协处理器 | 距离 0.1~0.5 m、量程 50~150 m、室内多径鲁棒 | 改板、双 MCU、成本与功耗上升 |
| UWB 模块（DW3000，SPI） | 距离 ~10 cm，支持 AoA 版本（双天线）可测角 3~5° | 模块 15~35 美元，>100 mA 峰值电流，改板 |
| BLE 5.1 AoA（多天线 + 射频开关） | 方向 3~5° | 卡片 60×95 mm 放 2.4 GHz 阵列（λ/2 ≈ 6 cm）极勉强；C3 本身不支持 CTE，需换主控 |

在不改板的前提下，第 4 节的 v3 就是本硬件的精度上限工程实现。

---

## 8. 参考资料

- ESP-IDF ESP32-C3 Wi-Fi 指南（FTM Initiator/Responder 支持）：https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-guides/wifi.html
- ESP-IDF FTM 示例（偏移 `-o` 标定、−70 dBm 限制、状态码）：https://github.com/espressif/esp-idf/blob/master/examples/wifi/ftm/README.md
- Fine Time Measurement for the IoT: A Practical Approach Using ESP32（LOS 约 ±1 m，室内 3~5 m，需 cm0 标定）：https://arxiv.org/html/2401.16517v1
- Benchmarking Wi-Fi FTM for IoT Devices：https://arxiv.org/pdf/2303.03766
- ESP-IDF Wi-Fi Vendor Features（CSI 子载波结构、C3 `first_word_invalid`）：https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-guides/wifi-driver/wifi-vendor-features.html
- ESP-CSI 项目文档：https://documentation.espressif.com/esp-csi/master/README.md
- ESPARGOS（单天线 CSI 相位 STO/CFO/SFO 不可用性的工程说明）：https://arxiv.org/html/2502.09405v1
- ESP-IDF RF 共存指南（ESP32-C3，Coded PHY 时隙限制选项）：https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-guides/coexist.html
- ESP-IDF BLE 特性支持表（C3 无 AoA/AoD/CS）：https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-guides/ble/ble-feature-support-status.html ；Espressif 确认：https://github.com/espressif/esp-idf/issues/15953
- ESP-IDF `esp_wifi_set_channel` 在 APSTA 下的限制：https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-reference/network/esp_wifi.html
- Fundamental limits in RSSI-based direction-of-arrival estimation：https://www.researchgate.net/publication/312569085_Fundamental_limits_in_RSSI-based_direction-of-arrival_estimation
- ARREST: RSSI-based mobile sensing and tracking（旋转天线测向 5~8° 中位）：https://anrg.usc.edu/www/wp-content/uploads/2019/03/ARREST-FINAL.pdf
- Landmark-assisted compensation of user's body shadowing on RSSI（人体遮挡 10~20 dB）：https://pmc.ncbi.nlm.nih.gov/articles/PMC8398681/
- ESP32-C3 单射频 ESP-NOW 跳频实践（切换空洞）：https://dev.to/gtanyware/channel-hopping-on-the-esp32-c3-with-esp-now-2led
