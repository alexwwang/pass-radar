# pass-radar 数据采集教程

一次采集 = 在固定环境参数下,以 250ms 间隔连续采满 **120 条**测距记录(约 30 秒),
采满自动收工并写入当前 batch。batch 内可无限次追加采集,唯一限制是分区物理空间
(典型 254 次,最坏载荷 127 次)。

## 进入采集菜单

主页面 **OK(单击)** → 进入采集菜单(共 2 页,UP/DOWN 翻项)。
采集菜单内 **UP 双击** 回主页面。

## 每个选项的含义

### 第 1 页:环境与场景

| 选项 | 操作 | 可选值 | 含义 |
|---|---|---|---|
| ENV | OK 单击切换 | INDOOR / OUTDOOR | 室内或室外,决定场景前缀 I- / O- |
| LINK | OK 单击切换 | LOS / STATIC_ATTEN / DYNAMIC_ATTEN / MULTIPATH | 链路状态:视距、静态遮挡衰减、动态遮挡衰减、多径反射。切换时会自动重置 SPACE 和 OBSTACLE |
| DIST | OK 单击切换 | 1 / 3 / 5 / 8 / 10 / 15 / 20 / 30 m | 采集时的目标距离(手机相对设备的距离) |
| SPACE | OK 进编辑器,UP/DOWN 选,OK 确认 | ROOM / CORRIDOR / OPEN_HALL / CORNER / STAIR / OUTDOOR_OPEN | 空间形态 |
| ROOM SIZE | 同上 | SMALL / MEDIUM / LARGE / N/A | 房间大小;OUTDOOR_OPEN 空间强制 N/A |
| OBSTACLE | 同上 | NONE / HUMAN_STATIC / HUMAN_DYNAMIC / FOLIAGE / WALL / METAL | 遮挡物类型(人体静/动、植被、墙、金属) |
| MATERIAL | 同上 | SOFT / MIXED / HARD / METAL_GLASS | 环境材质 |

### 第 2 页:卡的位置与 batch 操作

| 选项 | 可选值 | 含义 |
|---|---|---|
| FURNITURE | LOW <20% / MED 20-50% / HIGH >50% | 家具遮挡占比 |
| BOUNDARY | OPEN >2m / WALL ≤2m / CORNER ≤2m / DOORWAY / PARTITION | 周边边界 |
| HEIGHT | LOW<1.0m / CHEST 1.0-1.8m / OVERHEAD≥1.8m | 卡片(手机)架设高度 |
| POSE | UPRIGHT / FLAT / MIXED | 卡片朝向:竖立 / 平放 / 混合 |
| BEARING | FRONT / FRONT-RIGHT / RIGHT / REAR-RIGHT / REAR / REAR-LEFT / LEFT / FRONT-LEFT / UNSET | 卡片相对方位(相对按 OK 时设备顶边方向,顺时针)。**START 前必须设置**,否则拒绝启动 |
| START CAPTURE | — | 开始一次采集。采满 120 条自动写入 batch;中途异常不入库 |
| EXPORT BATCH | — | 手动重新导出最近一次已保存的采集 |
| NEW BATCH | OK → OK → 确认 | **抹掉当前 batch 全部采集**(不可恢复),开一个全新 batch |
| DROP LAST | OK → OK 确认 / DOWN 取消 | 删除**最后一次**采集,之前的保留;可反复用 |
| BATCH STATUS | — | 显示当前次数与剩余空间状态 |
| BACK | — | 返回 Debug 页 |

### 编辑器里的约束

部分选项值受 ENV/LINK/SPACE 组合约束,不可用值会自动跳过(例如 OUTDOOR 空间只允许
OUTDOOR_OPEN,LOS 链路下 METAL/WALL 遮挡不允许)。这是固件内的场景矩阵,不是故障。

## 上传

主页面 **DOWN 双击** → 进入上传模式,设备自动签名当前 batch 并开热点;
网页连热点打开 `192.168.4.1`,点上传。退出上传模式再 DOWN 双击(分裂启动模式下会重启回完整固件)。

服务器按 `capture/<设备>/<batch_id>` 存:**同一 batch 重复传 = 覆盖**,服务器永远只有最新快照;
**NEW BATCH 不影响**服务器上已传过的旧 batch,它们各自独立留存。

## 最小必要采集任务

START 的唯一硬门槛是 **BEARING 已设置**。但为了数据可用,最小任务至少覆盖:

1. **室内 + LOS**:ENV=INDOOR,LINK=LOS,SPACE=ROOM,DIST 选一个实际距离,BEARING 选一个方向 → START。一条 batch 采 3~5 次(每次可换 DIST/BEARING)。
2. **室外 + LOS**:ENV=OUTDOOR,LINK=LOS,DIST 选实际距离 → START。同上采 3~5 次。
3. **上传**:DOWN 双击 → 网页上传。

采错了不要慌:DROP LAST 删最近一次重采;全错了 NEW BATCH 重来。每次采集的环境参数
独立记录,同一次采集期间参数固定,改参数只影响下一次 START。
