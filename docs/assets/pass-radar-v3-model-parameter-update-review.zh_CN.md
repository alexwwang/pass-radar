# pass-radar v3 数据验算方案与 FORMAT_VERSION 4 代码评审

> English peer: `pass-radar-v3-model-parameter-update-review.md`
> 日期：2026-09-23  
> 评审基线：`db55cc9 feat: FORMAT_VERSION 4 run 级 FTM 原始 session 聚合`  
> 关联设计：`pass-radar-v3-model-parameter-update-program.zh_CN.md`  
> 评审范围：采集数据的语义、完整性、可复核性、格式兼容、FTM 原始 session 聚合、导出链路、测试覆盖。  
> 本文是独立的中文评审记录，不以英文文档为主文档。

---

## 1. 总体结论

**结论：存储和导出链路基本通过；用于正式模型参数拟合的数据验算闭环尚未通过。**

当前代码可以支持以下用途：

- 在 slot2 中追加保存 Capture；
- 通过 CRC、commit marker 和回读检查发现明显的 Flash/压缩数据损坏；
- 导出多个 Capture，并保留场景上下文、距离标签和 FORMAT_VERSION 4 的 FTM run 级统计；
- 在没有 FTM 成功时继续保存 RSSI 数据，不因单次 FTM 失败而崩溃。

当前代码和设计**还不能保证**以下结论：

- 每个 `ftm_raw_*` 聚合值严格只属于当前 Capture run；
- 采集完整后，PC 可以仅凭导出日志复核 CRC、固件来源和真实距离；
- `observed_level` / `observed_obstruction` 代表整个 run，而不是最后一个 100 ms 采样点；
- 当前数据足以自动重放所有文档中规划的分类、方向和扫盘验算；
- 规划中的 `tools/model_fit.py` 已经存在并可执行。

因此，**当前固件可以继续做“采集链路测试”和导出测试，但不建议在修复下述阻塞项前开始正式模型标定数据采集。**

---

## 2. 已确认通过的部分

### 2.1 存储格式与 NOR Flash 写入

以下内容有代码和 host 测试支撑：

- Capture payload 使用固定 codec 上限 `4096 B`；
- batch capture header 保持 `112 B`，FORMAT_VERSION 4 从原 reserved 区划出 12 B；
- header、payload、commit 分别按 4 KiB 对齐计算 span；
- padding 使用 `0xFF`，符合 NOR Flash 只能将位从 1 清为 0 的写入语义；
- payload CRC、record CRC、commit marker 在追加和导出时均有检查；
- `test_radar_capture_batch_flash.c` 使用真实的 RAM NOR AND-write 语义验证了历史 `ESP_ERR_INVALID_CRC` 根因；
- v3/v4 batch header 的读取兼容路径已经加入；
- codec 最坏情况、内存预算、批次 span 和场景字段有 host 断言。

### 2.2 FORMAT_VERSION 4 字段链路

以下链路已经接通：

```text
FTM report callback
  -> 原始 session median（offset/P25 之前）
  -> raw session id / bursts
  -> capture tick 去重聚合
  -> NVS header / batch capture header
  -> batch export META
```

导出 META 当前包含：

```text
ftm_raw_median_m
ftm_raw_iqr_m
ftm_raw_sessions
ftm_raw_bursts
```

### 2.3 自动化检查结果

本次评审执行：

```bash
./tools/validate.sh --static
```

结果：通过。通过项包括：

- 仓库静态检查；
- UI 像素计算、Kalman、融合、扫盘、遮挡、CSI、协议 host 测试；
- capture codec round-trip、最坏 payload 和内存预算测试；
- batch layout、场景合法性和容量测试；
- FTM session 聚合的空集、单值、奇偶数、中值/IQR、去重、溢出和 NULL 安全测试；
- NOR Flash AND-write append/export 测试。

**注意：上述测试没有覆盖本评审第 3 节的运行时语义问题。**

---

## 3. 阻塞正式模型采集/拟合的问题

### P0-DATA-1：Capture 没有保存 FTM session 起始边界

设计文档声称：

```text
radar_capture_start 快照当前 raw session id，
只聚合本 run 内新完成的 session。
```

但实际 `radar_capture_start()` 只执行：

```c
radar_capture_session_agg_reset(&s_session_agg);
```

没有保存 `start_raw_session_id`。`radar_capture_tick()` 直接把当前 observation 的 `ftm_raw_session_id` 送入聚合器。

结果是：

1. Capture 开始前刚刚完成的 FTM session 仍可能留在 FTM 快照中；
2. Capture 开始后的第一个 tick 可能把这个旧 session 当成当前 run 的新 session；
3. FTM session 在 COUNTDOWN/SETTLE 期间完成时，是否属于当前 run 没有明确的边界语义；
4. P1 的 `b0_run` 可能混入前一个观测窗口的数据。

这不是普通显示问题，而是会直接影响 `ftm_offset` 拟合。

**建议修复：**

- 在 `radar_capture_start()` 保存 `s_capture_raw_session_baseline`；
- 只有 `session_id > baseline` 的 raw session 才允许加入；
- 明确定义 COUNTDOWN/SETTLE 是否计入当前 run；建议：倒计时不计入，SETTLE 计入但单独记录阶段，或者全程计入但在文档和元数据中明确；
- 为 session id 回绕增加安全处理；
- 增加单测：开始前已有 id、开始后新 id、重复 tick、id 回绕。

**状态：阻塞 P1 FTM 数据可信度。**

---

### P0-DATA-2：聚合容量 16 与实际 Capture 时间窗口不匹配

当前 Capture 总活动时间为：

```text
COUNTDOWN 3 s + SETTLE 5 s + RUNNING 30 s = 38 s
```

FTM 发起周期约为 2 s，失败还有退避路径。理论上 38 秒窗口最多可能完成约 19 个 session，而聚合器容量为：

```c
#define RADAR_CAPTURE_AGG_MAX_SESSIONS 16u
```

当前设计文档写“30 s run 最多约 8 个 session”，没有把 COUNTDOWN/SETTLE 和 2 秒发起周期完整代入，也没有用仿真时钟证明上界。

超过 16 个 session 后，`radar_capture_session_agg_add()` 静默返回：

```c
if (agg->count >= RADAR_CAPTURE_AGG_MAX_SESSIONS) return;
```

这会导致：

- `ftm_raw_sessions` 被截断；
- `ftm_raw_bursts` 被截断；
- median/IQR 只来自前 16 个 session；
- 没有 overflow 标志，PC 无法知道数据被截断；
- P1 可能产生一个看似正常、实际不完整的 run 统计。

**建议修复：**

- 先按最坏时序计算上限，再决定容量；建议至少 24 或 32；
- 或在容量达到上限时设置 `overflow=true` 并把该 run 标记为 `SUSPECT`，禁止用于 P1；
- 将 `countdown/settle/running` 的 session 计数分别记录，便于验算；
- 加入时序仿真单测，覆盖 38 秒窗口、2 秒周期、成功、超时和退避组合。

**状态：阻塞 P1 FTM 数据可信度。**

---

### P0-DATA-3：`target_m` 不是已确认的真实距离

Capture 页使用预设值：

```text
1 / 3 / 5 / 8 / 10 / 15 / 20 / 30 m
```

代码保存：

```c
.measured_distance_m = -1.0f
```

也就是说，当前每个 batch Capture 都有 `target_m`，但没有保存卷尺或激光测得的真实距离。设计文档虽然说明 `target_m` 可能只是标签，并允许 PC 侧 YAML 覆盖，但当前代码没有提供该 sidecar 的生成、强制关联或验算门禁。

如果拟合器直接使用 `target_m`，就会把“大概距离”误当真值，产生系统性错误；如果没有拟合器，则无法自动保证使用了外部实测值。

**建议修复：**

- 正式采集前明确要求：`target_m` 仅为计划标签，不得直接作为标定真值；
- 为每个 `batch_id/run_id` 建立强制的 PC 侧实测距离 sidecar；
- `fit/report` 默认拒绝 `measured_distance_m < 0` 的 run，而不是静默使用 `target_m`；
- sidecar 至少包含 `batch_id`、`run_id`、`measured_distance_m`、测量方法和操作者；
- 若未来在设备 UI 输入实测值，应明确区分 `target_distance_label_m` 与 `measured_distance_m`。

**状态：阻塞正式距离模型拟合；不阻塞纯链路稳定性测试。**

---

## 4. 高风险但不一定阻塞当前存储的发现

### P1-DATA-4：raw FTM 快照读取不是一致性快照

FTM 回调任务写入多个独立的全局变量：

```text
s_raw_session_id
s_raw_session_m
s_raw_bursts
```

LVGL/主轮询任务在另一个执行上下文读取这三个值。即使每个 32 位写入通常是原子的，也不能保证读取到的是同一个 session 的三元组。例如可能出现：

```text
id = 新值，median = 旧值，bursts = 旧值
```

聚合器会把错误的值与新 id 绑定。

**建议修复：**

- 使用临界区保护完整 raw snapshot；或
- 使用 sequence lock：写入前递增序列、写完整结构、写入后递增序列，读取时只接受前后序列相同的快照；
- 对外提供一个一次性 `radar_ftm_get_raw_session_snapshot()`，不要让调用方分别读取三个 getter；
- host 测试至少验证快照结构和无效序列不会被消费。

**状态：P1，可能造成极少量但不可追溯的错误样本。**

---

### P1-DATA-5：probe session 与 regular session 被混合，`bursts` 无法实现文档所说的加权

代码对 probe session 也递增 raw session id，并送入同一个聚合器。probe 使用 8 帧，regular session 使用 16 帧。聚合结果只有：

```text
各 session median 的 median
各 session median 的 IQR
session 数
所有 burst 数之和
```

只保存 burst 总数，无法恢复“每个 session 的 burst 数”，因此 PC 端不能真正按 burst 对每个 session 加权。更重要的是，8 帧 probe 和 16 帧 regular 的误差分布可能不同，直接混合会改变 P1 的估计。

**建议三选一并写死：**

1. P1 只使用 regular session，probe 仅用于链路诊断；
2. header 分别保存 regular/probe 的 count、median/IQR 和 burst 统计；
3. 保存足以复现加权估计的聚合量，并明确估计公式。

在没有选择前，不能在文档中写“PC 按 bursts 加权”作为已实现语义。

**状态：P1，影响 FTM offset 的统计解释。**

---

### P1-DATA-6：`observed_level` 与 `observed_obstruction` 实际是最后一次采样值

`radar_capture_tick()` 每个有效 observation 都执行：

```c
s_observed_level = observation->observed_level;
s_observed_obstruction = observation->observed_obstruction;
```

保存时只把最后一次值写入 batch header。文档却把它们描述为整个 run 的“设备观测标签”，并计划用它们构建混淆矩阵。

因此一个 30 秒 run 中如果分类状态发生变化，导出的 header 会丢失变化过程，只保留结束时状态。

**建议修复：**

- 明确字段语义为 `observed_at_end`，不要再叫 run-level observed label；或
- 保存 run 内状态计数、占比、首次/最后状态和状态转换次数；或
- 采用多数投票/置信度聚合，并同时保存 `observed_label_quality`；
- 分类拟合应以逐样本可重算的特征和用户条件标签为主，不能只依赖最后一帧。

**状态：P1，阻塞分类阈值的严谨验算。**

---

### P1-DATA-7：batch export 覆盖了真实固件版本

在 `export_batch_record()` 中，NVS fallback header 的 `firmware_version` 被写成：

```c
"slot2-batch"
```

而不是产生该 Capture 的真实固件版本。设计文档要求每条原始记录带固件版本，且模型工具要按固件版本隔离数据；当前 batch export 无法满足这一要求。

`firmware_hash32` 在 batch meta 中也被写成 `0`。

**建议修复：**

- Capture header 保存真实的 `esp_app_desc_t.version` 或稳定 hash；
- batch export 原样透传，不使用 `slot2-batch` 覆盖；
- `BATCH_CAPTURE_META` 增加 `record_crc32`、`payload_length`、真实 `firmware` 和 `firmware_hash32`；
- host parser 将 firmware 缺失视为 provenance warning，并默认禁止跨版本混合拟合。

**状态：P1，影响数据可追溯性和跨固件隔离。**

---

### P1-DATA-8：USB 导出文本不足以让 PC 独立复核完整 CRC

设备内部会校验 payload CRC 和 record CRC，但 `emit_csv()` 对外输出的 META 主要包含：

```text
crc32=payload_crc32
compressed=compressed_length/uncompressed_length
```

没有输出 `record_crc32`，也没有输出足以重建原始压缩 payload 的字节流。PC 端拿到 CSV 文本后可以检查行数、字段和 payload CRC 字段格式，但不能独立重算设备内部的 record CRC。

因此文档中的“PC 验证每个 Capture CRC”目前只能理解为：设备在导出前已验证，而不是 PC 对导出内容完成独立密码学/CRC 验算。

**建议修复：**

- 导出 `record_crc32`、`payload_crc32`、`payload_length`、`record_count`；
- 若要求 PC 独立复核 payload CRC，导出原始压缩 payload（例如 binary/base64 块）或定义稳定的 CSV canonical CRC；
- 明确区分：设备端 Flash 完整性验证、导出文本结构验证、PC 端原始 payload 验证。

**状态：P1，影响验算闭环，但不影响设备内部发现损坏。**

---

### P1-DATA-9：`tools/model_fit.py` 及解析/拟合模块尚未实现

设计文档列出了：

```text
tools/model_fit.py
 tools/model_fit/parser.py
 tools/model_fit/clean.py
 tools/model_fit/features.py
 tools/model_fit/fit_rssi.py
 tools/model_fit/fit_ftm.py
 tools/model_fit/fit_class.py
 tools/model_fit/fit_kalman.py
 tools/model_fit/fit_sweep.py
 tools/model_fit/pack.py
 tools/model_fit/report.py
```

但当前仓库中没有这些文件。`validate.sh` 也没有编译或运行 PC 解析器、数据质量报告、拟合器、参数包生成器或 C/Python 对拍。

**建议修复：**

先实现最小闭环，不要直接实现全部拟合：

1. `parse`：解析真实 monitor 日志并按 `batch_id/run_id` 分组；
2. `report`：检查字段完整、run 数、记录数、CRC/provenance、实测距离 sidecar；
3. `ftm-report`：只计算 P1 所需的 raw session 统计和异常；
4. 在真实三次测试 Capture 导出日志上验收；
5. 再实现 RSSI 回归和参数包。

**状态：P1，意味着当前“验算方案”仍是设计，不是可执行工具。**

---

## 5. 方向、扫盘和遮挡数据的覆盖边界

### 5.1 方向数据没有与 Capture run 建立记录关系

代码只输出：

```text
SWEEP_DONE elapsed_ms=...
SWEEP_SAMPLES count=...
```

扫盘样本没有写入 Capture batch，也没有 `batch_id/run_id` 或场景标签。方向真值只在设计文档中作为 PC sidecar 计划存在。

因此当前 Capture batch 可以支持：

- RSSI 时序；
- FTM 成功/失败和 run 级 raw session 聚合；
- BLE/CSI 等当前记录字段。

但不能仅凭 batch CSV 完整拟合方向模型。方向拟合必须单独建立：

```text
sweep_id
batch_id/run_id
true_bearing_deg
sweep_duration_ms
sample timestamps
channel/sample RSSI
result angle/spread/confidence
```

### 5.2 遮挡特征的“可重放”结论需要输入对账

文档声称可由 CSV 重放 `p2p_1s`、`p2p_10s`、K 因子和 excess loss，但当前记录只保存 `local_rssi`、`peer_rssi` 和部分 FTM 字段；有些融合中间量、窗口边界、FTM session 原始序列和扫盘特征没有直接导出。

这不是说所有特征都绝对无法重算，而是必须先完成 C/Python 对拍，证明：

- 时间窗口边界一致；
- 缺失值/哨兵值处理一致；
- FTM 旧值不会被当成新样本；
- BLE/CSI/链路失效时特征结果一致。

在对拍完成前，分类参数拟合只能标记为未验收。

---

## 6. 文档中的不一致

### 6.1 batch worst-case span 数字不一致

当前代码和 batch layout 测试表明，最大 payload `4096 B` 时：

```text
aligned header = 4096
aligned payload = 4096
aligned commit = 4096
capture span = 12288 B
```

但 slot2 批次方案文档中的容量章节曾写成单次 worst-case `16384 B`，而模型参数文档 §10.5 写成 `12288 B`。

应统一为代码实际定义，并说明：header padding 与 payload 写入区有重叠，但 padding 为 `0xFF`，所以物理 span 仍按三个独立对齐块计算。

### 6.2 “30 秒最多约 8 个 FTM session”缺乏边界证明

当前 Capture 采集活动窗口为 38 秒，不能直接用 30 秒估算 session 上界。文档应给出：

- session 发起周期；
- 成功、失败、timeout、probe 的最小间隔；
- COUNTDOWN/SETTLE 是否计入；
- 聚合容量和 overflow 行为。

### 6.3 `measured_distance_m` 的设计与代码不一致

文档把 `measured_distance_m` 列为可用字段，但当前正式 Capture 路径固定写入 `-1.0f`。在该字段尚未实际填写前，设计文档必须把它标为“未实现”，不能写成当前验算输入。

---

## 7. 建议的修复顺序

### 第一阶段：先保证数据不会被错误归属

1. 增加 raw session baseline，严格限制 run 边界；
2. 将聚合容量扩大到经时序证明的上限，或增加 overflow/SUSPECT 标志；
3. 解决 FTM raw 三元组的一致性快照；
4. 明确 probe 是否进入 P1，并实现与文档一致的统计方式；
5. 增加上述场景的 host 单测和时序仿真。

### 第二阶段：保证数据可以被独立验算

1. 增加真实 firmware/version/hash；
2. 增加 `record_crc32`、payload length 等导出字段；
3. 实现实测距离 sidecar 及缺失门禁；
4. 将 observed 字段改为明确的 run 聚合语义，或输出状态统计；
5. 实现 `model_fit.py parse/report`，先不做参数拟合。

### 第三阶段：再开始模型拟合

1. 在已有测试日志上通过 parse/report；
2. 用三次非正式 Capture 验证 run 分组、行数和 raw FTM 统计；
3. 用小批正式数据先完成 P1/P2 复算；
4. 通过留出验证后再拟合 P3–P10；
5. 方向、扫盘和分类数据建立独立的关联格式后再纳入对应模型。

---

## 8. 最终评审结论与放行标准

### 当前允许

- 继续做 boot、Capture、保存、导出、重启恢复和 CRC/Flash 稳定性测试；
- 使用非正式测试 Capture 检查 UI、日志和数据链路；
- 在电脑上人工检查导出字段和批次容量。

### 当前不允许

- 直接把当前导出数据作为最终 `ftm_offset` 标定数据；
- 只使用 `target_m` 而没有实测距离 sidecar 就拟合 RSSI/FTM 模型；
- 用最后一帧 `observed_*` 建立整个 run 的分类混淆矩阵；
- 声称方向模型可以由当前 batch CSV 自动验算；
- 在 `model_fit.py` 尚未实现前声称“离线参数更新程序已可运行”。

### 正式采集放行条件

以下条件全部满足后，才允许开始正式参数采集：

- raw session baseline 已实现并有测试；
- 聚合 overflow 不会静默丢数据；
- probe/regular 统计语义已确定；
- FTM snapshot 为一致性读取；
- 每个 run 有可验证的实测距离；
- 导出包含真实 firmware provenance 和足够的 CRC 验算信息；
- `model_fit.py parse/report` 能对真实日志输出 PASS/SUSPECT/REJECTED；
- 非正式三次 Capture 可以被工具完整解析并对账；
- 方向和分类拟合所需的额外数据缺口已明确隔离，不把缺失字段当作已存在。

**最终判定：FORMAT_VERSION 4 的固件数据保存改动可以作为下一轮修复基础，但当前数据验算方案应评定为“条件通过，暂不放行正式模型标定采集”。**
