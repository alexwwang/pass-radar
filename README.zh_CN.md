# pass-radar — FoloToy AI Passport 户外 ESP-NOW 定向雷达

[English](README.md) | 简体中文

pass-radar 是一个 **360° 定向雷达玩具**，运行在 FoloToy AI Passport（ESP32-C3，
8MB Flash）上。利用**人体 2.4GHz 屏蔽效应**检测 ESP-NOW 信号峰值，配合**卡尔曼
滤波**，将单天线卡片升级为能估算目标**距离**和**方向**的伪雷达。

## 工作原理

两台设备配对：一台**发射** ESP-NOW 广播包（20Hz），另一台**接收**并测量 RSSI。
接收方人体会部分遮挡 2.4GHz 信号——旋转卡片时，天线正对发射端的方向会出现
可测量的 RSSI 峰值。

- **追踪模式**（默认）：实时距离估算，极坐标雷达 UI 显示同心距离环。
- **扫盘模式**（按 OK 键）：4 秒引导旋转。采集带时间戳的 RSSI 采样，找到
  峰值（人体遮挡最小的方向），将峰值时间映射为相对起始方向的 0°–360° 角度。

## 功能

- **卡尔曼滤波 RSSI**：一维卡尔曼滤波（q=0.15，r=4.0）将 ±5dB 原始抖动平滑为
  稳定估计值。
- **距离估算**：自由空间路径损耗模型，户外衰减因子 n=2.2。
- **360° 定向**：4 秒引导旋转，峰值时间→角度映射。
- **弱信号保护**：RSSI 低于 -88dBm 时显示"超出范围"，不误报方向。
- **最大发射功率**：19.5dBm，户外有效距离约 30 米。

## 按键

| 页面 | OK 短按 | OK 长按（1.5s） |
| --- | --- | --- |
| 追踪页 | 启动 360° 扫盘 | — |
| 扫盘页 | — | 取消，返回追踪页 |
| 结果页 | 返回追踪页 | 返回追踪页 |

## 快速开始

### 烧录

从 Releases 下载 `pass-radar_v<version>.bin`，或自行编译（见"开发"）：

```bash
python -m esptool --chip esp32c3 -p <port> -b 460800 \
    write-flash 0x0 pass-radar_v<version>.bin
```

### 使用

1. 在第二台 AI Passport 上烧录**发射端**固件（或任何运行 ESP-NOW 20Hz 广播
   的 ESP32 设备）。
2. 开机进入追踪界面，实时显示距离。
3. 按 **OK** 启动 4 秒扫盘。卡片胸前平握，顺时针匀速转一圈。
4. 结果页显示检测角度和距离。

## 开发

```bash
source <esp-idf-v5.5.3>/export.sh   # 需要 ESP-IDF v5.5.3
./tools/validate.sh --static        # 仓库检查 + host 测试
./tools/validate.sh --firmware      # 固件构建 + 保护分区验证
```

纯逻辑模块（卡尔曼滤波、扫盘峰值检测）是无 ESP-IDF 依赖的 C 代码，可在 PC 上
直接编译运行测试。UI 和 ESP-NOW 集成需要真机验证。

## 与官方固件的关系

本仓库基于 [FoloToy/ai-passport](https://github.com/FoloToy/ai-passport)
（`f75873f`，MIT）。`factory`/`cardid` 分区布局、`verify_firmware.py` 和 BSP
与基线字节兼容。移除了官方演示页面，关闭了 BLE。本项目为非官方项目，与
FoloToy 无关联。
