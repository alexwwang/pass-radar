# pass-radar v3 户外数据采集任务与存储容量方案

English version: `pass-radar-v3-outdoor-capture-tasks-and-storage-capacity-plan.md`

日期：2026-09-21
适用固件：`build/pass-radar_v0.1-20-g77a6efd-capture-export-fix2.bin`
状态：Capture 与重复导出真机验证通过；本文件定义正式户外数据采集任务和存储边界。

## 1. 结论

当前 Capture 可以稳定完成单个“距离标签 + 场景”的自动采集，并在采集结束后通过 USB 输出完整 CSV。当前 NVS 设计不能保存一整套户外矩阵的全部 Capture 数据。

正式方案是：

```text
USB 监控日志：完整户外数据集的主存储
NVS latest_a/latest_b：最近两次有效 Capture 的恢复缓存
```

不把采集期间的每条样本写入 Flash，也不把现有 NVS 误认为可无限累积的测试数据库。

## 2. 当前一次 Capture 能采集什么

当前 Debug Capture 状态机为：

```text
COUNTDOWN 3 s
  -> SETTLE 5 s
  -> CAPTURE 30 s
  -> SUMMARY / SAVE
  -> USB CSV
```

一次 Capture 对应一个测试单元：

```text
一个距离标签 + 一个场景 + 一个固定卡片姿态/方向 + 一个约 38 秒窗口
```

当前预设距离为：

```text
1 m / 3 m / 5 m / 8 m / 10 m / 15 m / 20 m / 30 m
```

一次 Capture 通常产生约 99～100 条记录。采样周期约 250 ms，正式采集窗口为 30 s。一次 Capture 不应同时包含多个距离或多个环境场景；否则无法把原始记录归属于真值条件。

## 3. 户外采集任务矩阵

### 3.1 当前固件可直接执行的基础矩阵

第一阶段只采集无遮挡、静止、视距（LOS）基线：

| 场景 ID | 场景 | 距离点 | 每点 Capture 数 | 预计 Capture 数 |
| --- | --- | --- | ---: | ---: |
| O-LOS | 室外空地、无遮挡、两卡静止 | 1/3/5/8/10/15/20/30 m | 3 次重复 | 24 |

每个距离的 3 次重复必须重新完成完整的 38 秒状态机。重复结果用于观察方差，不能只保留一次读数。

### 3.2 可选的环境矩阵

在 LOS 基线完成并确认 USB 数据持续保存后，再执行以下场景：

| 场景 ID | 场景定义 | 建议距离点 | 每点 Capture 数 |
| --- | --- | --- | ---: |
| O-HUMAN | 人体静态遮挡，操作者保持固定姿态 | 3/5/8/10/15 m | 3 |
| O-DYNAMIC | 动态人体或可控动态遮挡 | 3/5/8/10/15 m | 3 |
| O-FOLIAGE | 灌木/树丛动态遮挡 | 8/15/20/30 m | 3 |
| O-WALL | 墙/围栏等静态遮挡 | 8/15/20/30 m | 3 |

上述遮挡分类目前主要依靠测试记录中的 `scene_id`，不能假设当前 Capture 已经把完整的 `obstruction_class`、信道和 token 字段写入每条记录。场景真值必须写在测试记录中。

如果执行全部表格，预计数量为：

```text
O-LOS       24 次
O-HUMAN     15 次
O-DYNAMIC   15 次
O-FOLIAGE  12 次
O-WALL      12 次
----------------
合计        78 次 Capture
```

这不是“一次开机、一次 NVS 保存”可以完成的数量；它必须依靠 USB 日志连续归档或分批导出。

## 4. 单人和双人安排

### 单人必测

一人可以完成基础 LOS 和静态遮挡：

- 一张卡固定在已测量距离标记点；
- 另一张卡由操作者携带或固定；
- Capture 期间两张卡必须保持静止；
- 在开始前完成摆位，采集期间不操作卡片；
- USB 监听其中一张卡即可，另一张卡观察屏幕状态。

### 双人可选

动态遮挡、同时观察两张屏幕、同时记录两端日志属于 O2 可选项目。一人完成不了时记录：

```text
NOT RUN (O2 optional)
```

不能把未执行的双人项目记录为 PASS。

## 5. 当前 NVS 存储容量分析

### 5.1 分区和 key

当前分区表：

```text
nvs      0x9000  size 0x6000 = 24576 bytes
phy_init 0xf000  size 0x1000
factory  0x10000 size 0x300000
cardid   0x356000 size 0x4000
```

Capture 使用：

```text
namespace: radar_capture
keys:      latest_a / latest_b
```

`nvs` 不是 Capture 专用分区，还同时存储诊断、标定、Wi-Fi/PHY 等数据。

### 5.2 单个 Capture 的实际大小

当前固件一条成功日志示例：

```text
records=99
compressed=1390
uncompressed=1980
header=76
```

因此该次 NVS blob 约为：

```text
76 + 1390 = 1466 bytes
```

实际大小随数据变化。容量预算必须使用最坏 codec 上限，而不能只使用一次成功样本的 1390 bytes：

```text
header 76 + codec payload 上限 8192 = 8268 bytes
```

同时，NVS 的 `used/free/total` 是 entry 统计，不是可直接换算的剩余字节数；NVS entry、页管理、旧值回收和其他 namespace 都会影响实际可写容量。

### 5.3 当前设计实际能保存几次

当前代码只使用两个 key，并按 generation 交替：

```text
latest_a
latest_b
```

因此逻辑上最多保留最近两次有效 Capture。第三次保存会覆盖较旧的 key；它不是追加数据库。

即使暂时忽略双 key 限制，按一次约 1466 bytes 粗略计算，24 KiB 也只能得到约 16 个 blob 的理论上限；扣除 NVS 管理开销和其他 namespace 后不能作为保证值。按 8 KiB 最坏 blob 计算，则只能容纳约 2～3 个大记录，并且仍不能保证，因为分区被共享。

结论：

```text
8 个距离点 × 3 次重复 = 24 次
```

已经超过当前 NVS 的“最近两条”语义；

```text
全部户外场景约 78 次 Capture
```

更不可能全部留在 NVS 中。

## 6. 完整数据集的正式存储方案

### 6.1 USB 日志作为主存储

测试开始前启动自动重连监听器：

```bash
cd /Users/alex/ai-passport/pass-radar
rm -f /tmp/pass-radar-usb-monitor.log
python3 tools/monitor-usb.py \
  --output /tmp/pass-radar-usb-monitor.log
```

每次 Capture 成功后，日志应包含：

```text
CAPTURE_META ...
CAPTURE_CSV ...
CAPTURE_SAVE OK ...
```

因为固件在保存校验成功后立即输出 CSV，USB 日志会累积本次开机期间所有已完成 Capture 的完整数据。主机日志必须在整个测试批次中持续运行，不能在每个 Capture 后删除或覆盖。

每个测试单元的外部记录至少包含：

```text
batch_id
scene_id
run_id
measured_distance_m
target_label_m
card_a/card_b 标识
卡片姿态和方向
环境/遮挡说明
开始时间和结束时间
firmware_version
image_sha256
```

`scene_id` 和测量真值不能只依赖设备的 `TARGET` 标签；必须同时写入测试记录或主机侧批次清单。

### 6.2 NVS 的角色

NVS 只承担：

- 最近两次有效 Capture 的断线恢复；
- USB 断开后的重复导出；
- CRC 和 generation 校验；
- 保存失败时保护上一条有效记录。

NVS 不承担：

- 全天户外测试数据库；
- 24 次或 78 次 Capture 的累积保存；
- 采集期间逐条日志写入；
- 替代 USB 主机归档。

### 6.3 USB 中断时的操作

如果 USB 监听中断：

1. 停止继续采集，避免产生无法归档的数据；
2. 恢复监听器；
3. 在 Debug 页长按 DOWN，重新导出最近一次有效 Capture；
4. 确认日志出现 `CAPTURE_EXPORT OK` 后再继续；
5. 断线期间若已经完成多次 Capture，NVS 不能恢复全部历史，只能恢复最近两次中的有效记录。

如果必须完全脱离 USB 进行户外测试，当前固件只能安全地按“最多两个 Capture 一个批次”执行，并在每批结束后连接 USB 导出。不能把整天数据留在 NVS 后再期待一次性恢复。

## 7. 是否需要修改固件存储方案

针对“保存完整户外数据集”的目标，当前 NVS 方案本身不够，但不建议立刻把 NVS 改成高频流式写入。原因：

- NVS 24 KiB 仍然无法可靠承载 24～78 个 Capture；
- 每个 chunk 写入会增加写放大和掉电恢复复杂度；
- 采集期间写 Flash 可能影响 FTM/BLE/ESP-NOW 时序；
- 还需要设计 chunk 序号、批次 CRC、提交标记和恢复协议；
- USB 已经是固件当前可用且容量足够的流式出口。

因此当前不修改 NVS 为累积存储，采用“USB 主归档 + NVS 最近两条恢复缓存”。如果产品要求完全离线、无 USB 地完成全部 24～78 次 Capture，必须增加独立的大容量数据分区或外部存储，并另行设计批次协议；不能通过现有 24 KiB NVS 小修小补解决。

## 8. 一批户外测试的执行规则

推荐按以下批次组织：

```text
Batch 1: O-LOS，8 个距离点 × 3 次 = 24 次
Batch 2: O-HUMAN/O-DYNAMIC，按当天场地和人员情况分批
Batch 3: O-FOLIAGE/O-WALL，按可用场景分批
```

每次批次开始：

1. 清理或另存主机日志文件；
2. 写入 batch_id 和固件 SHA；
3. 启动 USB 监听；
4. 每个 Capture 完成后检查 `CAPTURE_SAVE OK`；
5. 每完成一个距离点，记录 3 次 run_id；
6. 批次结束后保存原始日志，不手工修改；
7. 另行生成解析后的 CSV，不覆盖原始日志。

### 停止条件

遇到以下任何情况立即停止数据采集：

- `CAPTURE_SAVE` 不是 `OK`；
- `CAPTURE_EXPORT` 失败；
- USB 日志缺少完整 CSV；
- 设备重启、白屏、退出或卡死；
- Capture 记录数为 0 或明显少于预期；
- NVS 报空间不足；
- 两卡链路状态异常且无法恢复。

## 9. 验收标准

本方案验收需要证明：

- 每个 Capture 生成唯一的外部 `run_id` 和场景记录；
- USB 原始日志包含每个成功 Capture 的完整 CSV；
- NVS 断线恢复最多验证最近两次，不把它误称为全量历史保存；
- 24 次 LOS 基础矩阵可以在持续 USB 监听下完整归档；
- 任何缺失的 Capture 都能通过 `CAPTURE_SAVE`、`CAPTURE_EXPORT` 或主机日志发现；
- 原始日志和解析数据分开保存，原始证据不被覆盖。

最终结论：

> **一次 Capture 只能代表一个场景和一个距离窗口；当前 NVS 只能保留最近两条，不能保存全部户外矩阵。完整数据集必须通过持续 USB CSV 归档，或分批导出。当前不引入高频流式 NVS。**
