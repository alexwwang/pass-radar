# pass-radar — FoloToy AI Passport 户外 ESP-NOW 定向雷达

[English](README.md) | 简体中文

pass-radar 是一个 **360° 定向雷达玩具**，运行在 FoloToy AI Passport（ESP32-C3，
8MB Flash）上。利用**人体 2.4GHz 屏蔽效应**检测 ESP-NOW 信号峰值，配合**卡尔曼
滤波**，将单天线卡片升级为能估算目标**距离**和**方向**的伪雷达。

## 工作原理

两台设备刷**同一固件**——各自以 20Hz 广播 ESP-NOW 包，同时接收对方信号。
接收方人体会部分遮挡 2.4GHz 信号——旋转卡片时，天线正对另一台设备的方向
会出现可测量的 RSSI 峰值。

**v2 增强**同时提升测距与定向准确度：

- **Wi-Fi FTM（802.11mc RTT）测距**——基于时间戳的往返测距，不受发射
  功率/天线增益标定误差影响（米级，而 RSSI 误差常达 50%~100%）。
- **双向 RSSI 融合**——双端互传各自测得的 RSSI，对置消除天线方向图不对称。
- **信道分集扫盘**——4 秒旋转期间射频每 100ms 在 1/6/11 信道间跳变；每个
  信道的方向图做一次谐波最小二乘拟合并投票（环形中值），剔除多径假峰。
- **BLE Coded PHY 哨兵**——长距低占空比 BLE 链路在 Wi-Fi 超界（~30m）后
  保持"FAR (BLE link)"粗提示。
- **置信度输出**——每个结果都带 HIGH/MED/LOW/OOR 置信度，可信度一目了然。

- **追踪模式**（默认）：实时距离估算，极坐标雷达 UI 显示同心距离环。
- **扫盘模式**（按 OK 键）：4 秒引导旋转。采集带时间戳的 RSSI 采样，找到
  峰值（人体遮挡最小的方向），将峰值时间映射为相对起始方向的 0°–360° 角度。

## 功能

- **双角色固件**：每台设备同时广播（20Hz）和接收——两台卡片刷同一固件即可
  自动配对。
- **卡尔曼滤波 RSSI**：一维卡尔曼滤波（q=0.15，r=4.0）将 ±5dB 原始抖动平滑为
  稳定估计值。
- **融合测距**：FTM RTT（主）+ 双向对称 RSSI（路径损耗模型，室内外衰减
  指数自适应），带反射径判别与置信度分级（HIGH/MED/LOW/OOR）。
- **360° 定向**：4 秒引导旋转人体遮挡检测，叠加信道分集投票（1/6/11 快跳）、
  一次谐波拟合、抛物线插值与峰显著性门限。
- **1m 标定**：追踪页 OK 长按学习 1m 对称参考 RSSI（存入 NVS）。
- **BLE 哨兵**：Coded PHY 长距链路在 Wi-Fi 超界时提示 "FAR (BLE link)"。
- **弱信号保护**：RSSI 低于 -88dBm 时显示"超出范围"，不误报方向。
- **最大发射功率**：19.5dBm，户外有效距离约 30 米。
- **meta-pass 兼容**：适配子固件——跨重启常驻，OK LONG2（3秒）返回启动器。

## 按键

| 页面 | OK 短按 | OK 长按（1.5s） | OK 超长按（3s） |
| --- | --- | --- | --- |
| 追踪页 | 启动 360° 扫盘 | 1m 标定（3s） | 返回启动器 |
| 扫盘页 | — | 取消，返回追踪页 | 返回启动器 |
| 结果页 | 返回追踪页 | 返回追踪页 | 返回启动器 |
| 标定页 | 取消 | 取消 | 返回启动器 |

## 快速开始

### 烧录

从 Releases 下载 `pass-radar_v<version>.bin`，或自行编译（见"开发"）：

```bash
python -m esptool --chip esp32c3 -p <port> -b 460800 \
    write-flash 0x0 pass-radar_v<version>.bin
```

### 使用

1. 在**两台** AI Passport 卡片上烧录 pass-radar（同一固件）。
2. 开机后各自显示追踪界面，实时显示到对方的距离。
3. 按 **OK** 启动 4 秒扫盘。卡片胸前平握，顺时针匀速转一圈。
4. 结果页显示检测角度和距离。

### 配合 meta-pass 启动器

pass-radar 可作为子固件安装到
[meta-pass](https://github.com/alexwwang/meta-pass) OTA 槽位。启动时自动标记
有效（跨重启常驻），OK LONG2（3秒长按）返回启动器。

## 开发

```bash
source <esp-idf-v5.5.3>/export.sh   # 需要 ESP-IDF v5.5.3
./tools/validate.sh --static        # 仓库检查 + host 测试
./tools/validate.sh --firmware      # 固件构建 + 保护分区验证
```

纯逻辑模块（卡尔曼滤波、扫盘峰值检测）是无 ESP-IDF 依赖的 C 代码，可在 PC 上
直接编译运行测试。UI 和 ESP-NOW 集成可通过
[passport-sim](https://github.com/alexwwang/passport-sim) Web 模拟器或真机验证。

## 仓库结构

| 路径 | 内容 |
| --- | --- |
| `main/` | 应用入口（`main.c`）、雷达 UI（`radar_ui`）、ESP-NOW 传输（`radar_espnow`）、FTM 测距（`radar_ftm`）、BLE 哨兵（`radar_ble`）、纯逻辑（`radar_kalman`、`radar_sweep`、`radar_fusion`）、UI 辅助（`ui_pixel`）、meta-pass 钩子（`metapass_hook`） |
| `components/bsp/` | 板级支持包（含 LONG2 按键事件） |
| `tools/validate.sh` | 统一门禁：静态检查 + host 测试 + 固件构建 |
| `tests/` | 纯逻辑模块 host 测试 |
| `docs/assets/pass-radar-design.md` | 设计文档 |

## 验证状态

| 检查项 | 结果 |
| --- | --- |
| Host 测试 | 卡尔曼收敛 + 降噪、距离模型、峰值角度检测——全部通过 |
| 固件构建 | ESP-IDF 5.5.3，app 961KB / 3MB（31%），保护分区 PASS |
| Web 模拟器 | 雷达 UI 渲染、按键交互、扫盘动画、弱信号保护、完整页面循环——全部验证 |
| 真机 | 双机配对、实时距离追踪、360° 扫盘定向——已验证 |

## 与官方固件的关系

本仓库基于 [FoloToy/ai-passport](https://github.com/FoloToy/ai-passport)
（`f75873f`，MIT）。`factory`/`cardid` 分区布局、`verify_firmware.py` 和 BSP
与基线兼容。移除了官方演示页面，关闭了 BLE。本项目为非官方项目，与
FoloToy 无关联。
