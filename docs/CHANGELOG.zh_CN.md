# 更新日志

[English](CHANGELOG.md) | 简体中文

## 未发布

### 新增
- 独立安装模式上行服务（方案 A 独立通道）：`radar_upload.c` 在 Wi-Fi SoftAP 上挂迷你 HTTP 服务（开放热点 `passradar-up`，默认 `192.168.4.1`），主页面 DOWN 双击进入、同手势退出。设备提供 boot 页，加载云端托管模块 `capture-upload.js`（metapass.chuanxilu.net，与 meta-pass 手机管理同构交互），暴露 `GET /api/capture/info`（批次概览含设备哈希）与分块 `GET /api/capture/read`（4096B 块、边界校验）。手机拉取 RCB1 原始区后代传 `POST /api/capture/ingest`——与 meta-pass 子固件通道汇入同一 ingest → R2 路径。退出时恢复 FTM SoftAP 配置。另有 CLI 兜底 `tools/capture_upload.py`：esptool 读片上传，可持本地 cardid 核对设备哈希
- 批次头 v6：头部 reserved 前 16B 改为 `device_hash[16]`——`SHA-256(cardid 前 16B)` 截断 16 字节，由 `radar_capture_device_hash()` 在 `begin()` 盖章（按分区 label `cardid` + `SUBTYPE_ANY` 解析，独立安装 partitions.csv 与子固件 carve 表同 label 不同 subtype 均命中，同一代码路径）。cardid 明文不出设备；持 cardid 者在本地重算哈希即可核对数据集来自哪台设备。无 cardid（未 provisioned 开发板）时盖确定性的全擦除输入哈希。新增公共访问器 `radar_capture_batch_data_size()` / `radar_capture_batch_data_read()`（边界校验的原始区读取，供上行服务用）

## v0.2.0 (2026-10-21)

### 修复
- 采集不改上下文直接 START 时 SAVE 必败 `CAPTURE_SAVE FAIL BATCH err=ESP_ERR_INVALID_STATE`：上电默认房间大小是 `LARGE`，而默认空间是 `OUTDOOR_OPEN`——采集矩阵规定户外开阔地只允许 `NOT_APPLICABLE`，batch append 守卫在完整 43 秒跑完后才拒绝（菜单选项有可用性过滤、选不出该组合，只有未初始化的默认值会踩）。默认值改为 `NOT_APPLICABLE`，与默认室外场景自洽（2026-10-21 真机定位根因）
- meta-pass v2（dynslot）启动器下批采集存储不可用：固件写死查找 v1 固定
  `ota_2` 槽，动态槽位表上永远解析不到（`BATCH data unavailable: slot2 is
  missing`），采集数据存不下来。批次数据区改为按优先级发现：子固件声明的
  `capture` DATA carve（subtype `0x81`，partitions.csv 声明 1 MiB，安装时
  用户可扩大），由启动器为 active play 物化、按标签解析；回退到 v1 空
  `ota_2` 槽。容量运行时按分区推导，`begin()` 把请求批次量钳制到容量
  （1 MiB carve = 85 个最坏情况 Capture；片上 RCB1/RCC1/RCOK 格式不变）
- 真机首轮问题:10 米内扫盘报 OUT OF RANGE（100ms 跳频 + BLE 扫描抢射频）
  ——跳频驻留提高到 250ms，扫盘/标定期间暂停 BLE 扫描
- FTM 建立不了会话：连续发起失败时暂停 BLE 扫描 5 秒让路，成功或超时后恢复
- 诊断页 UP 长按进不去（ADC 键盘长按被误读为 OK）：入口改为 UP 双击；
  追踪页距离改为 IIR 平滑；诊断页显示最近一次 FTM 失败原因码

### 新增
- `tools/build-firmware.sh`:一条命令完成交付构建，产出唯一发布单文件
  `pass-radar_<版本>.bin`（可引导本体 + 44B MPUPV2 指纹尾段，与 meta-pass
  同一契约）并校验保护布局
- 旋转速度自校正：谐波拟合周期搜索估计实际旋转完成时刻，剔除提前停转后
  的尾部样本并重标定时间→角度映射（含 3s 提前停转 host 测试）
- 诊断/回归页（追踪页上键长按）：v1 纯 RSSI 距离 vs v2 融合距离
  （A/B 基线）、FTM 质量、BLE 哨兵、环境 n 与标定参考、最近投票分歧
- 双端同时扫盘：对端开始扫盘且本端空闲时自动跟随（按 sweep_id 一次性
  触发，落后 ≤1s），结果用环形均值融合双端角度，分歧计入上报 spread
- 结果页低置信度扇形：用 ±spread 弧段标注方向不确定区域；距离误差档
  （HIGH ±0.5m / MED ±2m）显示在结果页与主页面
- 主页面置信度门控：最近信道投票分歧 > 20° 时 HIGH 封顶为 MED
- BLE 哨兵远/近分级：哨兵 RSSI > -75dBm 显示 "FAR NEAR (BLE)"，否则
  "FAR (BLE link)"（阈值待真机标定）
- 环境估计：长期弱信号（累计均值 < -80dBm）选中遮挡档（n=3.5），
  与室内抖动规则互补
- Wi-Fi FTM（802.11mc RTT）测距：对称 SoftAP responder + 周期性发起
  （每 2s 一次、16 burst），逐 burst 距离中值滤波
- BLE 5.0 Coded PHY 长距哨兵链路（NimBLE，1s 间隔扩展广播 + 稀疏被动
  扫描）：Wi-Fi 超界后显示"FAR (BLE link)"
- 融合引擎（`radar_fusion`）：双向 RSSI 对称平均、FTM/RSSI 加权融合与
  反射径判别、置信度分级（HIGH/MED/LOW/OOR）、室内外路径损耗自适应、
  环形角度统计
- 信道分集扫盘：1/6/11 快跳（100ms 驻留），每信道一次谐波最小二乘
  拟合测角 + 环形中值投票
- 抛物线插值峰值角度（亚采样分辨率）与峰显著性门限
- 1m 双卡标定（主页面 OK 长按，3s 采样，NVS 持久化）
- 结果页显示角度分歧（±）与置信度，低置信度结果以警示色标出
- ESP-NOW 协议 v2：双向 RSSI 回传、扫盘同步标志（对端跟随跳频）、
  结果共享（向后兼容 v1）
- 融合引擎与新扫盘算法的 host 测试

### 变更
- Wi-Fi 模式 STA → APSTA（FTM responder 需要）
- 固定主信道为信道 1；扫盘期间跳频 1/6/11，结束后恢复
- 重新启用 BLE（NimBLE），启用 Wi-Fi/BLE 软件共存

## v0.1.0（2026-09-13）

### 新增
- 卡尔曼滤波 RSSI 追踪 + 极坐标雷达 UI
- 360° 定向扫盘（人体屏蔽峰值检测，4 秒引导旋转）
- 自由空间路径损耗距离估算（户外 n=2.2）
- ESP-NOW 广播收发（20Hz），最大发射功率 19.5dBm
- 双角色固件：同一固件两台设备互发互收，自动配对
- 弱信号保护（RSSI < -88dBm 显示"超出范围"）
- 三页面 UI：追踪 / 扫盘 / 结果
- meta-pass 子固件适配（mark_valid 常驻、LONG2 返回启动器）
- BSP 按键 LONG2 事件（3000ms）
- 卡尔曼滤波与扫盘算法 host 测试

### 修复（模拟器测试）
- LVGL 内存耗尽：雷达圆环改用单一边框圆对象（3 个）替代 135 个点对象；
  LVGL 内存池增至 32KB
- LVGL 不支持 float printf：距离显示改为整数分米格式
- UI 初始化顺序：先构建显示再初始化 ESP-NOW，支持优雅降级
- 坐标系统一：所有雷达点使用面板相对坐标
