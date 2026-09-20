# pass-radar v3 Debug Capture 与当前固件真机测试计划

英文版本：`pass-radar-v3-debug-capture-and-current-hardware-test-plan.md`

日期：2026-09-20
固件范围：由 `./tools/validate.sh --firmware` 生成的当前正式镜像
状态：基本真机回归已完成；Debug Capture MVP 已实现，等待真机验证。

## 1. 结论与范围

当前适合进行基本功能和稳定性真机测试，但还不适合进行参数修正型测试。Simulator 只验证正式固件引导和首帧显示；FTM、RSSI、ESP-NOW、BLE、CSI、token、按键和方向精度必须在真机上验证。

当前真机测试目标是确认正式固件能够启动、发现对端、在条件满足时运行 FTM、在失败时安全降级、进入扫盘、显示结果并保持稳定。当前不能根据人工观察结果调整距离、P25、RSSI、token、方向或遮挡参数。

## 2. 当前实现基线

当前固件已经包含：

- FTM 标定状态、最近 session 历史、P25 输出和近期成功率统计；
- `FTM_GRANT`、`FTM_BUSY`、`FTM_CALIB` 协议字段及 token watchdog；
- 50 ms 追踪更新、25 ms 扫盘更新、目标 6 秒扫盘和最多 256 个 RSSI 样本；
- FTM 不可用或 session 失败时的 RSSI 降级；
- Debug 页 FTM 状态、成功率、历史窗口以及 BLE 本机/对端分离诊断；
- 真机和 simulator 共用一条正式固件构建路径；
- RTC 和 NVS 诊断 breadcrumb；
- 带 USB Serial/JTAG 自动重连的主机日志监听器。

以下内容在产生受控采集数据前仍未证明：

- 距离精度和标定质量；
- P25、IQR 和历史窗口参数；
- token 公平性和长期交替行为；
- 扫盘样本完整性和方向精度；
- BLE 阈值和遮挡阈值。

## 3. Debug Capture 设计

Debug Capture 是诊断功能，不得改变正常追踪和扫盘行为。

### 3.1 入口和目标标签

建议界面：

```text
DEBUG CAPTURE
TARGET: 3m
UP/DOWN SELECT
OK START
```

第一版使用预设标签：

```text
1m / 3m / 5m / 8m / 10m / 15m / 20m / 30m
```

界面必须显示 `TARGET`，不能把它当成真实距离。精度测试必须使用卷尺测量的实际距离；用户输入的大概值只能作为分类标签，不能作为真值。

### 3.2 固定采集状态机

```text
SELECT_DISTANCE -> COUNTDOWN 3s -> SETTLE 5s -> CAPTURE 30s
                 -> SUMMARY -> SAVE / DISCARD / NEXT
```

倒计时数据丢弃。稳定等待数据不计入正式窗口。采集使用固定时长和固定字段。设备重启或写入失败时，本次采集必须标记为无效，不能静默拼接两次采集。

### 3.3 原始字段

每条观测至少应包含：

```text
test_id
firmware_version
boot_sequence
target_distance_label_m
elapsed_ms
local_rssi_dbm
peer_rssi_dbm
peer_alive
espnow_rx_count
espnow_drop_count
ftm_session_id
ftm_status
ftm_raw_m
ftm_session_median_m
ftm_p25_m
ftm_iqr_m
ftm_history_spread_m
ftm_calibrated
ftm_token_role
ftm_grant
ftm_busy
ftm_calib
csi_frame_count
csi_selectivity_db
env_level
obstruction_class
channel
sweep_id
sweep_sample_count
reset_reason
```

采集期间应保存在 RAM，结束后一次性批量写入。不建议逐条写 NVS。应通过 USB 导出 CSV 或 JSONL，只有导出成功后才允许清除记录。

## 4. 真机测试任务

`S` 表示一人可以完成。`O2` 表示两人协作更方便但不是必须。没有第二人时，O2 项必须记为 `NOT RUN (O2 optional)`。

### H-00：测试前检查 — S

两张卡使用同一个正式镜像。确认所选 meta-pass 槽位能够启动该镜像，记录槽位、镜像大小和安装时间；除非测试清洁状态，否则保留 NVS；准备卷尺和 USB 日志。Capture 测试不要求必须使用 slot2，任何已知可正常启动的槽位都可以。

### H-01：单卡启动稳定性 — S

一张卡启动后至少运行 60 秒，不进行按键操作。通过标准是进入 STATUS，期间无重启、白屏、退出；无对端时显示安全的等待或超界状态，不能卡死。

### H-02：第二张卡加入 — S

保持 A 卡运行，启动 B 卡，两卡相距约 3 m，观察 60 秒。通过标准是两卡都保持运行，至少一端显示稳定对端或 `2way`，且没有白屏、退出或重启。

### H-03：RSSI 降级 — S

两卡相距 3～5 m 运行 60 秒。不判定距离准确率。通过标准是 FTM 失败后仍显示明确的 RSSI 降级结果或明确的无链路状态，且无卡死、重启。单独看到 `1way[--]` 只能证明当前没有可用对端，不能证明 RSSI 降级正常。

### H-04：Debug 页和状态 — S

使用当前约定的 UP 双击进入 Debug。确认无崩溃和字库乱码。`FTM WAIT` 表示 session 正在等待；后续 session 必须自动更新为 `FTM ok` 或 `FTM fail`。如果永久停留在 `FTM --`，则判定失败。

### H-05：FTM 观察 — S

两卡固定相距约 3 m，观察 60 秒。一组 `Starting FTM session`、`FTM session ends ...` 表示一次 FTM session；`radar_ftm: FTM ok` 表示应用接受了 report。本项只验证链路存在和失败恢复，不验证准确率。

### H-06：token 观察 — S，建议两人

两卡运行两分钟，条件允许时收集两端日志。新诊断输出应包含：

```text
FTM_TOKEN role=... grant=... busy=... calib=...
```

RSSI 变化本身不能证明 token 交替。没有同时发起导致的崩溃是必要现象，但不足以单独证明 token 通过。

### H-07：进入扫盘 — S

在 STATUS 按一次 OK。通过标准是进入 SWEEPING、看到动画、完成扫盘且无重启或白屏。

### H-08：扫盘时间和样本冒烟检查 — S

连接 USB 日志，记录：

```text
SWEEP_START
SWEEP_DONE elapsed_ms=...
SWEEP_SAMPLES count=...
```

目标是约 6000 ms。人工估计的时长或未输出的样本数不能作为参数验收依据。

### H-09：结果页 — S

通过标准包括：显示结果页、可以返回 STATUS、字符可读且无崩溃。结果页还必须保留雷达盘面，根据结果角度绘制目标点，并在盘面下方显示方向、角度分歧、距离和置信度文字。本项不判断方向准确率。

### H-10：短时稳定性 — S

两卡运行 10 分钟，执行 3 次扫盘，每次间隔至少 30 秒。通过标准是无重启、白屏、退出、卡死或乱码，并且 STATUS/SWEEPING 可以重复进入。

### H-11：meta-pass 启动器兼容 — S

从所选的 meta-pass 槽位启动 pass-radar，验证约定的 OK LONG2 返回路径。本项不要使用 UP 长按。通过标准是正常启动和返回，没有固件损坏或重启循环。这个启动器兼容性检查独立于 Capture，也不要求使用 slot2。

## 5. 当前真机证据与评估

修复后的第一轮启动时打印：

```text
previous NVS: seq=8 reset=4 stage=PEER_SEEN(6) flags=0x0002 rssi=-38 csi=0 uptime=541ms
```

这证明上一轮故障是发生在发现对端之后的 panic/reset。本轮修复镜像运行期间没有再次出现 panic、白屏或重启。

当前评估如下：

| 测试 | 评估 |
| --- | --- |
| H-03 | 部分通过：观察到了 FTM 失败和恢复，但没有直接从 UI/日志确认当时显示了 RSSI fallback |
| H-04 | 部分通过：可以进入 Debug；旧版 `FTM --` 卡住问题已改为 `FTM WAIT`，等待新镜像复测 |
| H-05 | 观察性通过：观察到 FTM session、report、应用接受的 `FTM ok` 和失败恢复 |
| H-06 | 未确定：没有同时发起导致的崩溃，但此前无法看到 token 角色/控制字段；需要用 `FTM_TOKEN` 日志复测 |
| H-07 | 通过 |
| H-08 | 时间和样本数未验证；人工观察约 8 秒，不能证明目标 6 秒 |
| H-09 | 旧镜像部分失败；纯文字结果页已经修复为重新创建雷达盘面并绘制目标点，等待真机复测 |
| H-10 | 通过 |
| H-11 | 通过 |

本轮日志中出现过约 0.10 m、0.47 m、1.17 m、1.87 m 和 2.34 m 的 FTM 接受结果。由于没有真实距离标签、固定采集窗口和原始导出，这些值不能用于判断精度或修正 P25。

## 6. 已实施的诊断和可视化修改

当前开发镜像包含：

- 静态 FTM report 缓冲，避免 `sys_evt` 栈耗尽；
- 使用 `FTM WAIT` 替代会产生歧义的永久 `FTM --`；
- `FTM_TOKEN` role/grant/busy/calib 状态变化日志；
- `SWEEP_START`、`SWEEP_DONE elapsed_ms` 和 `SWEEP_SAMPLES count` 日志；
- 页面销毁后重新创建的 RESULT 雷达盘面；
- 根据扫盘角度绘制的结果目标点；
- 盘面下方的方向、角度分歧、距离和置信度文字。

这些修改已经通过仓库检查、host 测试、固件构建、受保护布局验证和 simulator boot/首帧验证，但仍需要真机复测。

## 7. Debug 页 BLE 状态语义

Debug 页将 BLE 状态拆成两个独立字段：

- `BLE S:OK` 表示本机 BLE/NimBLE 控制器、广播和扫描流程均已初始化成功；`BLE S:NO` 表示可选 BLE 子系统不可用。
- `P:YES` 表示最近 10 秒内收到过匹配的对端哨兵广播；`P:NO` 表示当前没有有效的匹配对端广播。
- `age=...ms` 表示最近一次匹配对端广播距现在的时间，可解释为什么本机 BLE 正常时 `P:NO` 仍会出现。

紧凑显示格式如下：

```text
BLE S:OK P:YES r-68 age2s
BLE S:OK P:NO  r-68 age12s
BLE S:NO  P:NO  r-- age--
```

`r` 是最近一次匹配对端广播的 RSSI，即使对端已经超过 10 秒有效窗口仍会保留；`age` 是最近一次匹配广播距现在的秒数。因此，`BLE S:OK P:NO` 在扫描间隙、FTM/扫盘共存暂停，或对端没有广播时都是正常状态，不代表 BLE 初始化失败。

## 8. 崩溃诊断和 USB 监听

正式固件使用 ESP32-C3 USB Serial/JTAG。主机监听器会在 USB 重枚举后自动重连：

```bash
cd /Users/alex/ai-passport/pass-radar
python3 tools/monitor-usb.py --output /tmp/pass-radar-usb-monitor.log
```

监听器不会切换 DTR/RTS，也不会主动复位设备。应先启动监听器，再给设备上电或复位。故障后保留日志文件，并在下一次启动时查看 `previous NVS` 和 `previous RTC breadcrumb`。

RTC breadcrumb 不依赖 panic 时写 Flash。由于掉电可能同时中断 CPU 执行和 USB 传输，因此仍不能保证获得完整 brownout 回溯。

## 9. 当前 USB 证据和诊断解释

最新 USB 监听日志不能概括为“FTM 始终失败”。日志中同时存在成功和失败：共发起 228 次 session，收到 222 次 session report，出现 80 次 `FTM ok`，以及 12 次明确失败状态。主要失败码为 status 5（未从 responder 收到测量值）、status 6 和 status 3。

Debug 页显示的是当前 FTM 快照是否仍在有效时间内，而不是全部历史结果。成功快照超过 5 秒有效窗口后会显示不可用；`radar_ftm_last_status` 则表示最近一次失败原因。这两者不能混为一谈。

Responder 角色本身不是错误。在 v3 中，Responder 获得 FTM grant token 后可以发起 FTM。因此 `FTM_TOKEN role=RESPONDER grant=1` 表示 token 已授予，而不是角色非法。

USB 诊断流现在包含：

```text
BLE peer matched rssi=...
BLE_STATE ready=1 peer=1 rssi=... age_ms=... scan=1 adv=1
FTM_SESSION start=... role=... probe=... rssi=... channel=...
FTM_SESSION end=... result=OK ...
FTM_SESSION end=... result=FAIL status=...
FTM_SESSION end=... result=TIMEOUT status=...
```

屏幕上的 `BLE S:OK P:NO` 应与 `BLE_STATE ready=1 peer=0` 对应，表示本机 BLE 栈正常，但当前十秒有效窗口内没有匹配的哨兵广播。`expected=1` 表示已经绑定了经 ESP-NOW 验证的 identity；`BLE candidate rejected` 表示扫描到了候选广播但因不是已绑定对端而拒绝。`FTM WAIT`、`FTM fail` 和 `FTM ok` 必须结合 session 编号、busy 状态、快照年龄和成功率一起解释。

最新真机检查确认，Debug 页显示的 BLE 字段与 USB 日志中的对应字段（`ready`、`peer`、RSSI 和 age）基本一致。因此 BLE 诊断可观测性验收通过。但这还不能证明 BLE 在远距离或遮挡场景下的性能；这些内容仍需受控采集数据验证。

## 10. 已实现的 Debug Capture MVP

当前正式镜像已经实现第一版静态距离采集闭环：

```text
Debug 目标标签选择 -> COUNTDOWN 3 秒 -> SETTLE 5 秒 -> CAPTURE 30 秒
                  -> SUMMARY -> 压缩 NVS 保存 -> USB CSV 输出
```

在 Debug 页使用 UP/DOWN 选择预设目标标签，按 OK 启动采集。目标值只是分类标签，不能当作真实测量值。采集页显示阶段、耗时、记录数、有效性；批量写入成功后显示 `SAVED CSV`。采集完成后按 OK 返回 Debug；采集进行中按 OK 会中止并将本次标记为无效。在 Debug 页 DOWN 长按可以重复导出 NVS 中最新一代有效记录，但不会清除记录。

MVP 在 RAM 中最多保存 128 条观测，采样周期为 250 ms。采集结束后将记录编码并一次性写入现有 NVS namespace，回读 payload 后使用 CRC32 校验，并通过 USB 输出固定 CSV 表头和 `CAPTURE_CSV` 数据行。写入失败会将本次采集标记为无效。当前 MVP 在保存后立即导出，并支持通过 Debug 页 DOWN 长按重复导出 NVS 中最新一代有效记录。当前故意不提供清除动作，避免误删原始证据。

MVP 当前保存可用的原始 RSSI、对端存活、FTM 状态/距离/分歧/session 数、BLE 状态/RSSI/年龄、CSI 帧数和环境等级。NVS header 和 `CAPTURE_META` 行现在还包含固件版本、采集 ID、boot sequence、复位原因、目标标签、记录数和 CRC32。正式用于最终参数调优前，还需要补充 token 字段、遮挡分类、信道，以及持久重复导出/清除控制。

之前的 MVP 实现写入项目自有的 `radar_capture` 数据分区。现有 meta-pass 分区表中没有这个分区，因此最近一次真机测试虽然完成了 RAM 采集，却出现了 `CAPTURE_SAVE failed: partition missing`。当前实现已经改为复用 meta-pass 现有的 `nvs` 分区；这条新路径仍需下一次真机测试验收。

## 11. 已实现的与 meta-pass 兼容的压缩 NVS 存储

下一版存储实现将复用 meta-pass 现有的 `nvs` 分区，不新增分区，也不修改 `cardid` 或任何 OTA 槽位。使用独立 namespace 和 key，避免与已有标定、诊断数据冲突：

```text
namespace: radar_capture
keys:      latest_a / latest_b
```

保存内容不采用 ZIP。项目当前没有现成的 ZIP/deflate/miniz 依赖；ZIP 会增加容器格式、固件代码、RAM 使用和损坏边界，但不能解决 NVS 写入本身的问题。计划使用带版本的二进制 payload 和轻量无损编码：

```text
定点字段 -> 差分编码 -> 无符号变长整数 -> 可选 RLE
```

编码必须保留原始观测含义。距离和分歧使用厘米定点数，RSSI 保留有符号 dBm，时间使用文档化的差分单位，计数器使用差分，状态使用 bit 打包。不能为了提高压缩率而静默丢弃或过度舍入字段。

存储 header 至少包含：

```text
magic
format_version
codec_id
generation
capture_id
target_distance_cm
record_count
uncompressed_length
compressed_length
firmware_version
boot_sequence
reset_reason
payload_crc32
```

当前未解包的 C 结构中，128 条记录约占 4.7 KB。紧凑字段预计可降到约 2.6～3.2 KB；差分/变长/RLE 还可能进一步减少，但发布前必须使用真实样本和最坏情况样本测得最大长度。最终 NVS blob 放不下时必须拒绝保存，并输出 `CAPTURE_SAVE FAIL NVS_SPACE`。

保存和导出契约如下：

1. 采集期间所有观测保存在 RAM，不能每条样本写一个 NVS 条目；
2. `CAPTURE_DONE` 后一次性编码完整 payload 并计算 CRC；
3. 两个 key 轮换写入一代数据：`latest_a` 或 `latest_b`；
4. 写入后重新读取，校验 header、长度、codec、generation 和 CRC，然后解码；
5. 只有回读和解码校验成功后，才能输出 `CAPTURE_SAVE OK` 和 CSV；
6. 重复导出时选择 generation 最新且有效的一份，解码后输出 CSV；
7. 写入、回读、解码或 CRC 任一步失败时，保留上一代有效数据并报告失败。

该方案针对当前 meta-pass 布局设计，但仍必须在运行时检查 NVS 可用条目，因为 meta-pass 可能已经占用 0x6000 字节 NVS 分区的一部分。压缩 NVS 格式已经在当前源码和正式构建中实现。编解码已通过 host 测试，固件门禁也已通过；但在 meta-pass 真机产生 `CAPTURE_SAVE OK` 并成功完成重复导出前，不能称为硬件验收通过。

## 12. 编译环境和正式固件命令

在 macOS/Linux 上，从仓库根目录使用 ESP-IDF 5.5.3 和现有的 uv Python 环境构建正式镜像。该命令的编译 timeout 为 15 分钟；它生成的是 meta-pass、真机和 passport-sim 共用的唯一固件，不存在 simulator 专用固件。

```bash
cd /Users/alex/ai-passport/pass-radar

export IDF_PATH=/Users/alex/esp/esp-idf-v5.5.3
export IDF_PYTHON_ENV_PATH="$HOME/.espressif/python_env/idf5.5_py3.10_env"
export PATH="$IDF_PYTHON_ENV_PATH/bin:$PATH"
source "$IDF_PATH/export.sh"

idf.py --version                         # 必须显示 ESP-IDF v5.5.3
./tools/validate.sh --firmware           # 15 分钟编译门禁
IMAGE="$(ls -t build/pass-radar_*.bin | head -1)"
printf 'Firmware: %s\n' "$IMAGE"
shasum -a 256 "$IMAGE"
```

命令完成后使用 `build/` 下最新的版本化文件，即上面 `$IMAGE` 的值。实际路径类似：

```text
build/pass-radar_v0.1-<commit>.bin
```

不要把中间文件 `build/pass-radar.bin` 或 `build/pass-radar-full.bin` 烧录到 0x0，也不要交给 passport-sim。不要为 simulator 单独编译固件。如果本机 ESP-IDF 路径不同，只替换 `IDF_PATH`；要求的版本仍必须是 5.5.3。任何源码变更后都必须重新执行构建，不能用旧镜像判断当前 commit。

## 13. Capture MVP 真机验证方法

### 12.1 所需环境

第一轮验证由一人操作两张卡即可完成；第二名测试者只是可选项，不是基本验收的必要条件。

必须准备：

- 两张卡都安装刚构建的同一个正式镜像，并从已知可正常启动的 meta-pass 槽位运行；slot2 不是必需的；
- 记录每张卡实际使用的槽位，并在启动时确认镜像版本；

- 两张卡使用稳定供电；采集过程中不要更换 USB hub、线材或供电来源；
- 支持数据传输的 USB 线，并至少能访问一张卡的 USB Serial/JTAG 端口；
- 卷尺和安静、固定的测试位置；
- 已实际测量的目标距离，两张卡静止，并标记预期的正面方向；
- 被监听端口不能同时打开 WebSerial、`idf.py monitor` 或其他串口客户端。

第一轮建议使用一个实际测量距离，例如 3 m。界面目标标签应与布置距离一致，但在日志记录实际测量值之前，标签仍然只是元数据，不能直接当作真值。

### 12.2 刷机和启动监听

保留当前可用槽位及其中的回退固件不变，把版本化正式镜像安装到两张卡实际选定的可启动槽位；如果 slot0 是当前已知可正常启动的槽位，使用 slot0 也可以。本轮不要清除 NVS：这样可以保留设备身份、诊断 breadcrumb、标定/历史数据和对端状态。清除 NVS 不是 Capture 的要求；如果要做 NVS 清洁实验，必须另行记录。确认设备屏幕或启动日志中的版本与 `build/` 中选定镜像一致。

如果当前布局是 `slot0=pass-radar`、`slot1=已有固件`、`slot2=空`，只要 slot0 启动的是新构建镜像，就可以进行 Capture。slot1 被占用、slot2 为空不会影响 Capture 状态机、BLE、FTM、RSSI 或 `radar_capture` 数据分区。影响只在回退和启动器安全性：不要覆盖 slot1，并在依赖回到启动器前先确认 meta-pass 返回路径。

在第一张卡上电或重启前，先启动自动重连监听器：

```bash
cd /Users/alex/ai-passport/pass-radar
rm -f /tmp/pass-radar-usb-monitor.log
python3 tools/monitor-usb.py \\
  --output /tmp/pass-radar-usb-monitor.log
```

整个测试过程中保持监听器运行。只有一张卡连接 USB 时，监听器会记录其中一张卡，另一张卡通过屏幕观察。如果两张卡共用 USB hub，先启动第一张卡，确认稳定后再连接/启动第二张卡。38 秒采集期间不要反复拔插或复位设备。

### 12.3 Capture 操作步骤

1. 启动 A 卡，确认进入 STATUS，且没有重启、白屏或退出。
2. 启动 B 卡，将两张卡放在实际测量距离处，等待对端/链路状态稳定。
3. 在任一张卡的 STATUS 页使用当前约定的 UP 双击进入 DEBUG。
4. 使用 UP/DOWN 短按选择与实际测量距离对应的预设 TARGET 标签。
5. 按一次 OK。页面应依次进入 `COUNTDOWN`、`SETTLE`、`CAPTURE`。
6. 在 3 秒倒计时、5 秒稳定期和 30 秒采集期间不要按键，也不要移动任一卡片。
7. 等待约 38 秒，直到页面显示 `SUMMARY`、`RECORDS` 和 `SAVED CSV`。
8. 保存 USB 日志。日志应包含 `CAPTURE_START`、`CAPTURE_META`、一个固定的 `CAPTURE_CSV` 表头、CSV 数据行和 `CAPTURE_SAVE OK`。
9. 记录结果后按 OK 返回。如需确认 Flash 重复导出，可再次进入 DEBUG，按 DOWN 长按；该操作不会清除记录。

### 12.4 Capture 验收标准

以下信息必须从原始日志和测试记录中保存，不能手工改写原始日志：

```text
固件版本
镜像 SHA-256
实际测量距离和选择的 TARGET 标签
capture_id
boot_sequence
reset_reason
记录数
CRC32
CAPTURE_SAVE 结果
页面是否显示 SAVED CSV
任一卡是否重启、白屏、退出或卡死
```

基本 Capture 通过标准是：完整完成一次 38 秒流程；无重启、panic、白屏、退出或卡死；记录数非零；出现 `CAPTURE_SAVE OK`；`CAPTURE_META` 与 CRC32 可对应；并且 `/tmp/pass-radar-usb-monitor.log` 中存在预期的 CSV 表头和数据行。这只能证明采集/存储/导出链路，不证明距离精度。不能根据一次采集调整 FTM、RSSI、BLE、P25 或方向参数。

如果采集被中止、页面显示 `INVALID`、没有保存结果、CRC 无法对应，或监听日志缺少保存行，则记录为 `FAIL` 并保留完整日志。在证据明确前，不要继续进行参数采集。

## 14. 停止条件和下一步

出现重启、panic、白屏、退出、卡死、没有依据的 FTM 成功、供电异常或缺少原始证据时，立即停止参数采集。

下一步顺序：

1. 在当前 meta-pass 布局上验证第 11 节已经实现的压缩 NVS；
2. 在当前 meta-pass 布局上验证 NVS 容量、双 key 恢复、回读 CRC 和 CSV 导出；
3. 在一个实际测量距离上验证 Capture MVP；
4. 确认 USB CSV 表头、记录数、CRC/保存状态，以及 38 秒流程内无重启；
5. 补充 token/遮挡/信道字段和需要明确确认的清除控制；
6. 再按受控距离矩阵重复采集；
7. 最后根据数据修正 FTM、RSSI、扫盘和遮挡参数。
