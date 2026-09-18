# pass-radar 定向/测距增强方案调研与整合设计

> 目标读者：pass-radar 项目维护者
> 日期：2026-09-16
> 输入：alexwwang/pass-radar 当前实现（ESP-NOW RSSI + 人体遮挡峰检测 + 卡尔曼滤波）

---

## 1. 项目现状与误差来源分析

当前实现（`main/radar_espnow.c`、`radar_sweep.c`、`radar_kalman.c`）：

- **链路**：ESP-NOW 跑在 Wi-Fi PHY（802.11 b/g/n, 2.4GHz）上，20Hz 广播，单 PCB 天线，19.5dBm。
- **测距**：RSSI → 1D 卡尔曼滤波（q=0.15, r=4.0）→ 自由空间路径损耗模型 `d = 10^((txPower-rssi)/(10n))`，txPower=-42dBm@1m，n=2.2。
- **定向**：引导用户 4 秒匀速旋转，取滤波后 RSSI 峰值时刻，映射为 0°–360° 相对角；<-88dBm 判为超界。
- **固件**：双角色同镜像，meta-pass 兼容，ESP-IDF 5.5.3。

### 1.1 测距偏差的主要来源

| 误差源 | 机理 | 量级 |
| --- | --- | --- |
| RSSI 抖动 | 2.4GHz 多径衰落使 RSSI 瞬时抖动 ±5dB；对数模型下 6dB 误差 = 距离 ×2（n=2.2 时） | 距离 50%~100% 偏差 |
| 路径损耗指数 n 失配 | n=2.2 仅适配室外开阔地；室内 n=3~4、隔墙 n>4，直接套用会系统性低估/高估 | 室内可达数倍 |
| 天线方向图 | 单天线 PCB 方向图不均匀，持握姿态改变等效增益 ±3~5dB | 距离 40%~70% |
| 发射功率/参考点不准 | txPower=-42dBm 是单一标定值，个体差异未补偿 | 系统性偏移 |
| 人体遮挡 | 胸口持握时身体引入额外衰减，且随姿态变化 | 数 dB 级 |

### 1.2 定向偏差的主要来源

| 误差源 | 机理 | 量级 |
| --- | --- | --- |
| 峰检测受多径干扰 | 室内反射路径的 RSSI 峰可能盖过直射路径峰 | 数十度 |
| 旋转速度不均 | 假设匀速旋转映射时间→角度，加速/减速段直接变成角度误差 | ±10°~30° |
| 人体遮挡峰幅度个体差异 | 设计文档已记录：峰幅度因人而异、因握姿而异 | 峰弱时可能检错峰 |
| 单一样本点定峰 | 取滤波序列最大值，未做子样本插值 | 20Hz × 4s 下分辨率约 4.5°/样本 |
| 单信道衰落 | 单 Wi-Fi 信道上恰好处于深衰落时，峰被抹平 | 偶发大偏差 |

结论：**问题不在滤波不够，而在于 RSSI 这一观测量本身信息量不足**。任何只在 RSSI 上叠滤波器的改进空间都很有限，需要引入新的观测量（RTT 时间戳、多信道、多子载波相位）与多链路分集。

---

## 2. 硬件能力盘点（ESP32-C3 的边界）

FoloToy AI Passport 用的是 ESP32-C3（RISC-V 160MHz，400KB SRAM，8MB Flash，单 2.4GHz 射频，Wi-Fi 与 BLE 共享天线、支持共存）。以下能力边界决定了"哪些方案可行"：

### 2.1 Wi-Fi 侧

| 能力 | 是否可用 | 说明 |
| --- | --- | --- |
| 802.11mc FTM（RTT 测距） | ✅ 可用 | C3 支持 FTM Initiator（STA 模式）与 FTM Responder（AP 模式）；官方建议两台 ESP 设备之间做 FTM 效果最好（[ESP-IDF Wi-Fi 文档](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-guides/wifi.html)） |
| Wi-Fi CSI（信道状态信息） | ✅ 可用 | 全系列支持含 C3；HT20 下每包返回约 52~64 个子载波的幅度+相位；监听模式可对广播包取 CSI（[ESP-IDF CSI 文档](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/wifi-driver/wifi-vendor-features.html)、[ESP-CSI 项目](https://github.com/espressif/esp-csi)） |
| 信道切换 | ✅ 可用（有限制） | C3 单射频单信道，`esp_wifi_set_channel` 可切换，STA 未连接外部 AP 时可用；适合扫盘期间在 1/6/11 间跳变 |
| 多天线/天线分集 AoA | ❌ 不可用 | 单 PCB 天线；数据手册的 "Antenna diversity" 指外置天线切换，非测角 |

### 2.2 BLE 侧

| 能力 | 是否可用 | 说明 |
| --- | --- | --- |
| BLE 5.0 Coded PHY（S=2/S=8） | ✅ 可用（IDF ≥5.0） | 灵敏度增益约 +3dB / +9dB，实测开阔场通信距离从约 80m 提升到 160m / 240m（[ESP-FAQ](https://docs.espressif.com/projects/esp-faq/en/latest/software-framework/bt/ble.html)、[IDF BLE 特性表](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-guides/ble/ble-feature-support-status.html)） |
| BLE 5.0 2M PHY / 扩展广播 / 周期广播 | ✅ 可用 | 40 信道 + 3 个主广播信道分集；周期广播可做低占空比信标 |
| BLE 5.1 AoA/AoD（CTE 测角） | ❌ 硬件不支持 | Espressif 官方确认 "ESP32-C3 hardware itself does not support AoA/AoD features"（[esp-idf#15953](https://github.com/espressif/esp-idf/issues/15953)） |
| BLE 6.0 Channel Sounding（PBR+RTT 厘米级测距） | ❌ 硬件不支持 | CS 需要支持 Phase-Based Ranging 的新 PHY，仅 nRF54 等新芯片具备，无法软件升级（[Channel Sounding 综述](https://electronics.alibaba.com/product/cs-bluetooth)） |
| BLE CSI | ❌ 不存在 | BLE 协议栈无 CSI 概念 |

**关键判断**：在 ESP32-C3 现有硬件上，BLE 测距/测角的天花板就是 "BLE RSSI + Coded PHY"，物理机理与 Wi-Fi RSSI 相同（同为 2.4GHz 窄带接收功率），**单独换 BLE 不会带来精度质变**；BLE 的价值在于异构链路分集、远距信标和功耗。真正的精度跃升来自 Wi-Fi 侧的 FTM（时间戳观测量）与 CSI（多子载波观测量）。

---

## 3. BLE 雷达方案调研

### 3.1 方案 A：BLE RSSI 测距/定向（C3 可实现）

- 原理：广播/扫描收包 RSSI → 路径损耗模型，与现有 Wi-Fi 方案同构。
- 可增强点：40 信道频率分集（对 3 个主广播信道分别建模取中值，可抑制单频深衰落）；Coded PHY 将可用范围扩大 2~3 倍；周期广播（PAwR）做低功耗哨兵链路。
- 精度：业界典型 1~3m（[BLE 室内定位实践](https://blog.csdn.net/weixin_36369848/article/details/154364929)），与 Wi-Fi RSSI 相当，强依赖标定。
- 定向：C3 无 CTE，只能复用"人体遮挡峰"思路，无本质提升。

### 3.2 方案 B：BLE 5.1 AoA/AoD（C3 不可实现）

- 原理：发射恒定音调扩展（CTE），接收端天线阵列采样相位差测角，精度典型 3°~5°（配 4+ 天线阵列）。
- 现状：需要芯片硬件支持 CTE 采样 + 多天线射频开关。**C3 硬件不具备，软件无法补齐**。若未来换硬件（如 nRF5340 协处理器或 ESP32-C5/C6 + 外置天线开关），AoA 基站方案可做到 0.1~0.5m 级定位。

### 3.3 方案 C：BLE 6.0 Channel Sounding（C3 不可实现，未来路线）

- 原理：PBR（相位测距）+ RTT 双机制，认证级安全测距，精度 10~30cm（50m 内最佳），量程可达 150m（[FAQ](https://needcode.io/top-10-bluetooth-channel-sounding-bluetooth-6-0-development-companies/)）。
- 现状：需 nRF54、Silabs xG24 等新硅料；**无法 OTA 到 C3**。这是"下一代硬件"选项。

### 3.4 方案 D：BLE "雷达"式感知（存在/接近检测）

- BLE 无 CSI，不能像 Wi-Fi 那样做子载波级感知；但可利用 Coded PHY 的远距特性做"哨兵链路"：当 Wi-Fi/ESP-NOW 链路 RSSI < -88dBm 判超界时，切到 BLE Coded PHY 长距广播继续粗跟踪（有/无、强弱两档），扩大玩具的有效工作半径。

### 3.5 Wi-Fi 侧被低估的两个观测量

**FTM（802.11mc RTT）**：硬件时间戳往返测距，不依赖接收功率，天然免疫发射功率、天线增益标定误差。实测反馈：室外 LOS 3~12m 稳定且与真实距离成比例；室内多径下常命中反射路径（距离偏大）；RSSI 低于 -70dBm 时失败率上升（[ESP32 FTM 实测](https://forum.arduino.cc/t/esp32-wifi-ftm-evaluation-de-distances-par-temps-aller-retour/1201244)）。典型精度 1~2m。代价：需要一端为 SoftAP 角色，破坏"同镜像双角色"的简洁性。

**CSI**：每包 52+ 子载波幅度/相位。两个直接用途：
1. **定向增强**：扫盘时 CSI 各子载波能量之和（或主径相位变化率）形成比单点 RSSI 锐利得多的方向图，对多径峰、深衰落更鲁棒；
2. **测距增强（进阶）**：利用子载波间相位斜率估计直达径时延（合成带宽思想），辅助排除反射径，或对 RSSI/FTM 结果做一致性校验。

---

## 4. Wi-Fi 方案 vs BLE 方案对比

| 维度 | Wi-Fi RSSI（现状） | Wi-Fi FTM (RTT) | Wi-Fi CSI | BLE RSSI (Coded PHY) | BLE AoA/CS（新硬件） |
| --- | --- | --- | --- | --- | --- |
| 测距精度 | 差（误差可达 50%~100%） | 1~2m（LOS） | 辅助级（相位斜率可进阶） | 1~3m（同 RSSI 机理） | 0.1~0.5m / 10~30cm |
| 定向能力 | 弱（人体遮挡峰，±10°~40°） | 无 | 中（扫盘方向图锐化） | 弱（同左） | 强（3°~5° 真测角） |
| 多径鲁棒性 | 差 | 中（会锁反射径） | 好（频率分集） | 差（可用 3 广播信道分集略好） | 好 |
| 量程 | ~30m（19.5dBm） | 同 Wi-Fi | 同 Wi-Fi | Coded PHY 下 150m+ 哨兵 | 50~150m |
| 标定依赖 | 高（txPower、n） | 低（仅需时延标定） | 中（相位偏移标定） | 高 | 低 |
| 功耗 | 中 | 中 | 中高（持续监听） | 低（BLE 天然省电，Coded PHY 略高） | 中 |
| C3 硬件成本 | 零（已实现） | 零（纯软件） | 零（纯软件） | 零（官方固件已禁用 BLE，需重新启用） | 需换硬件 |
| 改造成本 | — | 中（角色协商、AP/STA 拆分） | 中高（算法复杂） | 低 | 高 |

**适合场景归纳**：

- **Wi-Fi RSSI**：室外开阔、快速粗测、无标定条件的兜底链路；维持现状并做增强（见第 5 节）。
- **Wi-Fi FTM**：两端都在视距内、需要稳定米级距离时的主测距源；室内多径环境需加"反射径排除"策略（与 CSI 或 RSSI 一致性校验）。
- **BLE Coded PHY**：作为**第二独立链路**做存在性/粗距离哨兵，覆盖 Wi-Fi 超界（>30m）场景；其衰落与 Wi-Fi 相关性低，可做融合时的独立观测。
- **BLE AoA / Channel Sounding**：属于"下一代硬件（换 nRF54/C5 协处理器）"路线，当前仅作预留接口设计。

---

## 5. 整合方案设计

### 5.1 设计原则

1. **观测量优先于滤波器**：新增时间戳（FTM）与子载波（CSI）观测，不再堆叠 RSSI 滤波。
2. **双链路异构分集**：Wi-Fi（ESP-NOW）+ BLE（Coded PHY）并行，利用两协议衰落不相关的特性做交叉校验与置信度评估。
3. **保持玩具体验**：单固件、自动角色协商、扫盘流程不变、结果页增加置信度指示。
4. **分阶段可落地**：每阶段独立可用、可验证，不依赖换硬件。

### 5.2 总体架构

```
┌─────────────────────────────────────────────────────┐
│  应用层：Tracking / Sweep / Result UI（含置信度指示）      │
├─────────────────────────────────────────────────────┤
│  融合引擎 (fusion)                                     │
│   · 距离融合：FTM 主 + 双向RSSI几何平均 + 信道分集          │
│   · 置信度模型：观测量一致性 → LOW/MED/HIGH               │
│   · 场景自适应：室内/室外 n 值、FTM 可用性开关              │
├──────────────┬──────────────────┬───────────────────┤
│ Wi-Fi 观测层   │ BLE 观测层        │ 传感器辅助         │
│ · ESP-NOW     │ · 广播(3信道)RSSI │ · 无 IMU：用 RSSI   │
│   双向 RSSI   │ · Coded PHY 哨兵  │   周期自相关估计    │
│ · FTM RTT     │   (>30m 超界)     │   旋转速度          │
│ · CSI(扫盘)   │                  │                   │
└──────────────┴──────────────────┴───────────────────┘
```

### 5.3 测距融合（按优先级）

**P0 — 双向 RSSI 几何平均（零硬件成本，立即做）**
双端都在广播，天然获得前向/反向 RSSI。对置消除天线增益与发射功率差异：

```
rssi_sym = (rssi_AB + rssi_BA) / 2
d = 10^((txPower_sym - rssi_sym) / (10·n_env))
```

效果：消除单端天线方向图与持握姿态引入的 ±3~5dB 系统误差，这是当前最大可收割的精度红利。

**P0 — 场景自适应 n 值与在线标定**
- 出厂标定 txPower 改为"双端对置 1m 自动标定"流程（两卡贴近 1m 按 OK 自动学习 txPower_sym，写入 NVS）。
- 依据 FTM 可用性与 RSSI 波动特征粗分室内/室外，自动切换 n（室外 2.2 / 室内 3.0 / 有遮挡提示 3.5）。

**P1 — FTM 作为主测距源**
- 角色协商：开机后 MAC 小者自动成为 Anchor（SoftAP + FTM Responder + ESP-NOW 照常广播），另一台为 Rover（STA + FTM Initiator）。同固件，自动完成，无需用户介入。
- 节奏：Tracking 态每 2s 做一次 FTM session（16 bursts）；同一信道运行，避免 "single burst ASAP" 降级。
- 融合规则：
  - FTM 成功且 RSSI > -70dBm → 距离 = 0.7·FTM + 0.3·RSSI 平滑（收敛快、抗跳变）；
  - FTM 失败或室内疑似反射径（FTM 值 > RSSI 推算值 ×1.8）→ 降级为 RSSI 融合并报 MED 置信度；
  - RSSI < -88dBm 且无 BLE 哨兵 → OUT OF RANGE。
- 已知局限与对策：FTM 在多径下锁反射径（偏大）→ 用 P2 的 CSI 相位斜率或 RSSI 上限做合理性截断（clip）。

**P2 — 信道跳变分集（扫盘期间）**
扫盘 4s 内按 1 → 6 → 11 三段切信道（C3 单射频，每段约 1.3s，段内保持连续采样）：
- 每信道独立做峰检测，三信道峰角一致性投票（取中值），可显著抑制单频多径造成的假峰；
- Tracking 态保持单信道（跳变会打断 ESP-NOW 20Hz 流，仅扫盘态启用）。
- 注意：`esp_wifi_set_channel` 在 STA 未关联外部 AP 时可直接调用；与 FTM 错时进行（扫盘时不做 FTM）。

**P3 — CSI 辅助（进阶，可选）**
- 扫盘采样从"RSSI 单值"升级为"CSI 能量图"：对每包取 52 子载波幅度平方和（或前 10 强子载波能量），得到远锐于 RSSI 的方向图；
- 进阶：子载波相位斜率估计直达径 ToF，为 FTM 提供反射径判别依据；
- 成本：CSI 回调在 Wi-Fi 任务，需双缓冲队列；内存约 2×(64B×80) 可接受。

### 5.4 定向增强

**P0 — 峰值子样本插值 + 峰显著性门限**
- 峰值附近三点抛物线插值，角度分辨率从 ~4.5°/样本提升到约 1°；
- 峰显著性 = 峰值 − (P10 分位基线)，< 6dB 判为低置信度，UI 显示扇形而非射线。

**P0 — 双端同时扫**
两台设备扫盘时刻已通过 ESP-NOW 对时（误差 <50ms），同一轮旋转两端各自出角估计，取圆形均值（circular mean）。双观测独立误差减半，且能发现"一端人体遮挡弱"的失效情况。

**P1 — 旋转速度自校正（无 IMU 替代方案）**
对滤波 RSSI 做短时自相关，人体遮挡周期 ≈ 旋转周期；当用户在 4s 内提前/滞后完成旋转时，用自相关峰修正 时间→角度 映射，缓解"转快/转慢"造成的系统性角度偏置。

**P2 — 信道分集投票**（同 5.3 P2）

**P2 — CSI 方向图**（同 5.3 P3）：CSI 能量对遮挡更敏感、对多径更不敏感，峰宽比 RSSI 窄 30%~50%（预期值，需实测验证）。

### 5.5 BLE 的角色（辅助而非主力）

1. **超界哨兵**：启用 BLE Coded PHY S=8 长距广播（低占空比，如 2Hz），对端扫描。Wi-Fi 判超界后若 BLE 仍可达 → 显示 "远/近" 两档 + 方向不可用提示，把玩具有效半径从 ~30m 扩到 100m+。
2. **异构校验源**：BLE RSSI 与 Wi-Fi RSSI 衰落相关性低，融合引擎用二者差值动态评估当前多径严重程度，调整置信度与 n 值。
3. **共存注意**：Wi-Fi 与 BLE 共享射频，启用 BLE 扫描/广播会与 ESP-NOW 抢占天线时间；采用时间片调度（BLE 广播放在 ESP-NOW 发送间隙，IDF coexistence 已支持），Tracking 态 BLE 占空比控制在 10% 以下。

### 5.6 置信度模型（UI 呈现）

| 置信度 | 条件（示例规则） | UI |
| --- | --- | --- |
| HIGH | FTM 成功 且 双向RSSI差 < 8dB 且 信道投票一致 | 实线射线 + 距离±0.5m |
| MED | 仅 RSSI 融合 或 FTM/RSSI 分歧 < 1.8× | 虚线射线 + 距离±2m |
| LOW | 单链路 或 峰显著性 < 6dB 或 室内 | 扇形区域 + "~" 前缀 |
| OOR | Wi-Fi 与 BLE 均超界 | OUT OF RANGE |

### 5.7 固件改动清单（对应仓库结构）

| 模块 | 改动 |
| --- | --- |
| `main/radar_espnow.c` | 双端 RSSI 交换（包里带本端最近 RSSI）；扫盘期信道跳变；FTM session 管理 |
| `main/radar_sweep.c` | 抛物线插值、峰显著性、三信道投票、双端角度融合（纯逻辑，先扩 host tests） |
| 新增 `main/radar_ftm.c` | Anchor/Rover 角色协商、FTM 发起/报告解析、反射径截断 |
| 新增 `main/radar_fusion.c` | 距离加权融合、置信度规则、场景自适应 n |
| 新增 `main/radar_ble.c` | NimBLE 初始化、Coded PHY 哨兵广播/扫描、BLE RSSI 上报 |
| `main/radar_ui.c` | 置信度可视化（线型/扇形/±误差） |
| `sdkconfig` | 打开 BLE（NimBLE）+ coexistence；开启 CSI（P3） |
| `tests/` | 融合引擎、投票、插值、circular mean 的 host 单测 |

### 5.8 分阶段路线图

| 阶段 | 内容 | 预期收益 | 风险 |
| --- | --- | --- | --- |
| M1（1~2 周） | 双向 RSSI 几何平均 + 1m 自动标定 + 抛物线插值 + 峰显著性 | 测距系统误差减半；定向分辨率 4.5°→1° | 低，纯软件 |
| M2（2~3 周） | FTM 角色协商 + 距离融合 + 置信度 UI + 场景 n 自适应 | 测距进入 1~2m 级；用户可感知可信度 | 中：AP/STA 拆分影响 ESP-NOW，需共存验证 |
| M3（1~2 周） | 扫盘信道跳变投票 + 双端同时扫 + 旋转自校正 | 室内定向从 ±30° 提升到 ±10° 量级 | 中：信道切换与 FTM/CSI 时序 |
| M4（2~4 周，可选） | CSI 方向图 + 相位斜率校验；BLE Coded PHY 哨兵 | 定向峰更锐；工作半径 ×3；多径识别 | 高：CSI 相位标定、共存调度 |
| 未来 | 换 nRF54/C5 协处理器 → BLE AoA / Channel Sounding | 厘米~分米级 | 需硬件改版 |

### 5.9 验证方案

- **host tests**：融合引擎、投票、插值、circular mean（沿用现有 `tests/` 风格，纯 C 无 IDF 依赖）。
- **对置标定场**：室内走廊 + 室外空地，距离梯度 1/3/5/8/12/20/30m，每点 60s 采样，统计各观测源误差分布与相关系数。
- **定向场**：8 方向 × 3 种握姿 × 室内/室外，比较单 RSSI 峰 vs 三信道投票 vs CSI 方向图。
- **回归**：保留 RSSI-only 模式（长按切换 Debug 页），A/B 对比新管线。

---

## 6. 一句话结论

在 ESP32-C3 现有硬件上，**BLE 无法单独解决定向/测距偏差**（无 AoA/CS 硬件），正确的整合路线是：**以 Wi-Fi 双向 RSSI 融合 + FTM RTT 为主测距体系、以信道分集 + 双端同扫 + CSI 方向图为定向增强、以 BLE Coded PHY 为超界哨兵与异构校验源**，并全程输出置信度。若未来允许硬件升级，BLE Channel Sounding 是分米级终局方案。

## 7. 参考资料

- ESP32-C3 Datasheet（FTM / Antenna diversity / BLE 5.0）：https://www.espressif.com/sites/default/files/documentation/esp32-c3_datasheet_en.pdf
- ESP-IDF ESP32-C3 Wi-Fi FTM 说明：https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-guides/wifi.html
- ESP-IDF ESP32-C3 BLE 特性支持表：https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-guides/ble/ble-feature-support-status.html
- Espressif 官方确认 C3 硬件不支持 AoA/AoD：https://github.com/espressif/esp-idf/issues/15953
- ESP-IDF CSI 配置指南：https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/wifi-driver/wifi-vendor-features.html
- ESP-CSI 项目（全系列支持含 C3）：https://github.com/espressif/esp-csi
- ESP32-C3 Coded PHY 实测（80→240m）：https://wenku.csdn.net/column/59phig1ca5d
- ESP32 FTM 实测反馈（室外 LOS 稳定、室内多径锁反射径）：https://forum.arduino.cc/t/esp32-wifi-ftm-evaluation-de-distances-par-temps-aller-retour/1201244
- Bluetooth Channel Sounding（BT 6.0, 10~30cm）：https://electronics.alibaba.com/product/cs-bluetooth ，https://needcode.io/top-10-bluetooth-channel-sounding-bluetooth-6-0-development-companies/
- ESP32-C3 单射频 ESP-NOW 信道跳变实践：https://dev.to/gtanyware/channel-hopping-on-the-esp32-c3-with-esp-now-2led
