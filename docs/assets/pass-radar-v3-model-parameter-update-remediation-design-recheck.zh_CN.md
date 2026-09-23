# 4 项修正更新后对补救设计文档的复审报告

> English peer: `pass-radar-v3-model-parameter-update-remediation-design-recheck.md`
> 日期 / Date: 2026-09-23
> 审查对象 / Reviewed: `pass-radar-v3-model-parameter-update-remediation-design.zh_CN.md`（含 4 项修正后版本）与英文对等文档
> 方法 / Method: 以 v65 固件代码（`db55cc9`）为事实基线，逐项验证修正内容的正确性、文档间一致性及新引入的缺口；未修改业务代码。

## 1. 复审结论

**4 项修正全部正确落盘且与代码事实一致；复审中另发现并当场修正了 3 处新问题（见 §3）。两份语言文档现在内容对等。判定：补救设计文档可以进入 v5 实施。**

| # | 修正项 | 位置（中/英） | 复审判定 |
| --- | --- | --- | --- |
| F1 | 容量上界改为代码常量推导（38 s ÷ 2 s = 19，容量 32） | §3.4 / §3.4 | ✅ 正确，算术复核通过 |
| F2 | sidecar 时间对齐校验（`DISTANCE_SIDECAR_MISALIGNED`） | §4.1 / §4.1 | ✅ 正确；复审补强了时间锚点定义 |
| F3 | canonical CRC 浮点文本格式权威源 = 固件输出 | §5.3 / §5.3 | ✅ 正确，与 ESP-IDF newlib 实况相符 |
| F4 | header 二段式读取（§5.4）+ v5 布局表（§8.1）+ NVS 披露（§8.2） | §5.4/§8.1/§8.2 | ✅ 正确；布局表已用 offsetof 审计数字 |

## 2. 逐项验证依据（代码事实）

### F1 容量推导

代码常量（`main/radar_ftm.c`）：

```text
FTM_PERIOD_US          = 2 s    (L22)   成功路径发起门控
FTM_SESSION_TIMEOUT_US = 1.5 s  (L23)
FTM_BACKOFF_US         = 2 s    (L25)
FTM_PROBE_PERIOD_US    = 5 s    (L32)
FTM_PROBE_FRAMES       = 8      (L35)
```

独立复算：

- capture 窗口 = countdown 3 s + settle 5 s + running 30 s = 38 s；
- 成功路径两次 session 完成间隔 ≥ 2 s（`FTM_PERIOD_US`，L343 同时检查 backoff），最坏完成 session 数 = floor(38/2) = **19**；
- 失败路径单次 = timeout 1.5 s + backoff 2 s = 3.5 s，38 s 内约 10 次，更慢，不构成更坏情况——文档表述正确；
- probe 每 5 s 一次且不并行计数额外 session，不构成独立上界；
- 容量 32 = 19 + 安全余量（约 1.68×），`RADAR_CAPTURE_AGG_MAX_SESSIONS 32u` 内存开销 32×6 B = 192 B，在 §3.4 声明的预算内。

结论：F1 的算术、常量引用和"约 8 个 session 不构成容量证明"的否定均正确。上游文档 §10.3 的旧声明已同步替换为指向本结论的表述（含 v4 容量 16 不满足 19 上界的缺陷披露）。

### F2 sidecar 时间对齐

- 原修正只要求时间窗重叠，但未定义时间锚从何而来；
- 代码事实：capture header 的 `boot_sequence` 来自 `radar_diag_current_sequence()`（`radar_capture.c` L767），是**运行序号，不含墙钟**；
- 复审补强（已写入两份文档）：时间锚 = 该 run 导出时的 USB 监控日志时间戳或导出会话记录；`boot_sequence` 仅用于同批次内 run 的先后/连续性验证；缺时间锚的条目标记 `DISTANCE_SIDECAR_NO_TIME_ANCHOR` 拒绝。

结论：F2 方向正确，补强后语义闭合。

### F3 浮点格式权威源

- 事实依据成立：ESP-IDF 的 `printf` 浮点输出依赖 newlib 配置（nano 格式化与完整 newlib 的 `%f` 行为不同），跨配置跨版本输出可能变化；
- "canonical 字节 = 设备打印原文、PC 不重格式化、跨版本按 `firmware_version` 分组各自重算"的规则可执行，且与 USB 导出（日志原文）的实际数据流一致；
- 该规则同时正确地回避了"PC 端自行约定小数位导致 CRC 永远对不上"的陷阱。

结论：F3 正确且必要。

### F4 二段式读取 / 布局表 / NVS 披露

二段式读取：

- 代码事实：`scan_committed_records()` 与导出路径以 `sizeof(header)` 一次性读取，NVS fallback 的 `read_key()`（L249）同样以 `sizeof(*header)` 截断读取——§6.2/§6.5 的原始发现属实，修正的必要性成立；
- 中文版定义了宽容式（`header_size < sizeof(struct)` 只读声明前缀）；复审将其扩展到英文版，并统一为"严格式/宽容式必须声明其一，宽容式只允许 reserved 区追加"的规则，两份文档现已一致；
- 宽容式的动机（未来 v6 仅追加 reserved 字段时旧固件可读）在逻辑上自洽。

v5 布局表（§8.1）：与 `radar_capture_batch.h` L182–L223 的 struct 逐字段核对：

| 检查项 | 文档声明 | 代码事实 | 判定 |
| --- | --- | --- | --- |
| magic/version/header_size | 0–7 | L183–185 | ✅ |
| batch/run/capture id | 8–19 | L186–188 | ✅ |
| 条件区 16 B（含 2 个 observed 字节） | 20–35 | L189–203 | ✅ |
| target/measured | 36–43 | L204–205 | ✅ |
| record_count…record_crc32 7×4 B | 44–71 | L206–212 | ✅ |
| ftm_raw 4 字段 12 B | 72–83 | L215–218 | ✅ |
| reserved[28] | 84–111 | L219 | ✅ |
| 总尺寸 | 112 B | L222 static_assert | ✅ |

复审更正（见 §3.1）：新增字段从 10 B 减为 8 B（mode 复用既有字节），剩余 reserved 从 18 B 修正为 20 B。

NVS 披露（§8.2）：

- 代码事实：`valid_header()` L214 为严格 `version == CAPTURE_STORAGE_VERSION`（当前 2），v3 升级后旧 blob 必然走 L245 invalid_header 拒绝路径，下次 save 覆盖——披露的丢失行为属实；
- NVS 仅是 slot2 不可用时的 fallback（save 失败才写入），"升级前先完整导出"的操作规程合理；
- batch 路径独立版本 + v3/v4 兼容读不受影响，表述准确。

## 3. 复审新发现并已当场修正的问题

### 3.1 §8.1 初稿把 mode 写成了新增字段（语义重复）

初稿在 reserved 区新增了 `observed_mode` 字段，但条件区既有 `observed_level`/`observed_obstruction` 字节在 v4 中本来就是 last-sample 语义，v5 将其重定义为 mode——同一语义出现两处存储，且两处可能不一致。

修正：mode 不占新字段，直接重定义条件区既有字节，按 version 字段区分解释；新增字段合计 8 B（probe_session_count、aggregate_flags、observed_first_last、observed_valid_samples），reserved 剩余 20 B。读取端禁止跨版本混读。

### 3.2 英文 §5.4 与中文读取策略表述不一致

中文定义了宽容式（`<` 只读前缀），英文初稿只有严格式（`!= sizeof` 即拒绝）。已统一为两种严格度可选、实现必须声明的规则，中英一致。

### 3.3 上游文档失效引用与旧声明残留

- 英文参数更新文档引用了不存在的"§10"（该章节只存在于中文版），已改为指向 §4.1.4 与补救设计；
- 中文 §10.3 的"最多约 8 个 session / 容量 16 留有余量 / PC 按 bursts 加权"三条旧声明与静态验证结论矛盾，已替换为容量 19 上界、v4 缺陷披露与"bursts 不作权重"的正确表述。

## 4. 遗留提示（不阻塞实施）

1. §3.6 的 `observed_*_mode` 定义为 run 内众数；实现时需明确并列众数的打破规则（建议取 first 与 last 之间较晚出现者），写入实现注释或后续文档；
2. 布局表假设 v5 header 仍为 112 B；若实施中发现字段超预算，优先压缩 `observed_first_last` 的打包位宽而不是突破 112 B；
3. `BATCH_CAPTURE_SUMMARY` 行的完整字段清单在实施 P1/P2 时随 parser 一并冻结，本报告不预先规定。

## 5. 门禁

```text
./tools/validate.sh --static  PASS（549 文件扫描，host 全套 PASS）
双语文档首部互链：已包含
```

本轮仅更新设计与复审文档，未修改业务代码，未创建新的本地 commit。
