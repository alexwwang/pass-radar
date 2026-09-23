# pass-radar v3 Capture 内存调试与安全分配方案

> English version: `pass-radar-v3-capture-memory-debug-and-safe-allocation-plan.md`

日期：2026-09-21

状态：方案已确认，正在实施；本文件记录调试证据、决策依据和验收边界。

> 2026-09-23 更新：首次真机 `start capture` 以 `ESP_ERR_NO_MEM` 失败。
> 第 8 节记录了捕获到的证据，以及恢复符合 5.2 节要求的修订分配方案。

## 1. 问题背景

Debug Capture 原设计在运行期间保留全部原始记录，保存时再申请编码/NVS scratch。原始预算如下：

```text
最大记录数：128
单条原始记录：32 bytes
记录缓冲：128 × 32 = 4096 bytes

存储 header：76 bytes
codec 最大 payload：8192 bytes
scratch：76 + 8192 = 8268 bytes

记录 + scratch：12364 bytes
安全余量：2048 bytes
理论安全预算：14412 bytes
```

设备是 ESP32-C3，Capture 与 Wi-Fi、FTM、BLE、LVGL 共用内部 8-bit heap。启动日志中的 RAM 区域总量不能代表 Capture 可用连续内存，必须使用：

```text
MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
```

读取：

```text
free
largest free block
minimum free since boot
```

## 2. FTM 错误排查过程

### 2.1 小于 1 米场景

曾出现大量：

```text
wifi: No FTM Measurements received from responder!
radar_ftm: FTM_SESSION ... result=FAIL status=5
```

同时 initiator、token 和 BLE 均正常，内存没有异常下降。日志中的 RSSI 约为 `-22` 到 `-25 dBm`，两卡距离小于 1 米。

### 2.2 3 米复测

清空日志并将一张卡放到约 3 米后：

```text
FTM_SESSION result=OK：36 次
FTM_SESSION result=FAIL：0 次
No FTM Measurements received：0 次
status=5：0 次
status=3：0 次
BLE 初始化错误：0 次
panic/watchdog：0 次
```

结论：该阶段的持续 `No FTM Measurements` 与小于 1 米近距离场景高度相关，不应归因于 Capture 内存或 FTM 编译配置。后续精度测试必须使用受控距离，不能用近场结果替代正常距离结果。

## 3. 实测内存证据

内存日志通过 USB Serial/JTAG 输出，由 `tools/monitor-usb.py` 保存到：

```text
/tmp/pass-radar-usb-monitor.log
```

日志格式：

```text
MEM_STATS stage=RADIO_READY free=... largest=... min=...
MEM_STATS stage=IDLE free=... largest=... min=...
CAPTURE_MEM stage=... free=... largest=... min=... required=... margin=...
```

3 米待机/FTM窗口实测：

```text
RADIO_READY free：约 106284 bytes
RADIO_READY largest：约 98304 bytes

待机 free：约 38096 到 41424 bytes
待机 largest：稳定为 14336 bytes
启动以来 minimum free：约 22304 bytes
```

FTM session 期间 free 会短暂下降，但 `largest` 仍稳定为 14336 bytes。这说明当前问题不是持续的内存碎片崩溃，但 Capture 保存峰值确实接近连续块上限。

## 4. 原方案为何不安全

原方案的理论安全预算为：

```text
记录 4096 + scratch 8268 + 余量 2048 = 14412 bytes
```

实测最小最大连续块为：

```text
14336 bytes
```

因此原方案超出实测连续块：

```text
14412 - 14336 = 76 bytes
```

不能因为总 free 仍有 38 KB 以上，就认为一次性分配安全。连续块大小才是该场景的硬约束。

## 5. 最终方案

已确认采用：

```text
紧凑记录
→ Capture 完成后一次性编码
→ 一次性写入现有 NVS 双 key
→ 回读、解码、CRC 校验
→ 校验成功后导出和报告成功
```

不采用采集期间流式写 NVS。

### 5.1 紧凑记录

原始记录从 32 bytes 降至约 20 bytes/条，使用定点和紧凑整数：

- elapsed 使用适合 30 秒窗口的定点/差分表达；
- RSSI 使用 `int8`；
- 距离和 spread 使用厘米定点；
- flags 使用 bit 位；
- session、BLE age、CSI frame 使用有界整数；
- environment 和状态使用 bit 位或小整数。

128 条记录的目标缓冲预算：

```text
128 × 20 = 2560 bytes
```

### 5.2 动态编码预算

不再无条件申请固定的 `76 + 8192` bytes。编码缓冲按实际记录数计算，并在分配前检查：

```text
largest_free_block >= records_required + scratch_required + safety_margin
```

安全余量目标为至少 2048 bytes。任何不满足预算的情况必须：

- 输出实际 `free/largest/min/required/margin`；
- 显示 `CAPTURE RAM FAIL` 或明确的 `CAPTURE LIMIT`；
- 不静默丢样本；
- 不覆盖上一条有效 NVS 记录。

### 5.3 NVS 写入策略

继续复用 meta-pass 的现有 NVS 分区：

```text
namespace: radar_capture
keys: latest_a / latest_b
```

采集期间不写 Flash。Capture 完成后批量写一次，保留 generation、CRC、回读和旧记录保护。

原因：

- 避免每条样本写 Flash；
- 减少 NVS 写放大和磨损；
- 不干扰 BLE、ESP-NOW、FTM 时序；
- 掉电一致性更容易验证；
- 不引入 chunk 恢复协议。

## 6. 验收标准

### 内存

- 128 条紧凑记录的最坏缓冲不超过设计预算；
- 运行时使用 `largest` 而不是仅使用 `free` 判断；
- 采集、编码、保存前后都有内存日志；
- 不发生 panic、watchdog 或静默截断。

### FTM/BLE

- 3 米受控场景下不持续出现 `No FTM Measurements`；
- FTM 失败时仍可明确降级 RSSI；
- BLE 状态和内存日志均可从 USB 读取；
- Capture 不改变 FTM/BLE 初始化结果。

### 存储

- NVS 空间不足和 RAM 不足分别报告；
- CRC 或回读失败不得报告 `CAPTURE_SAVE OK`；
- 保存失败时上一代有效记录保持不变；
- CSV/JSONL 导出字段稳定。

## 7. 当前禁止事项

在安全分配代码和边界测试完成前：

- 不进行参数标定型 Capture；
- 不根据手工结果调整 FTM/P25/RSSI 参数；
- 不把近距离 `No FTM Measurements` 直接判定为固件回归；
- 不将高频流式 NVS 写入作为临时补丁；
- 不刷未包含最新内存预算实现的 Capture 固件。

## 8. 2026-09-23 更新：首次 start capture 以 ESP_ERR_NO_MEM 失败

### 8.1 捕获的证据

正式固件在 BLE、ESP-NOW、FTM、LVGL 运行数分钟后，`start capture` 在采样开始前被拒绝。USB 监控日志（`/tmp/pass-radar-usb-monitor.log`）显示：

```text
MEM_STATS stage=IDLE free=30660 largest=8192 min=11912
CAPTURE_MEM stage=storage-before free=30424 largest=8192 min=11496 required=10828 margin=1536
E radar_capture: Capture storage rejected: largest=8192 required=10828 margin=1536
E radar_capture: CAPTURE_START FAIL RAM
```

解读：

- 约束从来不是总空闲堆（30424 bytes）；
- 最大连续内部 8-bit 块已碎片化到 8192 bytes，远低于第 3 节早前观测的 14336 bytes；
- `ensure_storage()` 把记录（2560 bytes）和 scratch（76 + 8192 = 8268 bytes）作为一个 10828 字节的整块申请，在 8192 字节连续块内永远放不下，与总空闲堆无关；
- 固定的 8192 字节 codec payload 上限同时违反了 5.2 节「编码容量必须推导而非固定」的要求。

### 8.2 codec 上限分析

每条编码记录是 1 字节 tag 加 12 个 varint 字段。`uint16` 字段和 zigzag 后的 `int16` 字段最多 3 字节；`int8`/`uint8` 字段（含 delta）最多 2 字节：

```text
elapsed(u16,3) ftm_status(i16,3) ftm_dist(u16,3) ftm_spread(u16,3)
ftm_sessions(u16,3) ble_age(u16,3) csi(u16,3)
local(i8,2) peer(i8,2) flags(u8,2) ble_rssi(i8,2) env(u8,2)
= 7*3 + 5*2 = 31 字节 + 1 tag = 32 字节/条
```

FIRST 与 DELTA 路径的最坏情况相同，因此 128 条记录的精确 payload 上限是 `128 * 32 = 4096` 字节，而不是 8192。现在有一个 host 测试用对抗性输入恰好填满该上限（4096 字节），并验证少 1 字节时被拒绝。

### 8.3 修订后的分配设计

```text
记录缓冲：  128 * 20 = 2560 bytes   （分配 1）
scratch：   76 + 4096 = 4172 bytes  （分配 2）
每次分配必须满足：
  largest_free_block >= 分配量 + 2048（余量）
最坏单块需求：4172 + 2048 = 6220 bytes < 8192 实测下限
总需求：6732 bytes（此前：10828 bytes）
```

关键修改：

- `RADAR_CAPTURE_CODEC_MAX_PAYLOAD`：8192 → 4096（精确最坏值）；
- `RADAR_CAPTURE_BATCH_MAX_PAYLOAD`：8192 → 4096（与 codec 上限保持相等；单次 capture 的 flash 占用从 16384 缩小到 12288 字节，78～160 次采集的批次容量实际变大了）；
- `RADAR_CAPTURE_BUDGET_SAFETY_MARGIN`：1536 → 2048（对齐 5.2 节）；
- `ensure_storage()` 改为两次独立分配，每块单独检查 `largest`，不再合并成单块；
- 部分失败清理：若记录块已存在而 scratch 分配失败，两块都不泄漏，且按块报告失败。

### 8.4 验证

- `capture_codec_worst_case: PASS (4096 bytes, bound 4096)` —— 对抗性输入恰好填满上限；更小的缓冲被拒绝；
- `capture_memory_budget: PASS` —— 两块都满足实测 8192 字节下限加 2048 字节余量；
- 全部既有 host 测试与仓库检查保持 PASS；
- 批次容量测试已按 12288 字节最坏 span 更新，78 次和 160 次采集均继续通过。
