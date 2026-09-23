# pass-radar v3 slot2 全量批次采集与离线模型方案

English version: `pass-radar-v3-slot2-batch-capture-and-offline-model-plan.md`

日期：2026-09-21
适用目标：在户外一次完成全部测试，回到电脑后一次性读取，不要求中途下载。
状态：开发者/现场标定版本已经实现户外 Capture MVP。第 3.3 节定义的完整室内/室外条件标签方案是下一阶段实现目标。本功能不是普通用户功能，普通产品运行不依赖它。

## 1. 用户场景和设计目标

开发者/测试人员需要把两张卡带到户外，连续完成完整测试矩阵，然后回到电脑后读取全部数据。不能要求测试人员每完成一次 Capture 就回到电脑下载，再重新出门。普通用户不运行 Capture，不需要 slot2 数据存储，也不参与模型标定。

目标流程：

```text
将正式固件安装到可用的 meta-pass 槽位（当前部署使用 slot0）
  -> 保持空 slot2 作为 Capture 数据区
  -> 户外连续完成全部 Capture
  -> 所有原始数据留在 slot2 数据区
  -> 回到电脑
  -> USB 一次性导出完整批次
  -> 电脑完成最终模型拟合和独立验证
```

## 2. 为什么不把最终模型直接在卡片上完成

开发者专用的 Capture 路径只做有限的在线统计，不直接修改正式运行参数。普通产品用户不需要进入这条路径。最终拟合和验证放在电脑上，原因是：

- 需要清理异常值、错误距离标签和无效场景；
- 需要按距离、场景、姿态和方向分组；
- 需要拆分拟合数据和验证数据；
- 需要比较多个 RSSI/FTM/方向模型；
- 需要保留旧模型并可回滚；
- 方向模型需要完整扫盘序列和真实方位标签；
- 卡片上立即改变模型会产生“模型影响后续采集”的反馈污染；
- 只保存模型摘要会丢失复盘和重新拟合所需的原始证据。

卡片上的在线统计只用于：

```text
显示采集进度、记录样本数、FTM成功率、均值、方差和残差摘要
```

在线统计不得覆盖原始数据，也不得自动替换正式参数。

## 3. 采集矩阵

### 3.1 基础 LOS 矩阵

```text
距离：1 / 3 / 5 / 8 / 10 / 15 / 20 / 30 m
每个距离：3 次重复
总 Capture：8 × 3 = 24 次
```

### 3.2 完整户外矩阵

| 场景 | 距离点 | 重复次数 | Capture 数 |
| --- | --- | ---: | ---: |
| O-LOS：无遮挡视距 | 1/3/5/8/10/15/20/30 m | 3 | 24 |
| O-HUMAN：静态人体遮挡 | 3/5/8/10/15 m | 3 | 15 |
| O-DYNAMIC：动态人体遮挡 | 3/5/8/10/15 m | 3 | 15 |
| O-FOLIAGE：树丛/灌木遮挡 | 8/15/20/30 m | 3 | 12 |
| O-WALL：墙/围栏遮挡 | 8/15/20/30 m | 3 | 12 |
| **合计** |  |  | **78** |

每个 Capture 仍只允许一个距离、一个场景、一个固定姿态和一个约 38 秒窗口。

### 3.3 完整条件标签设计

`O-` 前缀只保留给第一阶段户外采集，并不表示所有 Capture 都只能是户外场景。室内数据必须使用独立的条件命名空间，不能通过显示字符串推断后混入户外标签。

每条原始 Capture 必须同时保存两组相互独立的标签：

```text
TEST CONDITION（Capture 前由开发者设置的测试条件）
condition_scene_id       稳定的组合 ID，例如 O-LOS 或 I-MULTIPATH
condition_environment    OUTDOOR 或 INDOOR
condition_obstruction   NONE / HUMAN_STATIC / HUMAN_DYNAMIC / FOLIAGE / WALL / METAL
condition_propagation   LOS / NLOS / MULTIPATH

DEVICE OBSERVATION（根据采集到的无线数据自动计算）
observed_level           L0_LOS / L1_INDOOR / L2_NLOS / L3_LOST
observed_obstruction     NONE_LOS / FAR_LOS / ATTENUATING_STATIC /
                         FOLIAGE_DYNAMIC / REFLECTION / UNKNOWN
```

菜单和电脑端解析器使用以下稳定的组合条件 ID：

| ID | 环境 | 遮挡 | 传播特征 | 阶段 |
| --- | --- | --- | --- | --- |
| O-LOS | 室外 | 无 | LOS | 户外基线 |
| O-HUMAN | 室外 | 静态人体 | NLOS | 户外遮挡 |
| O-DYNAMIC | 室外 | 动态人体 | NLOS | 户外遮挡 |
| O-FOLIAGE | 室外 | 植被 | NLOS | 户外遮挡 |
| O-WALL | 室外 | 墙体/围栏 | NLOS | 户外遮挡 |
| O-METAL | 室外 | 金属围栏/结构 | MULTIPATH/NLOS | 户外遮挡 |
| O-MULTIPATH | 室外 | 无或未知 | MULTIPATH | 户外多径 |
| I-LOS | 室内 | 无 | LOS | 室内基线 |
| I-HUMAN | 室内 | 静态人体 | NLOS | 室内遮挡 |
| I-DYNAMIC | 室内 | 动态人体 | NLOS | 室内遮挡 |
| I-WALL | 室内 | 墙体/隔断 | NLOS | 室内遮挡 |
| I-METAL | 室内 | 金属/家具 | MULTIPATH | 室内多径 |
| I-MULTIPATH | 室内 | 无或未知 | MULTIPATH | 室内多径 |

以上是菜单预设，不是所有有效组合的全集；上面的结构化维度和组合 ID 规则才是权威定义。

#### ELI5 操作规则

按下 START 前，先用最简单的问题确认条件：

| 问题 | 字段 | 判断规则 |
| --- | --- | --- |
| 两张卡在哪里？ | environment | 建筑物内部 = INDOOR；空地/公园/道路 = OUTDOOR。 |
| 两张卡中间有什么？ | obstruction | 没有 = NONE；静止的人 = HUMAN_STATIC；移动的人 = HUMAN_DYNAMIC；树丛/树木 = FOLIAGE；墙/围栏/隔断 = WALL；大型金属物体 = METAL。 |
| 主要信号路径怎么走？ | propagation | 能直接看见且路径无遮挡 = LOS；直达路径穿过或绕过障碍 = NLOS；存在明显回波或多个可能路径 = MULTIPATH。 |
| 空间是什么形状？ | room_geometry | ROOM、CORRIDOR、OPEN_HALL、CORNER、STAIR 或 OUTDOOR_OPEN。 |
| 空间有多大？ | room_size | SMALL、MEDIUM 或 LARGE；尽量在测试记录中填写实际尺寸。 |
| 主要表面是什么？ | material | SOFT、MIXED、HARD 或 METAL_GLASS。 |
| 空间有多拥挤？ | furniture_density | LOW <20% / MEDIUM 20–50% / HIGH >50%，按大型家具/固定反射体占可用面积估计。 |
boundary
card_height_class
card_pose
observed_level
observed_obstruction

 | LOW、MEDIUM 或 HIGH；只统计大型反射体/遮挡物。 |
| 附近有重要边界吗？ | boundary | OPEN >2m、WALL_NEAR <=2m、AT_CORNER <=2m、DOORWAY 或 PARTITION。 |
| 卡片怎么拿？ | card_height/card_pose | 记录高度，以及 UPRIGHT、FLAT 或 MIXED；重复测试保持一致。 |

不要因为室内没有人就把所有测试都写成 `I-LOS`。走廊、墙角、金属柜和明显回波都必须保留上下文。看到异常结果后也不要反过来修改条件标签：条件标签描述测量前的实际布置，`observed_*` 字段描述设备采集后观察到的无线现象。如果换了房间、家具布局、卡片高度或姿态，就应使用新的 `room_id/layout_id` 条件。

建议的室内起始场景：

```text
I-LOS + ROOM + MIXED + MEDIUM
I-LOS + CORRIDOR + HARD
I-MULTIPATH + CORNER 或 METAL_GLASS
I-WALL + PARTITION 或 WALL_NEAR
```

当前 78 次户外矩阵使用 O-LOS、O-HUMAN、O-DYNAMIC、O-FOLIAGE 和 O-WALL。开始室内、金属或多径采集前，必须先把其他条件 ID 加入固件。设备必须同时保存组合 ID 和拆解维度，电脑端不能依赖字符串解析标签。

### 3.4 一个统一的室内/室外混合批次

室内和室外不是两个 Capture 阶段，也不是两套存储格式，而是一个统一批次中的独立条件维度。采集顺序自由：开发者可以先采集室内，再在同一批次中继续采集室外。

条件使用稳定的 16 位组合 ID 编码，而不是只能表示 12 个固定标签：

```text
condition_scene_id = (environment << 8) | (obstruction << 4) | propagation
```

拆解后的维度仍是电脑端分析的权威字段。UI 提供常用组合，记录格式则支持所有有效的环境/遮挡/传播组合，不需要再次修改二进制结构。

原来的 78 次户外 Capture 仍是计划中的一个子集，不再是整个批次的最大值。批次容量门禁根据 slot2 实际空间和最坏 Capture span 计算。室内和室外记录可以任意顺序追加，直到剩余空间不足以完整容纳下一次 Capture；此时必须在倒计时开始前拒绝新的 Capture，不能覆盖或静默丢弃已有条件数据。

批次生命周期统一为：

```text
NEW BATCH
  -> 按任意顺序追加室内和/或室外 Capture
  -> 完成整套活动后一次性导出
  -> 电脑端将可疑 run 标记为 SUSPECT/REJECTED，不修改原始记录
```

如果预定总次数超过容量，必须在明确的活动边界导出当前批次，再由用户明确创建新批次。这是容量限制，不是室内/室外设计上的拆分。

## 4. slot2 数据布局

meta-pass 分区表确认：

```text
ota_2: 0x560000 ~ 0x7FE000，size=0x29E000，约2.61 MiB
```

当前 slot0 部署在不修改 meta-pass 分区表的前提下，使用空的 slot2 作为 Capture 数据区：

```text
Capture data start: 0x560000（ota_2 起始）
Capture data end:   0x7FD000（最后 4 KiB 保留给 slot 显示名）
Capture data size:  0x29D000 = 2736128 bytes
```

固件运行时只在 slot2 不是当前运行分区，且 slot2 为空或包含有效的 pass-radar 批次头时使用它。发现 slot2 被其他内容占用时，直接拒绝使用，不擦除、不追加覆盖。

约束：

- 开发者部署可以把 pass-radar 运行在 slot0 或 slot1；当前运行在 slot0 时，可以把空 slot2 保留为 Capture 数据区；普通发布版本不依赖 slot2；
- 构建门禁必须检查应用末尾不得进入 Capture data 区；
- meta-pass slot2 安装器只写入镜像覆盖范围，不擦除整个 ota_2；
- 重新安装/更新固件前必须保留数据区；
- 不得覆盖 `cardid@0x356000` 或 `otadata@0x7FE000`；
- 数据区不是 app 分区表中的独立 data partition，而是 slot2 尾部的受控原始 Flash 区域，故只能由本项目固定偏移访问。

## 5. 容量预算

当前一次 Capture 约 99～100 条记录，实际压缩 payload 约 1.4 KiB。容量设计使用最坏上限而不是平均值：

```text
每个 Capture 最坏物理占用：header 对齐 4096 + payload 对齐 8192 + commit 对齐 4096 = 16384 bytes
Capture 数据从对齐后的批次头 4096 bytes 之后开始。
slot2 理论最坏容量为 167 条 Capture；实现会保留安全余量，并在运行时显示准确的剩余容量。
slot2 数据区：2736128 bytes
室内和室外记录共享同一追加式容量；切换环境不会重置或覆盖当前批次。
```

如果未来单次 payload 超过上限、记录字段增加或应用边界增长，固件必须拒绝继续追加并显示空间不足，不能覆盖已提交数据。

## 6. 批次存储格式

采用追加式、4 KiB 对齐的批次格式，不在采集期间逐条写 NVS。

```text
BATCH_HEADER
CAPTURE_HEADER + compressed payload + COMMIT
CAPTURE_HEADER + compressed payload + COMMIT
...
BATCH_SUMMARY
BATCH_COMMIT
```

每个 Capture header 至少包含：

```text
magic
format_version
batch_id
run_id
condition_scene_id
condition_environment
condition_obstruction
condition_propagation
room_geometry
room_size
material
furniture_density
boundary
card_height_class
card_pose
observed_level
observed_obstruction
target_distance_label_m
measured_distance_m
capture_id
record_count
payload_length
payload_crc32
firmware_version
boot_sequence
reset_reason
```

每条 Capture 单独 CRC。只有写入完成并验证回读后，才写该 Capture 的 commit marker。批次完成后写 BATCH_COMMIT。重启扫描时：

### 6.1 NOR 写入语义（已实施修复）

所有带填充的写入，对齐填充区一律填 `0xFF`，禁止填 `0x00`。

原因：ESP32 NOR flash 写入只能把位从 1 清成 0（等效按位与 `new & old`）；要把位写回 1 必须整扇区擦除。Capture 记录布局先把 112 字节 header 作为一个填充到 4 KiB 的块写入，然后从 `offset + 112` 开始写 payload——这个位置与 header 块的填充区重叠。如果填充是 0x00，header 写入就把这些 flash 字节清成了 0，随后的 payload 写入无法恢复（`payload_byte & 0x00 == 0x00`），payload CRC 在导出时校验失败，返回 `ESP_ERR_INVALID_CRC`，而 append 当时报告成功（真机观测：`batch append ... OK`，随后 `BATCH_EXPORT captures=0 used=4096`、`CAPTURE_EXPORT batch failed err=ESP_ERR_INVALID_CRC`）。

填充改为 `0xFF` 后，每个填充字节对写入是中立的（`0xFF & x == x`），payload 完整落盘。该行为已由 host 测试 `test_radar_capture_batch_flash.c` 锁定：该测试把真实 batch 模块编译到带真实 NOR 按位与写语义的 RAM flash mock 上，`0xFF` 填充时通过，`0x00` 填充时失败，精确复现原始真机故障。

header/payload/commit 三个区间的跨度独立计算（`radar_capture_batch_commit_offset` 与 `radar_capture_batch_capture_span`），后续写入不会覆盖前一条记录已提交的区域。

重启扫描时：

- 只接受有效 header、长度、CRC 和 commit marker；
- 遇到未提交尾记录时停止扫描，不拼接半条数据；
- 已提交记录保留；
- 不覆盖上一批有效数据，除非用户明确执行“新建批次”。

## 7. 采集期间的在线摘要

卡片可以同步维护候选摘要：

```text
每距离/场景样本数
FTM成功率
RSSI均值/方差
FTM bias均值/方差
P25/IQR
RSSI fallback比例
BLE可用率
```

摘要必须带：

```text
model_candidate_version
batch_id
sample_count
scene_id
```

摘要不是最终参数，不得自动覆盖当前运行模型。它的用途是：

- 确认采集是否足够；
- 发现某个场景完全没有 FTM；
- 回到电脑后快速检查批次质量；
- 作为原始数据解析的辅助索引。

## 8. 电脑端最终处理

回到电脑后通过 USB 导出完整批次：

```text
BATCH_META
CAPTURE_META/CAPTURE_CSV ...
BATCH_SUMMARY
BATCH_COMMIT
```

电脑端流程：

1. 校验整批 CRC 和每条 Capture CRC；
2. 检查 batch/run/scene/distance 标签；
3. 检查记录数、缺失批次和未提交尾记录；
4. 按 LOS、人体、动态、树丛、墙体分组；
5. 删除或标记异常数据，但不修改原始文件；
6. 拆分拟合集和验证集；
7. 拟合 RSSI 路损、FTM offset/P25/IQR 和遮挡残差模型；
8. 使用未参与拟合的数据验证误差、成功率和误报率；
9. 生成版本化候选参数；
10. 真机回归后才允许成为正式参数。

方向模型必须使用带真实方位和扫盘序列的数据；普通状态 Capture 只能支持测距/链路统计，不能单独完成方向模型拟合。

## 9. 真机交互和重复采集

交互分为两级。UP 双击专门用于进入或退出 DEBUG 模式，不再承担 Capture 功能：

```text
STATUS -> UP 双击 -> DEBUG
DEBUG  -> UP 双击 -> STATUS
```

DEBUG 页面显示明确的 `CAPTURE [OK]` 入口。OK 短按进入独立的 CAPTURE 菜单，不直接开始采集：

```text
DEBUG：OK 短按             进入 CAPTURE 菜单
CAPTURE：UP/DOWN 短按      移动菜单选项
CAPTURE：OK 短按           修改/选择当前选项
CAPTURE：UP/DOWN 短按      在 SCENE 或 DIST 中切换具体值
CAPTURE：START：OK 短按    开始一次 Capture
CAPTURE：EXPORT：OK 短按   导出 slot2 当前批次全部数据
CAPTURE：NEW BATCH：OK 短按进入擦除确认，再按 OK 执行
CAPTURE：BATCH STATUS：OK  显示数量和剩余空间
CAPTURE：BACK：OK          返回 DEBUG
```

DOWN 长按仅在 CAPTURE 菜单内保留为导出快捷方式。同一场景和同一距离的计划内重复 Capture 使用新的 `run_id` 追加保存，全部保留用于方差分析，绝不覆盖之前的数据。设备不保存无限多个完整历史批次，只保留一个当前批次。如果怀疑某次测试有偏差，回到电脑后将该 run 标记为 `SUSPECT` 或 `REJECTED`；确认批次已经导出后，再明确执行“新建批次”擦除旧批次并重新采集。只有“新建批次”操作允许擦除旧批次。

## 10. 读取和清除

默认导出不清除数据。只有在电脑确认：

```text
批次完整
CRC全部通过
导出文件已保存
```

之后，才允许用户明确执行清除并开始新批次。清除必须擦除整个 Capture 数据区并写入新的空批次 header；不能通过普通重启或重新启动 Capture 隐式清除。设备策略是一个当前完整批次，批次内部保留计划内重复；疑似数据在导出和复核前保留，但不在卡片上长期保留多个完整历史批次。

## 11. 方案取舍

### 仅在线更新模型

不采用。它不能可靠处理错误标签、遮挡混合、异常值、方向真值和模型回滚。

### 仅保存 NVS

不采用。24 KiB 共享 NVS 且当前只有两个 key，无法保存 78 次 Capture。

### slot2 原始批次 + 在线摘要 + 电脑拟合

采用。它同时满足：

- 一次户外完成全部测试；
- 保留原始证据；
- 支持断电恢复；
- 回来后一次性读取；
- 在线查看采集质量；
- 电脑端可重复拟合和验证；
- 不让候选模型污染采集过程。

## 12. 实施顺序

1. 增加 slot2 数据边界和应用大小门禁；
2. 实现 raw Flash 批次格式、CRC 和提交标志；
3. 将 Capture 保存接入批次追加区；
4. 增加批次导出和完整性摘要；
5. 增加在线候选摘要，不改变正式模型；
6. 增加完整条件标签和设备观察分类字段；
7. 增加 host 批次编码、恢复、容量、标签一致性和 CRC 测试；
8. 在不擦除 NVS 的情况下进行带标签的真机单批次验证；
9. 再执行 24/78 次户外矩阵；
10. 验证统一的室内/室外混合批次；可以先采集室内，再不改变批次格式地继续采集室外。

## 13. 最小场景矩阵与 Capture 编辑器基线

此前的 `SCENE + SPACE` 菜单描述由以下设计取代，不能再作为新标定数据的实现依据。

### 13.1 核心模型矩阵

第一版模型只使用两个维度：

```text
ENVIRONMENT：    INDOOR / OUTDOOR
LINK CONDITION： LOS / STATIC_ATTENUATION /
                 DYNAMIC_ATTENUATION / MULTIPATH
```

因此得到 8 个核心场景：

```text
I-LOS、I-STATIC、I-DYNAMIC、I-MULTIPATH
O-LOS、O-STATIC、O-DYNAMIC、O-MULTIPATH
```

`HUMAN`、`FOLIAGE`、`WALL`、`METAL`、`GLASS` 等是上下文说明，不是额外的模型轴。空间类型、材料、家具密度、边界、卡片高度和卡片姿态只作为上下文保存，不形成笛卡尔积。

最低可用矩阵为 48 次 Capture：8 个场景、3 个距离、每个 2 次重复。推荐第一版矩阵为 72 次 Capture，每个场景重复 3 次。第一轮建议距离为 1 m、3 m、5 m。

### 13.2 选项可用性规则

`ENVIRONMENT` 和 `LINK CONDITION` 是核心维度，永远不能灰显。操作员必须始终能够修改它们，从不兼容的上下文中切换出来。

只有上下文字段的候选值可以灰显。例如选择 `OUTDOOR` 后，`ROOM`、`CORRIDOR`、`OPEN_HALL` 为灰色，`OUTDOOR_OPEN` 可选；选择 `INDOOR` 后，`OUTDOOR_OPEN` 为灰色。

ROOM SIZE 候选项必须显示与空间类型对应的量化范围：ROOM 为 `<20 m² / 20–60 m² / >60 m²`，OPEN_HALL 为 `<50 / 50–150 / >150 m²`，CORRIDOR 为 `<10 / 10–30 / >30 m`，CORNER 为 `<3 / 3–6 / >6 m`，STAIR 为 `<5 / 5–15 / >15 m`。`OUTDOOR_OPEN` 不适用 ROOM SIZE。

硬冲突的上下文值不能确认。合法但容易误解的组合仍可选择，但 Capture 前必须显示警告。核心维度变化时立即重新计算上下文可用性；已经失效的上下文值自动回退到合法默认值，并向操作员显示回退提示。

### 13.3 界面布局与固定统计区

Capture 使用独立字段编辑器，不再使用隐藏含义的预设：

```text
ENVIRONMENT
LINK CONDITION
SPACE
OBSTACLE
MATERIAL
BOUNDARY
DISTANCE
START CAPTURE
EXPORT BATCH
NEW BATCH
BATCH STATUS
```

Capture 编辑页不设置 `BACK` 候选项。上下键只在真实候选值之间移动；选中合法值后按 `OK` 确认并立即返回 Capture 菜单。候选值必须作为独立条目绘制。可用值使用正常亮度，不可用的上下文值使用灰色且不能选择。倒计时开始前仍必须执行一次完整校验。

批次统计和临时操作结果不得再追加到菜单文本末尾。固定布局为：

```text
上部：条件编辑区
中部：当前值/候选值
底部第 1 行：COUNT 12  SPACE 82%
底部第 2 行：STATUS READY / SAVED / EXPORT OK
```

统计和状态使用独立的固定位置 UI label，更新它们不能增加菜单高度，也不能裁切最后一行。

### 13.4 旧版本边界

`v0.1-33` 使用的是已经被取代的打包场景/空间行为，不得继续用来生成本设计的标定数据。新记录必须带条件 schema 版本，并将操作员设置的条件字段与设备观察字段分开保存。旧记录仍可导出，但不能自动转换成 8 个核心场景。
