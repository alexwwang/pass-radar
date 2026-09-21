# pass-radar v3 slot2 全量批次采集与离线模型方案

English version: `pass-radar-v3-slot2-batch-capture-and-offline-model-plan.md`

日期：2026-09-21
适用目标：在户外一次完成全部测试，回到电脑后一次性读取，不要求中途下载。
状态：已为开发者/现场标定版本实现。本功能不是普通用户功能，普通产品运行不依赖它。

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
78 次：78 × 16384 = 1277952 bytes
批次头和边界余量：约 4 KiB
总预算：约 1250 KiB
slot2 数据区：2736128 bytes
完成 78 条最坏 Capture 后剩余约 1.45 MiB
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
scene_id
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
6. 增加 host 批次编码、恢复、容量和 CRC 测试；
7. 在不擦除 NVS 的情况下进行真机单批次验证；
8. 再执行 24/78 次户外矩阵。
