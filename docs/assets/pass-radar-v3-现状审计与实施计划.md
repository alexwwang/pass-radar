# pass-radar v3 当前现状审计与分阶段实施计划
# pass-radar v3 Current-State Audit and Phased Implementation Plan

> 日期 / Date: 2026-09-20  
> 基线 / Baseline: 当前开发分支及工作区中已有的未提交修复  
> 原则 / Principle: 结果可用性优先；无线能力缺失时必须诚实降级，不得阻塞 UI、按键或导致崩溃。  
> Priority: usable and honest results first; missing radio capabilities must degrade explicitly and must never block the UI/input path or crash.

## 1. 现状结论 / Current conclusion

当前固件已经具备启动安全降级、基础 UI、ESP-NOW RSSI 距离降级、部分 CSI/遮挡分析、持久诊断记录以及单一正式固件构建路径。passport-sim 当前只承担正式镜像 boot 与首帧验证；由于模拟器缺少蓝牙硬件，按键交互不作为 simulator 的验收条件。

The firmware now has safe boot degradation, the basic UI, ESP-NOW RSSI distance fallback, partial CSI/obstruction analysis, persistent diagnostic breadcrumbs, and one production-firmware build path. passport-sim currently verifies only production-image boot and first-frame rendering; because the simulator lacks Bluetooth hardware, button interaction is not a simulator acceptance criterion.

当前仍不能称为 v3 完成：FTM 尚未形成“成功结果 → 标定状态 → 历史窗口 → v3 融合 → 真实成功率”的闭环；扫盘参数、协议令牌、BLE 惰性启动、CSI 多观测量接入、HOMING/音频和完整真机验收仍未完成。

The v3 implementation is not complete: FTM still lacks the complete “successful result → calibration state → history window → v3 fusion → real success rate” path; sweep parameters, protocol token exchange, lazy BLE startup, CSI multi-observation integration, HOMING/audio, and full hardware acceptance remain incomplete.

## 2. 已完成或已有基础 / Completed or partially available

- 正式固件只有一个业务构建路径；不再依赖 `CONFIG_RADAR_SIM_BUILD` 注入假数据。
- Production firmware has one business build path and no longer relies on `CONFIG_RADAR_SIM_BUILD` synthetic data injection.
- UI、LVGL、按键先启动；ESP-NOW/FTM/CSI/BLE 失败时保留 UI 并降级。
- UI/LVGL/input start first; ESP-NOW/FTM/CSI/BLE failures preserve the UI and degrade.
- ESP-NOW 双向 RSSI、对端身份、信道和 AP MAC 已有实现基础。
- Bidirectional RSSI, peer identity, channel, and AP MAC handling exist in ESP-NOW.
- `radar_fusion_update_v3()` 已有 RSSI/FTM/遮挡区间的主要分支及 host 可测试接口。
- `radar_fusion_update_v3()` already contains the main RSSI/FTM/obstruction interval branches and host-testable interfaces.
- 结果页方向文案已定义为相对起始方向：`DIR: FRONT-LEFT`、`CW 315+/-20 deg`、`0=TOP EDGE AT OK`。
- Result-page direction semantics are relative to the sweep start: `DIR: FRONT-LEFT`, `CW 315+/-20 deg`, `0=TOP EDGE AT OK`.
- 持久诊断记录可保存 boot/display/radio/CSI/peer 阶段，便于真机崩溃后读取。
- Persistent diagnostic breadcrumbs record boot/display/radio/CSI/peer stages for post-crash inspection.
- simulator 只检查正式镜像 boot 和首帧，不因缺少蓝牙而误判业务失败。
- simulator checks only production-image boot and first frame, avoiding false failures caused by missing Bluetooth.

## 3. 未完成需求与风险 / Incomplete requirements and risks

### P0：先完成核心测距闭环 / P0: complete the core ranging loop first

1. **FTM 标定状态未持久化闭环。** 当前成功 FTM 快照仍固定为 `calibrated=false`，因此不会进入 `radar_fusion_update_v3()` 的 FTM 主分支。
   **FTM calibration state is not closed.** Successful snapshots are still forced to `calibrated=false`, so they cannot enter the FTM branch of `radar_fusion_update_v3()`.
2. **FTM 历史窗口/P25 尚未接入快照。** 当前只有单次 session 中值和 IQR；没有最近 session 历史、P25 距离和历史 spread。
   **FTM history/P25 is not connected to the snapshot.** Only the current session median and IQR exist; recent-session history, P25 distance, and history spread are missing.
3. **FTM 成功率是伪统计。** 当前 `ftm.valid ? 1.0 : NAN` 不能表示最近 N 次发起的真实成功率。
   **FTM success rate is synthetic.** `ftm.valid ? 1.0 : NAN` is not the real success rate over recent initiations.
4. **双卡 FTM 真机链路未验收。** 需要确认两卡 AP/信道/BSSID、FTM report、状态码和失败回退。
   **The two-device FTM path has not passed hardware acceptance.** AP/channel/BSSID, FTM reports/statuses, and fallback behavior must be verified.
5. **RSSI 降级必须保留。** FTM 不可用、无对端、报告超时或结果异常时，仍必须显示合理的 RSSI 距离/区间，不得显示伪造的 FTM 成功。
   **RSSI fallback must remain.** When FTM is unavailable, the peer is absent, reports time out, or results are invalid, the UI must show an honest RSSI distance/interval and never fake FTM success.

### P1：v3 功能闭环 / P1: v3 feature closure

- 扫盘仍为 4 秒/100 样本，设计目标为 6 秒、25 ms 周期、最多 256 样本；是否去跳频需在真机验证后实施。
- Sweep is still 4 seconds/100 samples; the design target is 6 seconds, 25 ms, up to 256 samples. Channel-hopping removal requires hardware verification.
- `FTM_GRANT/FTM_BUSY/FTM_CALIB/OBSTRUCTED` 已定义但尚未形成双端令牌状态机。
- `FTM_GRANT/FTM_BUSY/FTM_CALIB/OBSTRUCTED` are defined but do not yet form a two-device token state machine.
- BLE 当前开机初始化，尚未实现 TRACKING 关闭、LOST 惰性启动的完整调度。
- BLE currently initializes at boot; the complete TRACKING-off/LOST-lazy-start schedule is not implemented.
- CSI 当前主要提供基础选择性指标，尚未保存完整四组序列并参与扫盘多观测量投票。
- CSI currently provides basic selectivity only; full four-group sequences and multi-observation sweep voting are not integrated.
- 结果页静态图示、弱信号扇区和文字叠加需继续按真机屏幕确认。
- The result-page static graphic, weak-signal sector, and text overlay still require hardware-screen confirmation.

### P2：增强功能 / P2: enhancements

- `radar_rf_sched` 状态机统一 Tracking/Sweep/Result/Calib/Lost。
- Unified `radar_rf_sched` state machine for Tracking/Sweep/Result/Calib/Lost.
- HOMING 页面、音频节拍器和寻的音调。
- HOMING page, audio metronome, and homing tone.
- v3.1 的 15~100 m 远距、树丛/围墙遮挡和举高模式。
- v3.1 15–100 m ranging, foliage/wall obstruction, and raised-device mode.

## 4. 分阶段实施计划 / Phased implementation plan

### 阶段 A：FTM 核心闭环 / Phase A: FTM core closure

**A1（当前开始 / start now）**

- 增加 FTM calibration-valid 状态及 NVS 恢复/保存接口。
- Add explicit FTM calibration-valid state and NVS restore/save interfaces.
- 增加最近 session 成功/失败窗口，提供真实成功率。
- Add a recent session success/failure window and expose the real success rate.
- 增加最近 FTM 距离历史并以 P25 作为输出距离，保留本次 IQR。
- Add recent FTM distance history and use P25 as output distance while retaining current-session IQR.
- 让 `run_fusion()` 传递真实成功率和 calibration 状态；FTM 只有在真实标定且新鲜时进入 v3 融合。
- Make `run_fusion()` pass the real success rate and calibration state; FTM enters v3 fusion only when genuinely calibrated and fresh.
- 所有失败路径继续 RSSI 降级，并记录明确状态。
- Keep RSSI fallback on every failure path and record explicit status.

**A1 验收 / A1 acceptance**

- host 测试覆盖：未标定不采纳 FTM、标定后采纳 FTM、成功率随失败变化、P25 抑制高值离群。
- Host tests cover: uncalibrated FTM rejected, calibrated FTM accepted, success rate changes with failures, and P25 suppresses high outliers.
- 固件构建通过，不能引入新的 simulator 专用固件。
- Firmware build passes without introducing a simulator-only firmware.
- 真机前只确认代码/构建，不宣称 FTM 真机已调通。
- Before hardware testing, claim only code/build readiness—not hardware FTM success.

### 阶段 B：扫盘与结果页 / Phase B: sweep and result page

- 在阶段 A 真机最小验证通过后，将扫盘调整为 6 秒/25 ms/256 样本。
- After the minimal Phase A hardware check, change sweep to 6 s/25 ms/256 samples.
- 去除或保留跳频必须以双卡 RSSI/FTM 稳定性数据决定，不能只凭 simulator 判断。
- Remove or retain channel hopping based on two-device RSSI/FTM data, not simulator behavior alone.
- 恢复结果页静态雷达图并叠加三行英文文字；弱信号显示不确定扇区。
- Restore the static result radar graphic with three English lines; show an uncertainty sector for weak signals.

### 阶段 C：协议、BLE、CSI 与诊断 / Phase C: protocol, BLE, CSI, diagnostics

- 实现 FTM token grant/busy/calibration 交替机制，并兼容 v2 对端。
- Implement alternating FTM grant/busy/calibration tokens with v2 compatibility.
- 采用 LOST 惰性 BLE，验证资源释放和重新初始化。
- Implement lazy BLE startup in LOST and verify release/reinitialization.
- 完成 CSI 四组能量序列、时间戳和扫盘投票接入。
- Complete CSI four-group energy sequences, timestamps, and sweep-voting integration.
- 扩充诊断页/持久记录：FTM 发起、成功、失败状态、CSI、样本数和最后阶段。
- Extend debug/persistent records with FTM attempts, successes, status failures, CSI, sample count, and last stage.

### 阶段 D：HOMING、音频与 v3.1 / Phase D: HOMING, audio, and v3.1

- 先完成状态机和音频任务，再实现 HOMING。
- Complete the scheduler and audio worker before implementing HOMING.
- v3.1 必须建立在一期 M3 真机验收完成之后。
- v3.1 starts only after Phase-1 M3 hardware acceptance.

## 5. 验证与交付门槛 / Verification and delivery gates

每个阶段都必须满足：

Each phase must satisfy:

1. `./tools/validate.sh --static` 通过 / passes.
2. host 测试通过 / host tests pass.
3. `./tools/validate.sh --firmware` 通过；构建 timeout 使用 15 分钟 / passes with a 15-minute build timeout.
4. 只使用一个正式固件镜像；passport-sim 只验证正式镜像 boot/首帧 / one production image only; passport-sim verifies production boot/first frame only.
5. 真机测试记录区分“一人可独立完成”和“两人协同可选”项目 / hardware test records distinguish solo tests from optional two-person tests.
6. FTM 未通过双卡真机证据前，UI 必须明确显示 RSSI 降级而不是 `FTM ok` / until two-device hardware evidence exists, UI must honestly show RSSI fallback rather than `FTM ok`.
7. 文档、构建镜像名、源码状态和测试结果同步记录 / documentation, image name, source state, and test results are recorded together.

## 6. 当前实施边界 / Current implementation boundary

本次从 **A1** 开始，不同时实现扫盘、HOMING、音频或 v3.1。原因是 FTM 是否真的进入融合是当前结果可用性的关键路径；先把状态、统计、标定和降级闭环做好，再进行真机最小验证。

This work starts with **A1** and does not implement sweep, HOMING, audio, or v3.1 in parallel. Whether FTM truly enters fusion is the key result-usability path; calibration, statistics, state, and fallback must be closed before minimal hardware validation.

## 7. A1 实施记录 / A1 implementation record

已完成 / Completed:

- FTM 快照增加本次 session 中值、历史 P25、历史 spread 和历史计数。
- FTM snapshots now carry the current-session median, history P25, history spread, and history count.
- 增加最近 10 次常规 session 成功率；低 RSSI 探针不计入该成功率。
- Added success rate over the latest 10 regular sessions; low-RSSI probes are excluded.
- FTM 标定有效状态只在 NVS 偏移成功恢复或新标定值成功写入后置为有效。
- FTM calibration becomes valid only after an offset is successfully restored from NVS or successfully saved from a new calibration.
- 失败、超时、无有效测量继续走 RSSI 降级，并清除错误的 probe 会话标志。
- Failure, timeout, and no-valid-measurement paths retain RSSI fallback and clear stale probe-session state.
- host 新增 P25 低分位测试；正式镜像 boot/首帧回归通过。
- Added a host P25 test; production-image boot/first-frame regression passes.

验证结果 / Verification:

```text
Repository/static checks: PASS
Host tests: PASS
git diff --check: PASS
Firmware build: PASS
Protected firmware layout: PASS
passport-sim production boot/first frame: PASS
Firmware artifact: build/pass-radar_v0.1-10-ge5cde1e.bin
App size: 1,233,680 bytes
```

限制 / Limitations:

- 以上仍未证明 ESP32-C3 双卡 FTM 在真机上成功；必须通过现场日志和 debug/持久记录确认。
- This does not prove two-device ESP32-C3 FTM success on hardware; field logs and debug/persistent records are still required.
- 当前成功率统计从本次开机开始，不跨重启持久化；跨重启统计留在诊断阶段处理。
- The success-rate window starts at boot and is not persisted across reboots; cross-boot statistics remain a diagnostics-phase task.
- simulator 仍只验证 boot/首帧，不验证无线、测距、BLE 或按键。
- simulator still verifies boot/first frame only, not radio, ranging, BLE, or buttons.

## 8. 下一阶段实施记录：token、扫盘和 debug / Next-phase record: token, sweep, and debug

已完成 / Completed:

- v3 包复用 `v3_flags` 增加 FTM grant/busy/calibration 广播；v2 对端继续使用 MAC 小者回退策略。
- Reused `v3_flags` for FTM grant/busy/calibration broadcasts; v2 peers retain the MAC-lower fallback.
- 增加 FTM token watchdog：双方控制信息丢失超过 3 秒时，由 MAC 较小的一端恢复 token。
- Added an FTM token watchdog: if control information is absent for over 3 seconds, the lower-MAC side recovers the token.
- FTM session 开始/结束会同步 token 状态，失败和超时也会释放 busy 状态。
- FTM session start/end now updates token state; failure and timeout also release busy state.
- 扫盘参数改为 6 秒、最多 256 个样本；ESP-NOW 广播周期在 Tracking 为 50 ms、Sweep 为 25 ms。
- Sweep is now 6 seconds with up to 256 samples; ESP-NOW broadcast period is 50 ms in Tracking and 25 ms in Sweep.
- Debug 页增加 FTM 成功率、标定状态和历史窗口数量显示。
- Debug page now shows FTM success rate, calibration state, and history-window count.

验证结果 / Verification:

```text
Host/static checks: PASS
Firmware build: PASS
Protected firmware layout: PASS
passport-sim production boot/first frame: PASS
Firmware artifact: build/pass-radar_v0.1-10-ge5cde1e.bin
App size: 1,234,640 bytes
```

保留的真机决策点 / Hardware decision still pending:

- 扫盘跳频仍保留；必须通过双卡真机数据确认去跳频是否改善结果，再删除 `hop_tick()`。
- Channel hopping is still retained; two-device hardware data must confirm whether removing it improves results before `hop_tick()` is deleted.
- token、FTM 成功率和 6 秒样本数必须通过真机日志确认，simulator 不能替代这些验证。
- Token behavior, FTM success rate, and six-second sample count must be confirmed with hardware logs; simulator cannot replace these checks.
