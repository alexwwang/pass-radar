# 数据采集图文教程

[English](capture-walkthrough.md) | 简体中文

一次采集 = 在固定环境参数下,以 250ms 间隔自动采满 **120 条**测距记录(约 30 秒),写入当前批次。批次内可无限次采集,唯一限制是存储空间。

## 0. 准备

需要两张都刷有 pass-radar 固件的 AI Passport:

- **雷达主机**:固定在桌上,全程不动
- **目标卡片**:由你拿着,放到指定距离和方位

## 1. 进入采集菜单

主页面 **UP 双击** → 诊断页 → **OK 单击** → 采集菜单。

![主页面](assets/capture/shot-1-main.png)

## 2. 设置环境参数(菜单第 1 页)

![采集菜单第 1 页](assets/capture/shot-3-menu-p1.png)

| 选项 | 怎么选 |
|---|---|
| ENV | 室内 INDOOR / 室外 OUTDOOR(OK 单击切换) |
| LINK | 无遮挡 LOS / 静态遮挡 STATIC / 动态遮挡 DYNAMIC / 多径 MULTIPATH |
| DIST | 目标距离,1~30m 共 8 档 |
| SPACE ~ MATERIAL | 照实描述现场,OK 进编辑器后 UP/DOWN 选、OK 确认 |

编辑器示例(SPACE):

![SPACE 编辑器](assets/capture/shot-6-space-editor.png)

部分选项值受 环境×链路 组合约束,非法值会自动跳过,属正常现象。

## 3. 设置目标方位(菜单第 2 页)

DOWN 翻到第 2 页:

![采集菜单第 2 页](assets/capture/shot-4-menu-p2.png)

- **HEIGHT / POSE**:目标卡片举多高、平放还是竖立
- **BEARING**:目标卡片在主机的哪一侧(8 方位)——**必须设置,否则无法开始**

## 4. 开始采集

选中 **START CAPTURE**,按 OK,把目标卡片放到选定的距离方位并保持不动。约 30 秒后自动完成并保存,屏幕回到菜单,底部 COUNT 加一。

同一环境建议采 3~5 次,每次可换 DIST / BEARING / HEIGHT。

## 5. 采错了怎么办

- **DROP LAST**:只删最近一次,之前的保留;可反复用
- **NEW BATCH**:清空整个批次重来(谨慎)

## 6. 上传

主页面 **DOWN 双击** → 设备自动签名并打开热点 → 手机连热点,浏览器自动打开上传页 → 点上传,等成功提示。再 DOWN 双击退出。

![扫描页](assets/capture/shot-8-sweep.png)

> 提示:采集数据只存在主机里,采完请尽快上传;游戏与采集本身不需要配网。

## 最小任务清单

1. 室内 + LOS:采 3~5 次
2. 室外 + LOS:采 3~5 次
3. DOWN 双击上传

约 15 分钟完成。感谢贡献数据!
