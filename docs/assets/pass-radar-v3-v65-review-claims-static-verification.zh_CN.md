# pass-radar v3 v65 固件评审问题与 v5 修复设计静态验证报告

> English peer: `pass-radar-v3-v65-review-claims-static-verification.md`
> 日期：2026-09-23  
> 验证对象 1：`db55cc9`（v0.1-65，FORMAT_VERSION 4）上评审报告声称的问题  
> 验证对象 2：`pass-radar-v3-model-parameter-update-remediation-design.zh_CN.md` 提出的修复方案  
> 方法：逐声明静态代码证据 + 独立数值重算 + 设计正确性推演；不涉及真机。  
> 本文与英文版平等，无主次。

---

## 1. 总体验收结论

| 类别 | 结论 |
| --- | --- |
| 评审声称的 P0/P1 问题是否属实 | **9/9 属实**（其中 2 项严重程度需修正表述，见 §3.1/§4.3） |
| 评审的数值声明是否正确 | **大部分正确**；两处需要更正（P0-DATA-2 上界数字、log 行为细节） |
| v5 修复方案是否正确可行 | **总体正确可行**；存在 4 处设计缺口需补丁（见 §6） |
| v5 方案是否引入新的数据风险 | 发现 1 个新的格式缺陷：**header 读取非版本安全**（§6.4，必须修复） |

**最终判定：评审问题真实、补救方向正确；采纳 v5 方案时必须叠加本报告 §6 的四项修正，否则 v5 自身会引入新的数据错误。**

---

## 2. P0 问题验证

### 2.1 P0-DATA-1 无 run 起始边界 —— 属实

**代码证据**（`main/radar_capture.c`）：

- L484 `radar_capture_start()` 只调用 `radar_capture_session_agg_reset(&s_session_agg)`，无 baseline；
- L526 `radar_capture_tick()` 直接把 `observation->ftm_raw_session_id` 送入聚合器，无 `id > baseline` 过滤。

**交叉证据**（`main/radar_ftm.c` L177–179）：成功路径在 `sys_evt` 上下文递增 `s_raw_session_id` 并写 median/bursts，与 Capture 状态机无任何联动。

**推演确认**：FTM 快照 `FTM_VALID_US = 5 s`，session 结束后 5 秒内任何时刻进入 Capture，第一个 tick（≤100 ms 后）即会把开始前完成的 session 加入本 run。COUNTDOWN 3 s + SETTLE 5 s 内完成的 session 也被无差别计入，而文档声称“start 快照当前 id，只聚合本 run 内新完成的 session”——文档与代码不一致。

**评审定性正确：会直接污染 P1 的 `b0_run` 样本。**

### 2.2 P0-DATA-2 聚合容量溢出 —— 属实，但上界数字需更正

**代码证据**：

- `radar_capture_session_agg.h` L14：容量 16；
- `radar_capture_session_agg.c` L30：`if (agg->count >= RADAR_CAPTURE_AGG_MAX_SESSIONS) return;` 静默丢弃；
- `radar_capture.c` L23–25：窗口 3+5+30 = 38 s；
- `radar_ftm.c` L22：`FTM_PERIOD_US = 2 s`。

**独立数值重算**：

- 最快循环：session 完成几乎即时，发起间隔仅受 2 s 周期限制 → 38 s 内最多 **19 个** session（此前文档写 19、补救设计写“约 19”均正确；评审报告原文表述“约 19”成立）；
- 失败循环（1.5 s timeout + 2 s backoff）反而更慢（10 个），**不构成更坏情况**；
- 混合场景（1 次 timeout + 其余成功）上界仍为 19。

**结论修正**：容量 16 确实可被 19 的上界突破，溢出风险属实；但需要精确表述为“19 > 16，且 38 s 窗口内 probe 间隔（5 s）不构成额外独立上界”。评审若使用了更大的估算数字（如 24/32 才够），属于过度保守而非错误——补救设计选 32 仍然安全，只是裕量理由应改用本报告的 19 + 余量。

### 2.3 P0-DATA-3 `target_m` 非实测真值 —— 属实

**代码证据**：

- `radar_capture.c` L866：`.measured_distance_m = -1.0f` 硬编码；
- 设备 UI 仅有 `radar_capture_targets_m[]` 预设档位；
- 仓库无任何 sidecar 生成/读取代码（grep 无结果）。

**评审定性正确：这是流程缺陷，不是编码 bug；补救方案（PC sidecar + report 拒绝缺失 run）是当前唯一可行解，除非未来给设备 UI 增加实测距离输入，但那会显著扩大本期范围。**

---

## 3. P1 问题验证

### 3.1 P1-DATA-4 raw 三元组非一致快照 —— 属实，但实际风险等级需降级

**代码证据**：

- 写端（`radar_ftm.c` L177–179，sys_evt 任务）：三条独立全局变量顺序写；
- 读端（`radar_capture.c` L526 经 `main.c` L327–329，LVGL 任务）：三次独立 getter 调用；
- 两者之间无临界区、无 sequence lock。

**严格性修正**：评审声称“可能出现 id=新、median=旧”。逐语句推演后该具体交错**在本代码顺序下不可复现**：写端顺序固定为 `id++ → median → bursts`，读端顺序为 `id → median → bursts`，唯一可能错位是“读到的 id 比 median/bursts 新一拍”。且 `radar_capture_tick` 以 id 去重，即使错位，下一次 tick 仍会以新 id 重读同一 median/bursts 值，自愈为正确数据。

**真实风险收窄为**：错位发生的那一个 tick 会把“旧 median + 新 id”记为一条 session，且若该 session 恰好是 run 内最后一个，median 值永远是旧值，无法自愈。概率极低但非零。

**评审方向正确，但严重度应从“可能系统性污染”降级为“极低概率、末端样本错误”；补救设计（coherent snapshot API + sequence lock）依然是正确解，且成本低。**

### 3.2 P1-DATA-5 probe/regular 混合 —— 属实

**代码证据**：`radar_ftm.c` L187–189 中 `if (s_probe_session)` 只影响 `s_probe_successes` 计数与成功率统计，raw session 记录对 probe 与 regular 一视同仁（id 递增、聚合、进入 header）。

**推演确认**：probe 8 帧 vs regular 16 帧，误差分布不同；`bursts` 只有总和，无法恢复每 session 分布。评审正确。

**补救方向正确**（P1 只用 regular），且本报告确认聚合器可低成本扩展 `kind` 字段。

### 3.3 P1-DATA-6 observed 字段最后一帧 —— 属实

**代码证据**：`radar_capture.c` L571–572 每次 tick 覆盖，L856–857 保存时只写最终值到 batch meta；L491–492 在 start 时重置。中间变化全部丢失。

**文档交叉验证**：设计文档把 `observed_level/observed_obstruction` 描述为 run 级设备观测，用于混淆矩阵。与代码不符。评审正确。

### 3.4 P1-DATA-7 batch export 固件版本丢失 —— 属实

**代码证据**：

- `radar_capture.c` L409：`strncpy(header.firmware_version, "slot2-batch", ...)` 覆盖真实版本；
- L870：`.firmware_hash32 = 0` 硬编码；
- batch capture header 本身有 `firmware_hash32` 字段（`radar_capture_batch.h` L236），但 meta 填 0 后透传也是 0。

**补充发现**：NVS save 路径（L775–779）**正确**写入了 `esp_app_get_description()->version`——所以此问题只影响 batch 导出路径，不影响 NVS fallback 单条导出。评审报告未区分这两条路径，应补充说明。

### 3.5 P1-DATA-8 PC 无法独立复核完整 CRC —— 属实

**代码证据**：`emit_csv()`（L321–335）输出的 META 只有 `payload_crc32` 和 `compressed_length/uncompressed_length`，没有 `record_crc32`；逐行 CSV 是解码后的明文，不是原始压缩字节。

**评审正确：设备内部 CRC 与 PC 独立复核是两层，当前导出只支持第二层的结构校验，不支持第三层的字节级复核。补救方案（canonical CRC + 可选 base64 payload 块）是正确分层。**

### 3.6 P1-DATA-9 `model_fit.py` 不存在 —— 属实

`tools/` 下无该文件，`validate.sh` 无对应测试。“验算方案目前只是设计”这一表述准确。

---

## 4. 评审数值/表述复核

### 4.1 需要更正的两处

1. **P0-DATA-2 上界**：应精确表述为 19（由 38 s ÷ 2 s 周期直接导出），不是“约 8–19 之间的不确定值”；评审报告的“约 19”结论成立，但推导过程应明确写出“最快完成循环受 2 s 发起周期限制”。
2. **P1-DATA-7 范围**：固件版本丢失只发生在 batch 导出路径；NVS 单条导出路径的 `firmware_version` 是真实的。评审未区分，应更正。

### 4.2 确认正确的数值

| 声明 | 重算结果 | 判定 |
| --- | --- | --- |
| worst-case capture span = 12288 B | align(112)+align(4096)+align(16) = 4096+4096+4096 | ✅ |
| 观测 payload 1466 B → span 12288 B | align(112)+align(1466)+align(16) = 4096+4096+4096 | ✅ |
| sequence lock 半序列空间 | uint32 半空间 = 2³¹ | ✅ |
| probe 在 38 s 内最多 ~7 个（5 s 间隔） | floor(38/5) = 7 | ✅ |
| 聚合容量 16→32 的内存代价 | 96→192 B 静态 | ✅ 可接受 |

---

## 5. v5 修复方案逐项验证

### 5.1 R-01 baseline + 回绕安全比较 —— 正确

`(int32_t)(a - baseline) > 0` 是标准的无符号序列比较惯用法，前提是增量不超过 2³¹-1。FTM session id 每次成功 session 才递增，38 s 内最多 19 次，即使连续运行数月也不可能在单次 Capture 期间跨越半序列空间。**前提天然满足，方案正确。**

**小缺口**：补救设计说“只在半个序列空间内判断”，但未写明“超出即 RAW_ID_AMBIGUOUS”的具体判定阈值代码形态。建议实现时直接用 `(uint32_t)(a - baseline) <= 0x7fffffffu` 作为合法窗口判断。

### 5.2 R-02 容量 32 + overflow 标志 —— 正确但裕量理由需更新

32 > 19（精确上界），安全。`OVERFLOW` 标志 + run SUSPECT + `ftm-report` 拒绝的组合是正确的“不静默丢数据”语义。

**修正要求**：设计文档中“约 8 个 session”的推导必须替换为本报告的 19 上界推导；32 的选择理由应写成“19 上界 + 安全余量”，而不是任意倍数。

### 5.3 R-03 PC sidecar 实测距离 —— 正确且必要

sidecar 以 `batch_id/run_id` 主键、缺失即 REJECTED 的门禁设计与“设备端不加入 UI 输入”的边界一致。YAML schema 字段完整。

**补充要求**：设计未规定 sidecar 与导出日志的时间对齐验证。建议 parser 增加：sidecar `measurement_session` 时间窗与导出 `boot_sequence` 时间窗的合理性检查（防错位标注）。

### 5.4 R-04 coherent snapshot + sequence lock —— 正确

sequence lock 是无锁单写多读的标准方案，写端在 sys_evt 任务（单一写者），读端在 LVGL 任务（多读者但只关心最新值）。比临界区更适合此场景（写路径极短，读路径 100 ms 一次）。

**实现细节确认**：ESP32-C3 上 `uint32_t` 对齐读写天然原子，sequence lock 只需 `__atomic` 或 `portENTER_CRITICAL` 保护 32 位 sequence 变量本身。方案可行。

### 5.5 R-05 probe/regular 分离 —— 正确

P1 只用 regular、probe 仅诊断的语义与“probe 有不同触发条件和帧数”一致。header 增加 `probe_session_count` 与 `aggregate_flags` 的改动在 112 B 内可行（见 §6.4 的空间核算）。

### 5.6 R-06 observed 聚合统计 —— 正确但存储位置需明确

first/last/mode/valid_samples/transitions 的完整集合无法全部塞进 112 B header（每个枚举最多 4 值 × 2 字段，mode/first/last 至少 6 字节，计数至少 4 字节，transitions 至少 2 字节——两个观测字段合计 ~20 字节）。

**补救设计已正确预判**：“header 保存 mode/first/last/valid_samples/flags；完整计数进 BATCH_CAPTURE_SUMMARY 行”。本报告确认该拆分可行，但需明确 header 内字段的具体二进制位置（见 §6.4）。

### 5.7 R-07 firmware provenance —— 正确

把真实 `esp_app_desc_t.version` 写入 batch capture header 并透传，改动只在 `radar_capture_save()`（batch meta 构造）和 `export_batch_record()`（去掉 "slot2-batch" 覆盖）。`firmware_hash32` 可在 save 时从 app desc 计算（ESP-IDF 提供 app image hash API）或构建期注入。可行。

### 5.8 R-08 canonical CRC —— 正确但需补充一个细节

canonical bytes 定义（META 行 + CSV header + 数据行，固定顺序/固定小数位/`NA` 缺失值）是稳定可重算的。

**补充要求**：设计未规定**浮点文本格式是否随固件 printf 实现变化**。ESP-IDF 的 `%.1f/%.3f` 在不同 newlib 配置下可能输出不同位数。建议 canonical 规则明确“以固件实际输出文本为 canonical 字节，PC 不做任何重格式化”，把格式稳定性锚定在固件侧，而不是假设跨平台 printf 一致。

### 5.9 R-09 parse/report 先行 —— 正确

最小闭环（parse → report → ftm-report）与“先验算后拟合”的原则一致。验收用已有 3 次非正式 Capture 日志即可，无需新采集。

### 5.10 R-10 独立 sweep evidence —— 正确

独立 `sweep_id` + 不按时间相邻拼接的规则，正确避免了把无关 sweep 强行关联到 Capture run 的数据污染。

### 5.11 R-11 数字统一 —— 正确

以代码常量和验算测试为唯一来源的原则正确；本报告 §4 提供了应替换进文档的精确数字。

---

## 6. v5 方案的设计缺口（必须补丁）

### 6.1 NVS v2 header 的向前兼容陷阱

补救设计说“NVS fallback 头可从 88 B 增长到 96 B 或更高”。**这里有一个评审和补救设计都未指出的风险**：

- `valid_header()`（L214）严格 `== CAPTURE_STORAGE_VERSION`；
- v65 固件读到 v3 blob 时拒绝（invalid_header），这是设计内行为；
- 但如果 v5 把 NVS 头再改版（v2→v3），**旧 v2 blob 会被静默拒绝并覆盖**。在“NVS 只是 fallback、slot2 才是正式存储”的前提下这是可接受的，但文档必须显式声明“NVS v2→v3 升级会丢弃 fallback 最新一条”，否则用户会误以为数据还在。

### 6.2 `header_size` 校验的不对称风险

v5 如果向 reserved 区加字段但保持 112 B，`capture_header_valid` 检查 `header_size == 112` 没问题。但如果未来 v6 扩到 120 B，`scan_committed_records()` 与 `radar_capture_batch_export()` 用 `sizeof(capture)` 读取——固件编译期的 struct 大小决定读多长。**旧固件读新记录会截断，新固件读旧记录会读到脏数据**（因为 payload 紧跟 header 之后，多读 8 B 会把 payload 前 8 B 当 header）。

补救设计没有提出对策。**本报告要求 v5 实现中加入**：

- `scan/export` 循环内先读 8 B（magic+version+header_size），按 `header_size` 二段式读取；
- 或在 header 中增加 payload 起始偏移字段，彻底解耦 `sizeof(struct)` 与盘上布局。

### 6.3 flags 字段的 CRC 保护不完整问题

补救设计的 `aggregate_flags` 若放进 batch capture header reserved 区，会自然被 `record_crc32` 覆盖——前提是 CRC 计算包含 reserved 区。**当前实现**：`radar_capture_batch_record_crc32()` 对 112 B 全量 CRC，reserved 区已包含。确认无缺口，但设计文档应显式写明“reserved 区新字段自动受 record_crc32 保护”，避免未来实现者误以为需要单独 CRC。

### 6.4 112 B header 剩余空间核算

v5 需要新增：

| 字段 | 大小 |
| --- | --- |
| regular_raw_median_m | 4 B（可复用 ftm_raw_median_m 位置） |
| regular_raw_iqr_m | 4 B（可复用） |
| regular_session_count | 2 B（可复用 ftm_raw_sessions） |
| regular_burst_total | 2 B（可复用 ftm_raw_bursts） |
| probe_session_count | 2 B（新增） |
| aggregate_flags | 2 B（新增） |
| observed_level_mode + observed_obstruction_mode | 2 B（新增） |
| observed_valid_samples | 2 B（新增） |
| （可选）observed_transitions | 2 B（新增） |

新增合计 10–12 B。当前 reserved[28]，v5 后剩 16–18 B。**空间足够**，但设计文档必须给出精确字段布局表，否则实现者会各自猜测导致偏差。

### 6.5 新发现：v5 设计未覆盖的 header 读取版本安全

这是本报告在验证过程中发现的**新问题**，评审与补救设计均未覆盖：

当前 `scan_committed_records()` 与 `radar_capture_batch_export()` 都用 `read_bytes(offset, &capture, sizeof(capture))` 一次读出 112 B 再校验。如果盘上记录是 v3（112 B）而固件是未来 v6（120 B struct），`sizeof` 变了，读取长度就错了。

在 v5 改动中，这不仅是未来风险：**v5 若保持 112 B 则当前无害**，但设计必须同时规定“读路径必须按 header_size 分段读取”，否则 v5 的兼容声明（“v3/v4 可读”）在代码层面其实依赖“struct 不再增长”这个未声明的前提。

**要求 v5 补充**：读取路径按 `version + header_size` 二段式读取，`header_size > sizeof(struct)` 时拒绝，`header_size < sizeof(struct)` 时只读前缀并按版本解释。

---

## 7. 结论

1. **评审 9 项问题全部属实**；2 项（P0-DATA-2 上界推导、P1-DATA-7 影响范围）需要修正表述，1 项（P1-DATA-4）需要降级严重度。
2. **补救设计方向正确**：baseline、overflow、sidecar、coherent snapshot、regular-only、observed 聚合、provenance、canonical CRC、parse/report 先行，全部是针对真实问题的合理方案。
3. **v5 方案必须叠加本报告 §6 的修正后才能实施**：
   - §6.1 NVS v2→v3 升级丢数据的显式声明；
   - §6.2 header_size 二段式读取；
   - §6.3 reserved 区 CRC 覆盖的显式文档化；
   - §6.4 字段布局表；
   - §6.5 读路径版本安全（与 §6.2 同一改动）。
4. **v65 固件本身不需要回滚**；当前数据可继续用于存储链路测试，不可用于正式拟合——与评审的“条件通过”判定一致。

**最终判定：评审声称的问题属实、补救设计总体正确；采纳 v5 时必须按 §6 打四个补丁，其中 §6.2/§6.5（header 读取版本安全）是本验证新发现、必须在 v5 实现中一并落地的硬性要求。**
