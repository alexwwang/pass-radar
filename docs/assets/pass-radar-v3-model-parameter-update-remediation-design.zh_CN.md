# pass-radar v3 数据验算问题补充修复设计

> English peer: `pass-radar-v3-model-parameter-update-remediation-design.md`
> 日期：2026-09-23  
> 基线实现：`db55cc9`，FORMAT_VERSION 4 run 级 FTM 原始 session 聚合  
> 上游评审：`pass-radar-v3-model-parameter-update-review.zh_CN.md`  
> 文档性质：补充设计；本文件与英文文件平等，无主次关系。

---

## ELI5 通俗摘要（先读这段）

上一轮检查发现：存下来的数据有的地方**说不清来历**（哪次采集、离多远、哪台固件），有的地方会**悄悄丢数据**（装满时不停止），还有的标签是**事后一帧算的、不代表全程**。

打个比方：你要给尺子重新对表，结果尺子上的刻度纸条有的没写日期和地点，有的中间被撕掉几条没告诉任何人，还有一张写着"全程无遮挡"的纸条其实只量了最后一秒。这样的数据拿来对表，对出来的尺子是歪的。

**这份设计就是给数据立规矩**：每条数据必须能回答"**哪个固件、哪一批、哪一次、离多远（真值：整数档 paced 或尺量）、什么条件、哪个完整时间窗**"；任何不确定就标 `SUSPECT`/`REJECTED`（可疑/作废），**宁可不用，不许乱用**。规矩齐了以后的新格式叫 **FORMAT_VERSION 5**；旧数据（v3/v4）还能看、能导出，但**不许混进正式对表的数据堆**。

关键名词对照：`run`=一次 38 秒采集；`batch`=一整轮活动的所有 run；`sidecar`=随数据一起交的"实测距离小票"；`OVERFLOW`=装满后还在往里塞导致静默丢数据的危险状态。完整细节从 §2 起。

---

## 1. 设计结论

上一轮评审确认：FORMAT_VERSION 4 已经解决了部分 Flash、CRC 和 raw FTM 保存问题，但仍存在数据归属、容量溢出、实测距离、快照一致性、probe 语义、观测标签、固件 provenance 和 PC 验算工具缺口。

本补充方案的最终目标是：

> **任何进入正式模型拟合的数据，都必须能够回答“它来自哪个固件、哪个 batch、哪个 run、哪个真实距离、哪个条件、哪个完整采样窗口”，并且发现不确定性时标记为 SUSPECT/REJECTED，而不是静默使用。**

修复后的正式数据格式定义为 **FORMAT_VERSION 5**。设计原则：

- v3/v4 数据仍可读取、导出和人工检查；
- v3/v4 数据不得自动混入 FORMAT_VERSION 5 的正式拟合数据集；
- 关键错误不能通过“尽量拟合”掩盖；
- 存储完整性、导出完整性、统计语义和模型准入分别验收；
- 先实现 `parse/report` 数据验算，再开始参数拟合。

---

## 2. 修复范围和优先级

| 编号 | 问题 | 优先级 | 修复结果 |
| --- | --- | --- | --- |
| R-01 | raw FTM 没有 run 起始边界 | P0 | baseline + 回绕安全比较 |
| R-02 | 聚合容量可能静默溢出 | P0 | 容量上界证明 + overflow 标志，溢出 run 禁止拟合 |
| R-03 | `target_m` 不是实测真值 | P0 | 真值分级：拟合级 = 整数档 paced 摆位纪律（`truth=paced`）；验证级（D6 对外验证）= PC 实测 sidecar 强制门禁 |
| R-04 | raw FTM 三元组不是一致快照 | P1 | 一次性结构体 API + sequence lock/临界区 |
| R-05 | probe 与 regular 混合 | P1 | P1 只使用 regular；probe 单独计数，不进入 offset 估计 |
| R-06 | observed 字段只是最后一帧 | P1 | 改为 run 聚合统计，不再伪装成整段真值 |
| R-07 | batch export 丢失真实固件版本 | P1 | 保存并透传 firmware version/hash |
| R-08 | PC 无法独立复核完整 CRC | P1 | 导出 canonical CRC 与可选原始 payload 块 |
| R-09 | `model_fit.py` 不存在 | P1 | 先实现 parse/report/ftm-report，再实现拟合 |
| R-10 | 方向/扫盘数据无 run 关联 | P1 | 独立 sweep evidence 格式，不把缺失数据当已存在 |
| R-11 | 文档容量和时序数字不一致 | P2 | 所有数字以代码常量和验算测试为唯一来源 |

---

## 3. FORMAT_VERSION 5 数据模型

### 3.1 Capture 的身份和 provenance

每个 Capture 必须形成以下唯一键：

```text
batch_id
run_id
capture_id
format_version
condition_schema_version
firmware_version
firmware_hash32
boot_sequence
reset_reason
```

`run_id` 只在一个 batch 内递增，不能因为重启后重新打开设备而重复。已有 batch 的下一 run 必须从已提交记录数推导，不能从 RAM 中的 `s_capture_id` 推导。

`firmware_version` 保存 `esp_app_desc_t.version`；`firmware_hash32` 保存构建产物或 app image 的稳定 hash。导出时必须原样透传，禁止使用 `slot2-batch` 替代真实版本。

### 3.2 FTM raw session 记录语义

对外提供一个整体快照，而不是三个独立 getter：

```c
typedef enum {
    RADAR_FTM_SESSION_REGULAR = 0,
    RADAR_FTM_SESSION_PROBE = 1,
} radar_ftm_session_kind_t;

typedef struct {
    uint32_t sequence;
    uint32_t session_id;
    float raw_median_m;
    uint8_t valid_bursts;
    uint8_t kind;
    bool valid;
} radar_ftm_raw_session_snapshot_t;
```

FTM 回调使用 sequence lock 或同等临界区：

```text
write sequence = odd
write complete snapshot
write sequence = even
```

读取者只有在两次 sequence 相同且为偶数时才接受快照；否则本次 tick 不追加。

### 3.3 run 边界

Capture 开始时保存：

```text
capture_start_raw_session_id
capture_start_monotonic_ms
```

采用无符号序列号的回绕安全比较：

```text
is_new(a, baseline) = (int32_t)(a - baseline) > 0
```

session id 回绕时只允许在不超过半个序列空间的正常窗口内判断。若无法判定，run 设置 `RAW_ID_AMBIGUOUS`，禁止 P1 拟合。

阶段策略固定为：

| Capture 阶段 | FTM raw session 是否计入 P1 |
| --- | --- |
| COUNTDOWN 3 s | 不计入；只等待用户和设备稳定 |
| SETTLE 5 s | 计入，但写入 `settle_sessions`，不与正式 running 统计混淆 |
| RUNNING 30 s | 计入 P1 |

实现上，session 快照必须携带完成时间或 capture 阶段；没有阶段信息的 v4 数据不能升级为 v5 语义。

### 3.4 聚合容量与 overflow

容量上界由代码常量直接推导（静态验证报告 §2.2 已复核）：

```text
capture_window = countdown 3 s + settle 5 s + running 30 s = 38 s
initiation gate = FTM_PERIOD_US = 2 s   （成功路径的主导约束）
worst-case completed sessions = floor(38 / 2) = 19
（timeout 1.5 s + backoff 2 s = 3.5 s/次的失败循环更慢，约 10 次，
  不构成更坏情况；probe 间隔 5 s 亦不构成额外独立上界）
```

第一版容量取 32（= 19 上界 + 安全余量），并保留明确的溢出字段：

```c
#define RADAR_CAPTURE_AGG_MAX_SESSIONS 32u
#define RADAR_CAPTURE_AGG_FLAG_OVERFLOW       (1u << 0)
#define RADAR_CAPTURE_AGG_FLAG_ID_AMBIGUOUS   (1u << 1)
#define RADAR_CAPTURE_AGG_FLAG_SNAPSHOT_LOSS  (1u << 2)
#define RADAR_CAPTURE_AGG_FLAG_NO_REGULAR     (1u << 3)
```

容量达到上限时不得静默丢弃：

1. 置 `OVERFLOW`；
2. 继续记录已经得到的摘要，但把 run 状态标记为 `SUSPECT`；
3. 导出时输出 flags；
4. `ftm-report` 默认拒绝该 run 的 P1 估计。

### 3.5 Probe 和 regular 的统计规则

P1 `ftm_offset` 只使用 regular session：

```text
regular_raw_medians -> median/IQR -> b0_run
probe sessions -> diagnostic counts only
```

原因：probe 使用不同帧数和触发条件，不能与 regular session 假设为同一误差分布。

v5 header 至少保存：

```text
regular_raw_median_m
regular_raw_iqr_m
regular_session_count
regular_burst_total
probe_session_count
aggregate_flags
```

旧字段 `ftm_raw_median_m` 在 v5 中不再含糊使用，建议改为 `ftm_regular_raw_median_m`；如必须保持 112 B 结构，则在代码中保留二进制位置，但通过 FORMAT_VERSION 5 和字段注释重新定义，并在 CSV 使用新字段名。

不再声称“按 burst 加权”。第一版 P1 估计是 session-level median：

```text
b0_run = regular_raw_median_m - measured_distance_m
ftm_offset = median(b0_run across accepted runs)
```

`regular_burst_total` 仅用于质量报告和最小 burst 门限，不参与隐含加权。

### 3.6 observed 状态聚合

当前最后一帧字段改为明确的 run 统计：

```text
observed_level_first
observed_level_last
observed_level_mode
observed_level_valid_samples
observed_level_transitions
observed_obstruction_first
observed_obstruction_last
observed_obstruction_mode
observed_obstruction_valid_samples
observed_obstruction_transitions
```

如果 header 空间不足，不把所有计数硬塞进 Capture header。优先策略是：

- batch header 保存 mode、first、last、有效样本数和 flags；
- 完整状态计数作为 `BATCH_CAPTURE_SUMMARY` 导出行；
- 模型工具以逐行可重算特征为主，header 观测值只作质量辅助，不能当物理真值。

字段命名禁止继续使用没有语义限定的 `observed_level`。v5 字段命名对照：

| v4 旧字段 | v5 语义 |
| --- | --- |
| `observed_level` | 拆为 `observed_level_mode/first/last`，另有计数进 SUMMARY 行 |
| `observed_obstruction` | 拆为 `observed_obstruction_mode/first/last`，同上 |
| `ftm_raw_median_m` | 改名 `ftm_regular_raw_median_m`（仅 regular session） |
| `ftm_raw_iqr_m` | 改名 `ftm_regular_raw_iqr_m` |
| `ftm_raw_sessions` | 改名 `ftm_regular_session_count` |
| `ftm_raw_bursts` | 改名 `ftm_regular_burst_total`，仅作质量指标，不作权重 |

---

## 4. 实测距离和条件标签

### 4.1 target 与 measured 的严格分离

设备菜单中的距离仍然是计划标签：

```text
target_distance_label_m
```

它不是自动测得的真实距离。

真值分两级（2026-09-24 按实际采集条件修订：户外采集以步幅估距、无卷尺条件）：

- **拟合级（日常）**：`measured_distance_m < 0`（缺省）时，真值 = Capture 页选定的整数距离档 `target_m`，前提是执行**摆位纪律**——步幅摆到位才 START，摆不出该距离就重摆或改档，不允许「摆哪算哪」；此类 run 标 `truth=paced`（精度约 ±3~8%）。P7/P8 分类拟合不依赖距离真值，P7 的误差评估因 paced 噪声略偏保守（安全方向）；
- **验证级（D6 对外验证前）**：受控实测（tape/laser）值，经 `measured_distance_m` 字段或 PC sidecar 提交。**证明 80% 的验收结论必须使用验证级真值**——paced 真值的 ±5% 量级误差会直接污染 20% 判据的分子分母。

`measured_distance_m` 字段语义：≥0 = 实测覆盖值（验证级）；−1.0 = 「真值=所选整数档」（`truth=paced`），固件当前恒 −1.0 即此行为，无需改动。

提交验证级实测值时使用 PC sidecar：

```yaml
batch_id: 493512032
measurement_session: outdoor-2026-09-23-a
operator: alex
runs:
  - run_id: 1
    measured_distance_m: 1.02
    method: laser
    uncertainty_m: 0.02
  - run_id: 2
    measured_distance_m: 3.04
    method: tape
    uncertainty_m: 0.03
```

约束：

- `batch_id/run_id` 必须唯一匹配；
- `measured_distance_m > 0` 且在合理范围内；
- 方法必须为 `tape`、`laser` 或受控测量方法；
- uncertainty 缺失时可报告但不能用于高精度准入；
- sidecar 中距离与 Capture 标签差异超过预设阈值时标记 `DISTANCE_LABEL_MISMATCH`；
- 没有 sidecar 的 run 以 `truth=paced` 参与拟合；`verify`（D6 验收）默认将其标记 `REJECTED_NO_MEASURED_DISTANCE` 并排除出验证集。
- parser 必须做时间对齐校验：sidecar `measurement_session` 的时间窗必须与该 run 所属固件运行窗重叠；不重叠的条目标记 `DISTANCE_SIDECAR_MISALIGNED` 并拒绝，防止事后补录时错位标注。注意 `boot_sequence` 只提供运行窗的**序**，不提供墙钟时间：时间锚点取该 run 导出时的 USB 监控日志时间戳（或导出操作记录），`boot_sequence` 用于验证同批次内 run 的先后与连续性；时间锚必须是数字日志时间戳，墙钟字符串/散文锚无法与开机毫秒时间轴比对，解析器必须标 `DISTANCE_SIDECAR_UNVERIFIABLE_ANCHOR` 并拒绝（不得静默跳过对齐校验）；
- sidecar 条目必须包含 `capture`（导出）时间或引用导出会话；缺失时间锚的条目标记 `DISTANCE_SIDECAR_NO_TIME_ANCHOR` 并拒绝。

### 4.2 条件字段的验算

PC parser 必须验证：

```text
scene_id == encode(environment, link)
propagation == propagation(link)
所有 context 值在 schema 允许域内
room_size 与 space 的适用关系一致
```

合法但可能有歧义的组合只标记 WARNING；硬冲突标记 REJECTED。设备端和 PC 端必须共享同一套 schema version。

---

## 5. provenance、CRC 和导出协议

### 5.1 设备内部校验和 PC 外部校验分层

必须分开描述三类校验：

1. **设备 Flash 校验**：读取 payload、验证 payload CRC、record CRC、commit marker；
2. **导出结构校验**：PC 检查 batch/run 顺序、记录数、字段域、日志是否截断；
3. **PC 原始数据校验**：PC 重新计算 canonical CRC 或从原始 payload 字节重算 CRC。

### 5.2 推荐导出行

META 行至少增加：

```text
format_version
condition_schema_version
firmware
firmware_hash32
batch_id
run_id
capture_id
record_count
payload_length
payload_crc32
record_crc32
ftm_aggregate_flags
```

### 5.3 canonical CSV CRC

为了不强制第一版立刻增加 binary export，先定义稳定的 canonical CRC：

```text
canonical_bytes =
  canonical META key=value 行
  + '\n'
  + CSV header
  + '\n'
  + 每条 CSV 数据行的规范化 UTF-8 文本
  + '\n'
```

规范化规则：

- key 按固定顺序；
- 浮点使用固定小数位；
- 缺失值统一为 `NA`；
- 行尾统一 `\n`；
- 不包含日志时间戳、ANSI 控制字符和 ESP_LOG 前缀。
- **浮点文本格式的权威源是固件输出本身**：canonical 字节就是设备打印的原文，PC 端不做任何重格式化（不改小数位、不补零、不换科学计数法）。ESP-IDF 不同 newlib 配置下 `%.1f/%.3f` 的输出可能不同，因此格式稳定性锚定在固件侧：同一固件版本输出即稳定；跨固件版本比较时，parser 以 META 行里的 `firmware_version` 分组，各自使用本组实际输出文本重算 canonical CRC。

设备输出 `canonical_crc32`，PC parser 按相同规则重算。后续如需要验证压缩 payload，再增加：

```text
BATCH_CAPTURE_PAYLOAD chunk_index=... total=... base64=...
```

在此之前，不得把 payload CRC 字段当成 PC 已完成独立复算的证据。

### 5.4 header 读取版本安全（二段式读取）

当前 `scan_committed_records()` 与 `radar_capture_batch_export()` 均以编译期 `sizeof(header)` 一次性读取记录头。这只在 struct 尺寸从此不再变化的前提下才安全；v5 落地时必须改为二段式读取，把盘上布局与编译期 struct 解耦：

```text
第一段：读前 8 字节（magic + version + header_size）
第二段：按 header_size 读剩余部分
  严格式：header_size != sizeof(struct) -> 拒绝该记录
  宽容式：header_size >  sizeof(struct) -> 拒绝（未来布局）
          header_size <  sizeof(struct) -> 只读声明前缀，按 version 解释，
                                          声明尺寸之外的 v4+ 字段视为缺失/零
```

宽容式存在的理由：未来 v6 若只在 reserved 区追加字段，旧固件仍可读；任何结构重排必须升 version 并使用严格式。实现必须声明采用哪种严格度，不允许未声明的混合行为。

适用位置：`scan_committed_records()`、`radar_capture_batch_export()`，以及 NVS fallback 的 `read_key_header()/read_key()`。该项是静态验证报告 §6.2/§6.5 提出的硬性要求，随 v5 一并实现。

---

## 6. PC 验算程序实施方案

### 6.1 第一阶段只实现 parse/report

新增：

```text
tools/model_fit.py
tools/model_fit/parser.py
tools/model_fit/clean.py
tools/model_fit/report.py
tools/model_fit/sidecar.py
```

命令：

```bash
python3 tools/model_fit.py parse monitor.log -o dataset/
python3 tools/model_fit.py report dataset/ --distance-sidecar measured.yaml
python3 tools/model_fit.py ftm-report dataset/ --distance-sidecar measured.yaml
```

`report` 输出每个 run：

```text
PASS
SUSPECT
REJECTED
```

并列出：

- batch/run/capture identity；
- format/schema/firmware；
- records 实际数和 header 数；
- CSV canonical CRC；
- raw FTM regular/probe 数；
- overflow、snapshot loss、id ambiguity；
- measured distance 是否存在；
- 标签合法性；
- 日志截断和重复导出检测。

### 6.2 第二阶段实现 P1/P2 复算

只有 `PASS` run 才能进入：

```text
regular_raw_median_m - measured_distance_m
```

P1 报告必须同时给出：

```text
accepted_run_count
rejected_run_count
median
MAD
per-distance residual
firmware groups
```

如果不同距离的 b0 出现系统漂移，报告异常，不自动生成新 offset。

### 6.3 第三阶段再实现 P3–P10

RSSI、分类、Kalman、方向和 sweep 拟合都必须以 `report PASS` 数据为输入。没有真实方向标签、扫盘样本或 C/Python 对拍结果的参数，不生成对应参数包字段。

---

## 7. 方向、扫盘和遮挡的单独数据协议

普通 Capture CSV 不承担完整方向模型输入。方向证据单独定义：

```text
sweep_id
batch_id
run_id 或独立 test_id
condition schema
true_bearing_deg
measured_distance_m
sweep_duration_ms
sample_count
sample timestamp/channel/rssi
result_angle_deg
spread_deg
confidence
sweep_crc32
```

若扫盘和 Capture 不是同一个物理 run，必须使用独立 `sweep_id`，不能仅凭时间相邻关系拼接。

遮挡分类第一版只使用：

- 物理条件标签；
- Capture 中可重放的 RSSI/FTM 时序；
- 已通过 C/Python 对拍的特征。

未完成对拍的特征只允许出现在报告的“未验收”区，不得进入参数搜索。

---

## 8. 兼容策略

| 数据版本 | 允许操作 | 正式拟合 |
| --- | --- | --- |
| v3 | 读取、导出、CRC 检查、人工审阅 | 禁止自动进入 |
| v4 | 读取、导出、非正式链路验证 | 只有经过人工补充且明确标记，默认禁止 |
| v5 | 完整 parse/report、P1/P2 复算 | 满足所有门禁后允许 |

v3/v4 读取时不能被新固件启动逻辑静默擦除。v5 新 batch 不覆盖旧 batch，除非用户显式执行 NEW BATCH。

如果 v5 header 继续保持 112 B：

- 新增字段必须占用 reserved 区；
- header CRC 覆盖完整 112 B（reserved 区新字段自动受 `record_crc32` 保护，无需单独 CRC，也不得移出覆盖范围）；
- `header_size` 和 `version` 必须严格校验；
- 读取路径必须按 §5.4 的二段式方式读取，不得依赖编译期 `sizeof(struct)`；
- v4 的 raw 字段不得按照 v5 regular-only 语义解释。

#### 8.1 v5 capture header 字段布局表（112 B，静态验证报告 §6.4 已用编译期 offsetof 审计）

v4 现状：字段区 0–83，`reserved[28]` 位于偏移 84–111。

| 偏移 | 字段 | 大小 | v5 变化 |
| --- | --- | --- | --- |
| 0–3 | magic | 4 | 不变 |
| 4–5 | version | 2 | 值 4→5 |
| 6–7 | header_size | 2 | 不变（112） |
| 8–19 | batch_id / run_id / capture_id | 12 | 不变 |
| 20–35 | scene/条件字段（含 `observed_level`/`observed_obstruction` 字节） | 16 | 值域不变；v5 把 `observed_level`/`observed_obstruction` 字节重定义为 `*_mode`（v4 语义为 last-sample，按 version 字段区分） |
| 36–43 | target_distance_m / measured_distance_m | 8 | 不变 |
| 44–71 | record_count / payload_length / payload_crc32 / firmware_hash32 / boot_sequence / reset_reason / record_crc32 | 28 | 不变 |
| 72–75 | ftm_raw_median_m | 4 | 语义重定义为 regular-only，命名 `ftm_regular_raw_median_m` |
| 76–79 | ftm_raw_iqr_m | 4 | 同上，`ftm_regular_raw_iqr_m` |
| 80–81 | ftm_raw_sessions | 2 | 同上，`ftm_regular_session_count` |
| 82–83 | ftm_raw_bursts | 2 | 同上，`ftm_regular_burst_total` |
| 84–85 | probe_session_count | 2 | 新增（原 reserved） |
| 86–87 | aggregate_flags | 2 | 新增（原 reserved） |
| 88–89 | observed_first_last（level first/last 高 4+4 bit，obstruction first/last 低 4+4 bit） | 2 | 新增（原 reserved） |
| 90–91 | observed_valid_samples | 2 | 新增（原 reserved） |
| 92–111 | reserved | 20 | 剩余 20 B（原 28 B − 新增 8 B） |

> 新增字段合计 8 B；mode 不另占新字段——直接重定义条件区既有字节，避免同一语义出现两处存储。`observed_level_transitions`/`observed_obstruction_transitions` 按 §3.6 进入 `BATCH_CAPTURE_SUMMARY` 行，不进 header。

布局约束：新增字段全部落在原 reserved 区，v3/v4 记录在这些偏移上为全零；`probe_session_count=0 && aggregate_flags=0 && observed_first_last=0 && observed_valid_samples=0` 的组合在 v4 记录中合法，在 v5 校验中对应“无 probe、无异常 flag、无观测聚合”，语义自洽。条件区 `observed_level/observed_obstruction` 字节在 v4 记录中是 last-sample、在 v5 中是 mode，**读取端必须按 version 字段选择解释，不得跨版本混读**。`offsetof` 静态断言随实现提交，防止未来字段漂移。

NVS fallback 从 88 B 增长到 96 B 或更高，但必须重新计算：

```text
records allocation
scratch allocation
safety margin
largest-free-block gate
```

不能为了保持旧数字而截断字段。

#### 8.2 NVS fallback 版本升级的数据丢失披露

NVS fallback 采用严格 `version == CAPTURE_STORAGE_VERSION` 校验。v5 将 NVS 头从 v2 升到 v3 时，**旧 v2 blob 会被 invalid_header 拒绝，并在下次 save 时被覆盖**。由于 NVS 仅是 slot2 不可用时的 fallback，这是可接受行为，但必须在此显式披露：

- 固件升级到 v5 后，NVS fallback 中尚未导出的最后一条 v2 记录不可恢复；
- 操作规程相应要求：升级固件前先完成一次完整导出；
- slot2 批次数据不受影响（batch header 有独立版本与 v3/v4 兼容读路径）。

---

## 9. 测试计划

### 9.1 host 单测

必须增加：

1. baseline 前后 session 过滤；
2. session id 回绕和歧义拒绝；
3. 32 session 容量和 overflow；
4. snapshot sequence lock 的不一致读取拒绝；
5. regular/probe 分离；
6. COUNTDOWN/SETTLE/RUNNING 阶段计数；
7. observed first/last/mode/transitions；
8. firmware/hash/CRC 字段透传；
9. canonical CSV CRC 稳定性；
10. sidecar 缺失、重复、距离不一致和非法值；
11. v3/v4 读取不被擦除，v5 新字段校验；
12. 截断日志、重复导出、乱序 run 的 report 状态。

### 9.2 设备测试

非正式测试顺序：

1. 一次无 FTM 的 Capture，确认 `NO_REGULAR` 而不是伪造 raw 数据；
2. 一次 regular FTM Capture，确认 baseline 不吸收开始前 session；
3. probe/regular 混合测试，确认 P1 只使用 regular；
4. 至少一次导出并由 PC 重算 canonical CRC；
5. 重启后继续同一 batch，确认 run_id 不重复；
6. 故意制造日志截断，report 必须为 SUSPECT/REJECTED；
7. 只在上述测试全部通过后，开始正式采集。

### 9.3 放行门禁

`validate.sh --static` 必须新增：

- v5 聚合/格式 host tests；
- parser/report fixture tests；
- canonical CRC fixture tests；
- C/Python 特征对拍测试。

正式固件编译和 simulator boot 只能作为后续门禁，不能替代数据语义测试。

---

## 10. 实施顺序与不应做的事

实施顺序：

1. 先实现 FTM coherent snapshot、baseline 和 overflow；
2. 再更新 v5 header/NVS/batch/export；
3. 再实现 firmware provenance、canonical CRC 和 sidecar report；
4. 再实现 observed 聚合；
5. 再实现 parse/report/ftm-report；
6. 通过非正式日志验收；
7. 最后实现模型拟合和参数包。

在此之前不应：

- 先扩展拟合公式再修数据语义；
- 用 `target_m` 替代 measured distance；
- 用最后一个 observed 值代表整段 run；
- 用 probe session 伪装 regular session；
- 把设备内部 CRC 检查说成 PC 独立验算；
- 把 v4 数据无条件升级为 v5 正式数据。

---

## 11. 最终验收标准

补充方案完成后，正式采集必须满足：

```text
每个 run：
  identity 完整
  firmware provenance 完整
  measured distance 完整
  condition schema 合法
  record count 对账通过
  canonical CRC 通过
  raw FTM snapshot 无歧义
  aggregate 无 overflow
  regular/probe 语义明确
  log 未截断
```

只有 `PASS` run 可以进入模型拟合；`SUSPECT` 只能进入方差/故障分析；`REJECTED` 只能保留为审计证据。

**本设计的放行结论：先完成 v5 数据语义和 parse/report 验收，再开始正式的定向、测距模型参数采集。**
