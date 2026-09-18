# pass-radar v2 后续计划:差距分析与分阶段实施

> 基线:`feat/ble-radar` 工作树(v2 增强,host 测试全绿)。
> 关联文档:[定位增强方案调研与设计](pass-radar-定位增强方案调研与设计.md) ·
> [v2 迁移与实测指南](pass-radar-v2-迁移与实测指南.md)
> English: [pass-radar-v2-followup-plan.md](pass-radar-v2-followup-plan.md)

## 1. 差距清单(v2 设计目标 vs 当前代码审计)

| # | 差距 | 设计出处 | 现状 |
| --- | --- | --- | --- |
| G1 | 双端同时扫盘 + 双端角度融合未接线 | 调研 5.4 P0 | 基础设施齐全(协议 v2 结果字段、`radar_fusion_angle_pair`、`circular_mean`、跟随跳频同步),但 `sweep_done` 只用本地角度;`radar_espnow_peer_angle()` 从未被消费;对端不会自动跟随启动扫盘 |
| G2 | 旋转速度自校正缺失 | 调研 5.4 P1 | 全仓无自相关代码;匀速 4s 假设保留 ±10°~30° 系统性偏差 |
| G3 | 置信度 UI 不完整 | 调研 5.6 | 仅文字/颜色/`~`前缀;LOW 无扇形区域、无 ±0.5m/±2m 误差标注;信道投票分歧未参与主页面 HIGH 判定 |
| G4 | BLE 哨兵无远/近分级 | 调研 5.5 | 只有 alive/死亡二态(`FAR (BLE link)`);`radar_ble_peer_rssi()` 已采集但未使用 |
| G5 | CSI 方向图 + 相位斜率 FTM 交叉校验缺失 | 调研 5.3 P3(可选) | 无 CSI 代码,`CONFIG_ESP_WIFI_CSI_ENABLED` 未开 |
| G6 | 小项 | — | `RADAR_ENV_OBSTRUCTED`(n=3.5)不可达;无 RSSI-only 回归/诊断页(调研 5.9);真机测试矩阵未跑;v2 设计文档未入库 |

## 2. 分阶段计划(每阶段:实现 → 验证 → 本地 commit)

### 阶段 0 — 计划与基线入库(本提交)

- 新增本文档(英文默认路径 + 中文配对)。
- 将当前 v2 工作树(代码 + 测试 + 文档 + 补丁)提交入库,后续阶段以绿色基线
  上的小增量形式呈现,便于回看。
- 验证:host 测试(fusion/sweep/protocol/kalman)+ `idf.py build`。

### 阶段 1 — 接线已有能力(无新算法)

1. **G1 双端扫盘**:对端报告 SWEEPING 且本端未扫盘时,自动启动本地扫盘
   (按对端 `sweep_id` 一次性触发)。`sweep_done` 中用
   `radar_fusion_angle_pair` 融合 `radar_espnow_peer_angle()` 与本地角度,
   并把融合分歧计入上报 spread。协议 v2 持续广播对端最新结果,扫盘结束时可用。
2. **G3 置信度 UI**:结果页 LOW 置信度画 ±spread 扇形弧;距离行追加
   `±0.5m`(HIGH)/ `±2m`(MED);主页面缓存最近投票分歧,spread > 20° 时
   HIGH 封顶为 MED。
3. **G4 BLE 远/近**:哨兵 RSSI 分级:`> -75dBm` → `FAR NEAR`,否则 `FAR`
   (阈值待真机调优)。
4. **G6 遮挡档**:`radar_env_est` 增加长期 RSSI 均值跟踪;持续均值低于阈值
   时在峰峰抖动判室内之外额外选中 `RADAR_ENV_OBSTRUCTED`。

验证:扩展 `tests/test_radar_fusion.c`(env-obstructed 用例);原有套件全绿;
`idf.py build` 通过。

### 阶段 2 — 新纯逻辑功能

1. **G2 旋转自校正**:新增 `radar_sweep_period_ratio()` —— 对扫盘滤波样本做
   短时自相关估计人体遮挡周期 T(≈ 旋转周期);角度映射改用
   `T / RADAR_SWEEP_DURATION_MS` 作为时间→角度比例。host test 覆盖
   3s/4s/5s 完成旋转的样本。
2. **G6 调试/回归页**:上键长按在追踪页切换诊断页,显示原始 RSSI、卡尔曼
   输出、v1 纯 RSSI 距离、融合距离、FTM 状态(bursts/spread)、BLE 哨兵
   RSSI、环境与 n 值 —— 即调研 5.9 的 RSSI-only A/B 基线,也是不接日志时的
   现场诊断视图。

验证:新 host 测试全绿;全量套件全绿;`idf.py build` 通过。

### 阶段 3 — CSI(可选、高风险,先真机验证前置阶段)

1. 打开 `CONFIG_ESP_WIFI_CSI_ENABLED`;CSI 回调在 Wi-Fi 任务,双缓冲队列
   (内存预算见调研 5.3 P3)。
2. 扫盘采样增加"子载波能量和"第二方向图,与 RSSI 谐波图一起参与信道投票。
3. 进阶:子载波相位斜率估计 ToF,对 FTM 反射径做 clip。

仅在阶段 0–2 固件通过下述真机矩阵后再启动。

### 阶段 4 — 真机验证闭环(持续进行)

按迁移指南测试矩阵在真机上执行并记录到 CHANGELOG/发布说明:室外 1–30m
距离梯度、室内多径降级、8 方向 × 3 次扫盘(≤±15°)、跟随跳频不中断、
共存 10min 压力、100m 超界三级接力(Wi-Fi OOR → BLE 哨兵 → 双超界)、
meta-pass LONG2 返回启动器。

## 3. 各阶段验证状态

```text
Phase 0: Build PASS · Host tests PASS · Device tests NOT RUN
```
