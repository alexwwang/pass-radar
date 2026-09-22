# pass-radar v3 最小场景矩阵与 Capture 条件编辑器设计方案

English version: `pass-radar-v3-minimal-condition-matrix-and-capture-editor.md`

日期：2026-09-22  
状态：**设计待确认，尚未实施**  
适用范围：开发者/现场标定 Capture 功能，不影响普通用户运行路径。

## 1. 设计目标

Capture 的目的不是穷举所有真实环境，而是用尽可能少的模型场景覆盖主要无线行为，同时保留足够的上下文信息，便于电脑端解释异常和决定是否需要第二版细分模型。

必须满足：

1. 室内、室外可以在同一个批次中按任意顺序采集；
2. 不把空间形状、材料、家具、边界、姿态全部做笛卡尔积；
3. 采集者能看懂每个选项，不依赖隐藏的默认值；
4. 明显不适用的组合在界面中显示为灰色，不能确认；
5. 真实但复杂的组合不能被过度禁止；
6. 原始记录同时保存模型标签和现场上下文；
7. 采集前设置的条件和设备采集后的无线观察结果必须分开。

当前 `v0.1-33` 的 `SCENE + SPACE` 设计不满足上述要求，不得作为本方案的实现基线继续采集标定数据。

## 2. 最小模型场景数量

### 2.1 模型维度

第一版模型只使用两个正交维度：

```text
ENVIRONMENT
  INDOOR
  OUTDOOR

LINK CONDITION
  LOS
  STATIC_ATTENUATION
  DYNAMIC_ATTENUATION
  MULTIPATH
```

因此核心场景数量为：

```text
2 × 4 = 8 个核心场景
```

### 2.2 八个核心场景

| ID | 环境 | 链路条件 | ELI5 定义 |
| --- | --- | --- | --- |
| I-LOS | 室内 | LOS | 在建筑物内，主要信号可以直达，没有刻意挡住直线路径 |
| I-STATIC | 室内 | 静态衰减 | 固定墙、隔断、门等让信号稳定变弱，现场没有动态遮挡 |
| I-DYNAMIC | 室内 | 动态衰减 | 人体移动、人员经过或其他移动物体让信号随时间变化 |
| I-MULTIPATH | 室内 | 多径 | 走廊、墙角、金属/玻璃等产生明显回波或多个可能路径 |
| O-LOS | 室外 | LOS | 室外开阔区域，主要信号直达，没有刻意遮挡 |
| O-STATIC | 室外 | 静态衰减 | 围墙、固定栅栏、建筑物等使信号稳定变弱 |
| O-DYNAMIC | 室外 | 动态衰减 | 人体、树丛、树叶或移动物体使信号随时间变化 |
| O-MULTIPATH | 室外 | 多径 | 金属栏、车辆、建筑外墙等产生明显回波或反射路径 |

`FOLIAGE`、`HUMAN`、`WALL`、`METAL` 不再是独立模型场景，而是障碍物上下文。例如：

```text
OUTDOOR + DYNAMIC_ATTENUATION + FOLIAGE  => O-DYNAMIC
INDOOR + STATIC_ATTENUATION + WALL       => I-STATIC
OUTDOOR + MULTIPATH + METAL               => O-MULTIPATH
```

### 2.3 数据量估算

每个核心场景至少需要三个距离点，才能观察距离曲线而不是只得到一个偏差点。

最低可用数据集：

```text
8 个场景 × 3 个距离 × 2 次重复 = 48 次 Capture
```

推荐第一版数据集：

```text
8 个场景 × 3 个距离 × 3 次重复 = 72 次 Capture
```

建议第一轮距离使用：

```text
1 m / 3 m / 5 m
```

空间允许时，室内或室外远距场景可以增加 8 m，但不应因此增加新的模型场景类别。

48 次是最低可用规模；72 次更适合估计重复方差。该数量不包括因采集失败而被标记为 `INVALID` 的运行。

## 3. 正交维度与上下文字段

### 3.1 参与模型场景矩阵的字段

只有以下字段参与第一版核心模型分组：

```text
environment:
  INDOOR / OUTDOOR

link_condition:
  LOS / STATIC_ATTENUATION / DYNAMIC_ATTENUATION / MULTIPATH
```

### 3.2 只作为上下文保存的字段

以下字段必须保存，但不参与笛卡尔积：

```text
space_type:
  ROOM / CORRIDOR / OPEN_HALL / CORNER / OUTDOOR_OPEN / UNKNOWN

obstacle_type:
  NONE / HUMAN / FOLIAGE / WALL / PARTITION /
  METAL / GLASS / VEHICLE / UNKNOWN

material:
  SOFT / MIXED / HARD / METAL_GLASS / UNKNOWN

furniture_density:
  LOW / MEDIUM / HIGH / UNKNOWN

boundary:
  OPEN / WALL_NEAR / CORNER / DOORWAY / PARTITION / UNKNOWN

card_height:
  LOW / CHEST / OVERHEAD / UNKNOWN

card_pose:
  UPRIGHT / FLAT / MIXED / UNKNOWN
```

这些字段用于：

- 解释同一核心场景内的数据差异；
- 电脑端分析室内布局是否造成系统性偏差；
- 标记异常或不可重复的采集；
- 决定未来是否需要增加专门模型。

它们不会生成 `I-LOS-ROOM`、`I-LOS-CORRIDOR` 等新的核心模型类别。

### 3.3 设备自动观察字段

设备根据采集数据自动填写，不能由操作员修改：

```text
observed_level:
  L0_LOS / L1_INDOOR / L2_NLOS / L3_LOST

observed_obstruction:
  NONE_LOS / FAR_LOS / ATTENUATING_STATIC /
  FOLIAGE_DYNAMIC / REFLECTION / UNKNOWN
```

操作员填写的是“采集前的实际布置”；设备填写的是“采集后无线系统表现”。两者不一致时必须保留两者，不能为了让标签看起来一致而修改任一方。

## 4. ELI5 场景判断规则

### 4.1 环境

```text
建筑物内部       => INDOOR
空地、公园、道路   => OUTDOOR
半开放门廊         => 按主要空间选择，并在备注中说明半开放
```

第一版不增加第三个 `SEMI_OPEN` 模型环境，避免扩大矩阵。

### 4.2 链路条件

#### LOS

同时满足：

- 两张卡之间能看到大致直达方向；
- 两卡之间没有刻意放置的墙、人、树丛或大型物体；
- 不存在明显的强回波或反射主导现象。

墙在旁边但不挡住直达路径时，仍可以是 `LOS`，但应记录 `boundary=WALL_NEAR`。

#### STATIC_ATTENUATION

适用于固定物体挡住或明显削弱主要路径：

```text
墙、隔断、固定门、围栏、固定建筑结构
```

物体在采集期间保持不动。

#### DYNAMIC_ATTENUATION

适用于遮挡或散射随时间变化：

```text
移动的人、经过的人、随风摆动的树叶/灌木、移动车辆
```

如果人体只是静止站在直线路径上，应使用 `STATIC_ATTENUATION`，并将 `obstacle_type=HUMAN`。

#### MULTIPATH

适用于主要问题不是单纯信号变弱，而是反射路径和直达/绕行路径同时明显存在：

```text
金属栏、金属柜、玻璃、大面积墙面、墙角、狭长走廊、车辆附近
```

如果无法确认是多径还是普通衰减，可以选择最接近的条件，并将 `obstacle_type=UNKNOWN`；不能为了“看起来准确”强行选择 `MULTIPATH`。

## 5. 合法性与灰色选项规则

### 5.1 核心维度不灰显

`ENVIRONMENT` 和 `LINK CONDITION` 是核心模型维度，始终可选，不受上下文灰显规则限制：

```text
ENVIRONMENT       INDOOR / OUTDOOR
LINK CONDITION    LOS / STATIC_ATTENUATION /
                  DYNAMIC_ATTENUATION / MULTIPATH
```

例如当前环境为 `OUTDOOR` 时，`ENVIRONMENT` 仍然可以切换回 `INDOOR`；不能因为当前选择使某些上下文失效，就把核心维度本身禁用。

只有上下文字段的候选值根据核心维度和其它上下文进行灰显。用户切换核心维度后，相关上下文选项必须立即重新计算可用性。

### 5.2 基本原则

参数编辑器中每个字段的候选值都必须有状态：

```text
AVAILABLE  正常亮色，可选择
DISABLED   灰色，不可确认
WARNING    正常可选，但确认前显示警告
```

灰色选项不能通过 OK 确认，也不能通过循环选择绕过校验。

开始 Capture 前必须再次执行完整校验，即使 UI 已经禁用了非法选项。

### 5.3 直接禁用的组合

以下组合属于明显不适用，候选值必须灰显：

| 当前条件 | 灰色值 | 原因 |
| --- | --- | --- |
| `environment=OUTDOOR` | `space_type=ROOM/CORRIDOR/OPEN_HALL/CORNER` | 室外不能标记为室内空间类型 |
| `environment=INDOOR` | `space_type=OUTDOOR_OPEN` | 室内不能标记为空旷室外 |
| `environment=INDOOR` | `obstacle_type=FOLIAGE` | 第一版不把室内植物作为植被遮挡模型；如确有室内植物，使用 `UNKNOWN` 并备注 |
| `space_type=OUTDOOR_OPEN` | `furniture_density=MEDIUM/HIGH` | 室外开放区域不使用室内家具密度字段 |
| `link_condition=LOS` | `obstacle_type=WALL/PARTITION` 且墙体位于直达路径上 | 与 LOS 语义冲突 |
| `link_condition=LOS` | `obstacle_type=FOLIAGE` 且植被穿过直达路径 | 与无遮挡 LOS 语义冲突 |

### 5.4 可以选择但需要警告的组合

以下不是必然错误，因此不能直接灰掉：

```text
INDOOR + LOS + WALL
```

如果墙在路径旁边，可能仍然是 LOS。界面应提示：

```text
WARNING: WALL MAY BLOCK DIRECT PATH
CHECK WHETHER THE WALL IS BETWEEN THE CARDS
```

其他警告示例：

```text
LOS + METAL
MULTIPATH + NONE
STATIC_ATTENUATION + HUMAN
OUTDOOR + METAL_GLASS
```

这些组合可以真实存在，但开始采集前必须显示条件摘要，让操作员确认。

### 5.5 字段变更后的回退规则

如果改变一个字段后，当前其它字段变成灰色：

1. 保留用户刚刚修改的字段；
2. 找到受影响字段的第一个合法默认值；
3. 自动切换到该默认值；
4. 在页面显示一次：

```text
VALUE RESET: NOT APPLICABLE
```

不得静默保留非法旧值，也不得自动修改用户刚刚选择的核心模型条件。

## 6. Capture 界面设计

### 6.1 菜单结构

不再使用当前的 `SCENE + SPACE` 隐藏预设。新的菜单采用字段编辑器：

```text
CAPTURE MENU

> ENVIRONMENT
  LINK CONDITION
  SPACE
  OBSTACLE
  MATERIAL
  BOUNDARY
  DISTANCE
  START CAPTURE
  EXPORT BATCH
  NEW BATCH
  BATCH STATUS
  BACK
```

每个字段进入独立的候选值页面。

核心维度页面中的所有候选值都必须保持可选；上下文字段页面才显示灰色禁用值。

### 6.2 字段候选值页面

例如环境页面：

```text
ENVIRONMENT

> INDOOR
  OUTDOOR

OK SELECT
UP/DOWN MOVE
UP DOUBLE BACK
```

链路条件页面：

```text
LINK CONDITION

> LOS
  STATIC ATTEN
  DYNAMIC ATTEN
  MULTIPATH
```

空间页面根据环境动态显示：

```text
SPACE / INDOOR

> ROOM
  CORRIDOR
  OPEN_HALL
  CORNER
  OUTDOOR_OPEN   [灰色，不可选]
```

灰色候选值必须使用低亮度文本，并且光标不能停留在灰色值上；即使底层按键事件再次到达，也必须被逻辑层拒绝。

### 6.3 条件摘要页

开始采集前显示：

```text
CHECK CONDITION

CELL I-MULTIPATH
SPACE CORRIDOR
OBSTACLE NONE
MATERIAL HARD
BOUNDARY WALL_NEAR
DIST 3.0m

OK START
DOWN CANCEL
```

有警告时显示：

```text
CHECK WARNING
LOS + WALL_NEAR
VERIFY WALL IS NOT IN DIRECT PATH

OK ACCEPT
DOWN EDIT
```

存在硬冲突时不显示 `OK ACCEPT`，只显示：

```text
INVALID CONDITION
EDIT REQUIRED
DOWN BACK
```

### 6.4 固定统计区域

当前实现把菜单文本、批次数量和操作状态拼接成一段不断变长的文本，因此统计结果会追加在菜单最后一行之后。这是为了复用单个 LVGL label、避免增加控件，但会导致内容裁切、状态位置不固定，以及菜单和状态信息混在一起。

新设计不再追加动态文本，而使用固定区域：

```text
上部：条件编辑菜单
中部：当前字段值和候选值
底部第 1 行：COUNT 12  SPACE 82%
底部第 2 行：STATUS READY / SAVED / EXPORT OK
```

统计和操作状态使用独立的 LVGL label，始终占据固定位置。状态更新不得改变菜单区域的行数或布局。

### 6.5 单人操作原则

默认值固定为：

```text
card_height = CHEST
card_pose = UPRIGHT
furniture_density = UNKNOWN 或 MEDIUM
```

操作员只有在实际布置确实不同的时候才修改它们。重复 Capture 时，除距离和 run_id 外，条件必须保持一致；若换房间、换明显家具布局、换姿态或换高度，应创建新的上下文标识。

## 7. 数据记录与兼容性

每条记录必须保存：

```text
core_scene_id
environment
link_condition
space_type
obstacle_type
material
furniture_density
boundary
card_height
card_pose
observed_level
observed_obstruction
target_distance_m
run_id
room_id/layout_id（如启用）
```

旧版 `scene_id` 不能继续作为唯一语义来源。为避免把 v0.1-33 产生的模糊数据混入新模型：

- 新格式必须增加 `condition_schema_version`；
- v0.1-33 及更早数据标记为 `LEGACY_LABEL_SCHEMA`；
- 旧数据可以导出和人工复核，但不能自动转换为新八场景标签；
- 新 Capture 必须拒绝未完成的条件配置。

## 8. 验收标准

### 8.1 逻辑验收

host 测试必须覆盖：

1. 8 个核心场景均可建立；
2. 直接非法组合全部返回不可用；
3. 灰色候选值不能提交；
4. 合法的复杂组合不会被错误禁用；
5. 字段改变后失效值自动回退；
6. 开始 Capture 前完整校验再次执行；
7. 记录中的核心标签和上下文标签一致；
8. 设备观察字段不会覆盖操作员条件字段。

### 8.2 真机 UI 验收

首次刷写后只测试：

1. 单卡启动稳定；
2. 进入 `DEBUG -> CAPTURE`；
3. 依次修改 `ENVIRONMENT`、`LINK CONDITION`、`SPACE`；
4. 观察不适用值是否灰显；
5. 确认灰色值不能被 OK 选中；
6. 选择一个合法的室内条件，确认摘要完整；
7. 只做一次短 Capture；
8. 导出并检查全部条件字段。

在上述验收通过前，不执行 48/72 次正式标定矩阵。

## 9. 实施顺序

1. 将本设计与原 Capture 设计文档对齐；
2. 增加纯逻辑条件模型和可用性判断；
3. 增加 host 测试；
4. 重构 Capture UI，使候选值可单独显示和灰显；
5. 扩展记录格式和导出字段；
6. 更新中英文操作文档；
7. 运行静态检查和 host 测试；
8. 通过逻辑测试后再构建版本化正式固件；
9. 先做短流程真机验收，再开始正式采集。

本文件在用户确认前是设计草案。确认后才进入代码实施阶段。
