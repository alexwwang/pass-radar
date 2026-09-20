# pass-radar v3 Debug Capture 与当前固件真机测试计划

英文版本：`pass-radar-v3-debug-capture-and-current-hardware-test-plan.md`

日期：2026-09-20
固件范围：由 `./tools/validate.sh --firmware` 生成的当前正式镜像
状态：基本真机回归已完成；Debug Capture 已完成设计但尚未实现。

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

两张卡使用同一个正式镜像。确认 slot2 启动方式，记录镜像大小和安装时间；除非测试清洁状态，否则保留 NVS；准备卷尺和 USB 日志。

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

将正式镜像安装到 slot2，启动 pass-radar，验证约定的 OK LONG2 返回路径。本项不要使用 UP 长按。通过标准是正常启动和返回，没有固件损坏或重启循环。

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

## 10. 停止条件和下一步

出现重启、panic、白屏、退出、卡死、没有依据的 FTM 成功、供电异常或缺少原始证据时，立即停止参数采集。

下一步顺序：

1. 刷入当前正式镜像，重新执行 H-01/H-02；
2. 使用新诊断日志复测 H-04、H-06、H-08、H-09；
3. 修复剩余的可观测性或状态恢复问题；
4. 实现 Debug Capture 和 CSV/JSONL 导出；
5. 完成以上工作后，再采集受控距离数据并修正 FTM、RSSI、扫盘和遮挡参数。
