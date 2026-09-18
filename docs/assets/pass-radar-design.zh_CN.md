<p align="right">
  <a href="pass-radar-design.md">English</a> · <strong>简体中文</strong>
</p>

# pass-radar 设计文档

> Fork 私有设计文档，不提交上游。

## 1. 目标

将 FoloToy AI Passport（ESP32-C3，单 PCB 天线，240×320 屏幕，3 按键）变为
户外 360° 定向雷达玩具，纯软件实现，无需额外硬件。

## 2. 物理原理：人体屏蔽峰值检测

ESP32-C3 的 PCB 天线位于卡片顶部边缘。当卡片置于胸前平握时，人体会部分
遮挡来自身后的 2.4GHz 信号。将卡片旋转 360° 会产生可测量的 RSSI 变化：

- **RSSI 峰值** = 天线正对发射端（遮挡最小）
- **RSSI 谷值** = 人体挡在天线与发射端之间（遮挡最大）

峰值通常比谷值高 5–15dB。通过在 4 秒引导旋转中为每个 RSSI 采样记录时间戳
并找到峰值时间，方向计算公式：

```
角度 = (峰值时间戳 / 扫盘总时长) × 360°
```

这是**相对角度**（相对于旋转起始方向），不是绝对罗盘方位。

## 3. 双角色架构

两台设备运行**同一固件**。每台设备：

1. **广播** ESP-NOW 包（20Hz，50ms 间隔），包含 `radar_packet_t`（设备名 +
   序列号）。
2. **接收** 对方设备的广播包并测量 RSSI。

ESP-NOW 广播特性：设备不会收到自己的广播，因此每台接收端只看到对方的信号。
无需角色分配或配对握手——两台卡片刷同一固件即可自动互相检测。

```
设备 A                            设备 B
  │                                 │
  ├── 广播 @20Hz ─────────────────→│ 接收 A 的信号，测量 RSSI
  │                                 │
  │←──────────── 广播 @20Hz ────────┤ 接收 B 的信号，测量 RSSI
  │                                 │
```

## 4. 算法流水线

```
esp_timer（50ms，broadcast_tick）
  → esp_now_send(BROADCAST_MAC, radar_packet_t)

ESP-NOW 接收（20Hz，Wi-Fi 任务）
  → 卡尔曼滤波（平滑 ±5dB 抖动）
  → volatile s_filtered_rssi（跨线程）
  → lv_timer 轮询（100ms，LVGL 任务）
    → radar_sweep_calc_distance() → 距离显示
    → 如果扫盘中：累积采样
  → 4s 一次性 lv_timer 触发
    → radar_sweep_peak_angle() → 0°–360°
    → radar_sweep_peak_rssi() → 阈值检查
    → 结果页
```

### 卡尔曼滤波参数

| 参数 | 值 | 依据 |
| --- | --- | --- |
| q（过程噪声） | 0.15 | 户外开阔地慢速目标 |
| r（测量噪声） | 4.0 | 2.4GHz 典型 RSSI 抖动 ±5dB |
| 初始估计 | -70.0 | 户外中性起点 |

### 距离模型

自由空间路径损耗：`距离 = 10^((txPower - rssi) / (10 × n))`

| 参数 | 值 | 依据 |
| --- | --- | --- |
| txPower 参考 | -42dBm | ESP32-C3 最大功率下 1m 处参考 RSSI |
| n（衰减指数） | 2.2 | 户外开阔地（自由空间=2.0，略含地面反射） |

### 扫盘参数

| 参数 | 值 | 依据 |
| --- | --- | --- |
| 时长 | 4000ms | 足够舒适的完整旋转 |
| 最大采样数 | 100 | 20Hz × 4s = 80 预期；100 留余量 |
| 弱信号阈值 | -88dBm | 低于此值方向不可靠 |

## 5. 线程安全

| 写入方 | 读取方 | 机制 |
| --- | --- | --- |
| Wi-Fi 任务（ESP-NOW 回调） | LVGL 任务（lv_timer） | `volatile float s_filtered_rssi` + `volatile int s_raw_rssi` |
| Wi-Fi 任务（扫盘采样） | LVGL 任务（sweep_done） | `volatile bool s_sweeping` + `volatile unsigned s_sample_count` + `radar_sample_t s_samples[]` |
| esp_timer（广播） | Wi-Fi 驱动 | `esp_now_send()`（IDF 线程安全） |

ESP-NOW 接收回调运行在 Wi-Fi 任务中，不得触碰 LVGL。所有 LVGL 操作在
`lv_timer` 回调（LVGL 任务）或 `on_key`（button 任务，持有 `bsp_lvgl_lock()`）
中执行。

## 6. UI 布局（240×320）

```
 0        60        120       180       240
 |---------|---------|---------|---------|
 |         RADAR  （标题栏）             |  y=0-40
 |                                       |
 |    . - ~ ~ ~ - .                      |
 |  /  内圈          \     ● 目标         |  y=50-220
 | |  中圈            |                   |
 |  \  外圈          /     ⊕ 你           |
 |    ' - ~ ~ ~ - '                      |
 |                                       |
 |         DIST: 8.5 m                   |  y=232
 |         RSSI: -68 dBm                 |  y=258
 |    [OK] 启动 360° 扫盘                 |  y=290
 |---------|---------|---------|---------|
```

三个同心环（25、50、75px 半径）用边框样式 `lv_obj` 圆绘制（每环 1 个对象，
避免大量点对象耗尽 LVGL 内存）。中心点 = 用户。目标点按极坐标定位
（距离→半径，角度→方向，扫盘后显示）。

## 7. meta-pass 子固件适配

pass-radar 是 meta-pass 兼容子固件：

- **`metapass_mark_valid()`**（`app_main()`）：调用
  `esp_ota_mark_app_valid_cancel_rollback()` 实现跨重启常驻。从 factory
  直接调试运行时返回错误——忽略。
- **OK LONG2（3秒）** → `metapass_return_to_launcher()`：切换启动分区到
  factory 并重启。
- **MNAM 显示名 blob**：由 meta-pass 安装器在安装时写入 OTA 槽位最后
  4KB sector。pass-radar 不触碰该区域。
- **BSP LONG2**：`BSP_BTN_LONG2` 事件（3000ms），与 LONG（1500ms）作为
  第二个 `BUTTON_LONG_PRESS_START` 回调同时注册。

## 8. 分区布局

与基线一致：3MB factory，cardid@0x356000。作为 meta-pass 子固件安装时，
使用启动器的分区表（含 OTA 槽位）——子固件 app 二进制由安装器写入
OTA 槽位。

## 9. 验证状态

真机已验证（两台 AI Passport 卡片，同一固件）：

- ESP-NOW 双角色配对与 20Hz 广播收发
- 卡尔曼滤波 RSSI 实时距离追踪
- 360° 扫盘定向（人体屏蔽峰值检测）
- 弱信号保护行为

剩余实地注意事项（不阻塞，可能因环境而异）：

- 人体屏蔽峰值幅度因用户和握持姿势不同而异
- 30 米距离取决于环境（开阔地 vs 障碍物）
- 拥挤 2.4GHz 环境下的 ESP-NOW 广播可靠性
- meta-pass 槽位安装端到端流程

## 10. v2：测距与定向增强

v2 针对 v1 的两大误差源：单链路 RSSI 信息量不足（测距）与单信道人体遮挡
峰的脆弱性（定向）。

### 10.1 观测源

| 观测源 | 物理层 | 用途 |
| --- | --- | --- |
| 双向 RSSI | ESP-NOW (Wi-Fi) | 对称平均消除天线/发射功率不对称 |
| FTM RTT | Wi-Fi 802.11mc | 时间戳测距，米级，免标定 |
| 信道分集 | Wi-Fi 1/6/11 信道 | 扫盘期间独立衰落采样 |
| BLE RSSI | BLE 5.0 Coded PHY | 长距哨兵 + 异构交叉校验 |

ESP32-C3 硬件边界决定设计：不支持 BLE 5.1 AoA/AoD（无 CTE 硬件）、不支持
BLE 6.0 Channel Sounding（需 PBR PHY）、单天线。FTM 需要 AP/STA 角色拆分
（responder 仅 AP 模式），故 Wi-Fi 工作在 APSTA，双端均开启隐藏 SoftAP
（`passradar`，启用 FTM responder），各自每 2s 向对方发起一次 FTM session
（16 burst）。

### 10.2 融合管线（纯逻辑，`radar_fusion` 有 host 测试）

```
本端 RSSI ─┐
           ├─ 对称平均 ─→ 路径损耗距离(n 自适应) ─┐
对端 RSSI ──┘                                      ├─ 融合 ─→ 距离 + 置信度
FTM burst ─→ 中值 + IQR ─→ 反射径判别(vs RSSI×1.8) ─┘
```

- FTM 有效且一致 → 0.7·FTM + 0.3·RSSI；双向 RSSI 差 ≤ 8dB 时置信度 HIGH。
- FTM 存疑（反射径）→ 纯 RSSI，MED。
- 无对端数据 → 单链路 RSSI，LOW；对称 RSSI < -88dBm → OOR。

### 10.3 定向（`radar_sweep`）

- 4 秒扫盘期间射频在 1/6/11 快跳（100ms 驻留）；双端经 v2 广播包中的
  扫盘标志共享同一调度。
- 每信道样本做一次谐波最小二乘拟合 `y = a + b·cosθ + c·sinθ`——对快跳
  造成的稀疏/成簇采样鲁棒（原始峰检测不行）。
- 各信道角度按环形中值投票；最大环形分歧作为 ± 不确定度显示，超过
  20°/45° 时逐级降低置信度。
- 回退路径：全局抛物线插值峰检测 + 显著性 ≥ 6dB 门限。

### 10.4 BLE 哨兵

NimBLE 扩展广播（Coded PHY，1s 间隔）+ 稀疏被动扫描（约 10% 占空比）。
Wi-Fi 超界后 UI 显示 "FAR (BLE link)"。BLE 占空比刻意压低——Wi-Fi 与 BLE
共享单 2.4GHz 射频（软件共存）。

### 10.5 标定

追踪页 OK 长按启动 3s 标定：两卡相距 1m 静止，对称 RSSI 均值钳位到
-60~-30dBm 后作为 `tx_ref_sym` 存入 NVS（命名空间 `radar`）。
