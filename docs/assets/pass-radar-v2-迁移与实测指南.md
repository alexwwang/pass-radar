# pass-radar v2 定位增强 —— 迁移与真机实测指南

## 交付物清单

| 文件 | 用途 |
| --- | --- |
| `pass-radar_v0.1-2-gf7c645c.bin` | 已编译好的完整固件（含 bootloader，烧录地址 0x0），可直接双卡烧录实测 |
| `pass-radar-v2-enhancement.patch` | 单提交补丁（`git am` 直接应用到你的仓库） |
| `pass-radar-v2-branch.bundle` | Git bundle（含分支 `feature/ranging-direction-enhancement`，可 `git fetch`） |
| `pass-radar-v2-源码包.zip` | 完整工作树源码（不含 .git/build/managed_components） |

## 应用方式（三选一）

```bash
# A. 补丁（推荐,你的仓库已克隆时）
cd pass-radar && git checkout -b feature/ranging-direction-enhancement
git am < pass-radar-v2-enhancement.patch

# B. bundle
cd pass-radar
git fetch /path/to/pass-radar-v2-branch.bundle \
    feature/ranging-direction-enhancement:feature/ranging-direction-enhancement
git checkout feature/ranging-direction-enhancement

# C. 直接解压源码包作为新工作区
unzip pass-radar-v2-源码包.zip
```

## 验证状态（交付前）

```text
Build:       PASS (ESP-IDF 5.5.3, ESP32-C3, 应用 1.25MB / 3MB 上限)
Host tests:  PASS (kalman / sweep / fusion 共 3 套件,含新增 7 组融合测试)
Device tests: NOT RUN（需真机,按下文矩阵执行）
Unverified:  FTM 双端实测精度、BLE 哨兵覆盖半径、Wi-Fi/BLE 共存稳定性、
             标定流程体验、跳频跟随同步在丢包下的表现
```

## 烧录与快速冒烟

```bash
python -m esptool --chip esp32c3 -p <port> -b 460800 \
    write-flash 0x0 pass-radar_v0.1-2-gf7c645c.bin
```

两台卡都烧录后：

1. 开机进追踪页。第二行应显示 `RSSI:xx 2way [MED]`（双向 RSSI 已交换）。
2. 相距 3~5m 静置 30s：出现 `[HI]` 且 `FTM` 字样 → FTM 测距生效
   （FTM 每 2s 一轮，日志可见 `FTM ok: x.xxm`）。
3. 距离读数对比 v1：应在真实距离 ±2m 内收敛，且显著更稳。
4. 主页面 **OK 长按**：进入 3s 标定（两卡相距 1m 静止），之后距离系统性
   偏差应进一步减小（标定值持久化）。
5. 按 OK 扫盘：结果页显示 `Angle: xxx+/-yy deg`，`+/-` 是三信道投票分歧。
   同一方向多扫几次，v2 的读数离散度应明显小于 v1。
6. 双卡远离至 Wi-Fi 超界（>30m）：主页面应显示 `FAR (BLE link)`
   （BLE Coded PHY 哨兵，最远约 100m+ 开阔场）。

## 真机测试矩阵（建议）

| 场景 | 项目 | 期望 |
| --- | --- | --- |
| 室外空地 | 1/3/5/8/12/20/30m 距离梯度 | FTM 生效段误差 ≤2m；置信度 HI/MED |
| 室内多径 | 同上梯度 | FTM 存疑时降级 RSSI 且置信度 MED；不出现离谱跳变 |
| 定向 | 8 方向 × 3 次扫盘 | 角度误差 ≤±15°；`+/-` 分歧室内偏大属正常 |
| 跳频跟随 | 一端扫盘时观察另一端追踪页 | 对端追踪不中断（跟随跳频同步） |
| 共存 | 连续使用 10min | 无重启、无 LVGL 卡死（Wi-Fi+BLE 共存压力） |
| 超界 | 双卡拉远至 100m | Wi-Fi OOR → BLE 哨兵接管 → 最终双超界 |
| meta-pass | 经启动器安装为子固件 | LONG2 (3s) 正常返回启动器 |

## 已知边界（来自方案调研，需在实测中确认）

- FTM 在室内多径下可能锁定反射径（距离偏大），融合引擎已按
  `FTM > RSSI×1.8 且 +2m` 规则降级；若你的场景规则触发过频/过少，
  调 `radar_fusion.h` 中 `RADAR_FTM_SUSPICIOUS_RATIO`。
- FTM Initiator 不需要与对端 SoftAP 关联（管理帧直达）；若日志持续
  出现 `FTM session status=3 (NO_RESPONSE)`，说明对端 FTM responder 未就位，
  检查两端是否都刷了 v2。
- 与 v1 固件混用时：v2 能收 v1 的包（16 字节，按旧格式解析），
  但双向 RSSI、跳频跟随、对端结果全部不可用，置信度停在 LOW——
  两端请都刷 v2。
- BLE 扫描占空比已压到约 10% 避让 Wi-Fi；若实测 ESP-NOW 丢包率上升，
  可在 `radar_ble.c` 中加大 `BLE_SCAN_ITVL_MS`。
- 定向基准仍是"扫盘起始方向"的相对角；若需绝对方位需后续加磁力计/IMU
  （本卡无 IMU，属硬件限制）。

## 代码结构速查

```
main/radar_fusion.c/h   融合引擎(纯逻辑):对称RSSI、FTM滤波、置信度、环境自适应、环形角度
main/radar_sweep.c/h    扫盘算法(纯逻辑):谐波拟合、抛物线插值、显著性、信道投票
main/radar_espnow.c/h   ESP-NOW 协议v2:双向RSSI回传、扫盘同步、信道快跳、NVS标定
main/radar_ftm.c/h      Wi-Fi FTM:对称 responder/initiator,事件解析
main/radar_ble.c/h      BLE Coded PHY 哨兵:扩展广播 + 稀疏扫描
main/radar_ui.c/h       UI:置信度标记(FTM/2way/HI-MED-LOW)、角度分歧、标定页
tests/                  host 测试:新增 test_radar_fusion.c,扩展 test_radar_sweep.c
```

纯逻辑模块均可用 `./tools/validate.sh --static` 在开发机上跑 host 测试；
完整门禁 `./tools/validate.sh` 需激活 ESP-IDF 5.5.3。
