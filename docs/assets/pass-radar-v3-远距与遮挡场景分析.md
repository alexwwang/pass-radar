# pass-radar v3：15~100 m 远距与遮挡（树丛、围墙）场景分析

> 目标读者：pass-radar 维护者与实现 `radar_obstruct` 模块的工程师
> 日期：2026-09-19
> 配套：[精度目标评估](pass-radar-定向测距精度目标评估与v3设计方案.md)、[v3 详细设计（一期）](pass-radar-v3-详细设计与实现指南.md)、**[v3.1 远距与遮挡实施指南（二期，本文的实现级文档）](pass-radar-v3.1-远距与遮挡实施指南.md)**、[测试设计与用例](pass-radar-v3-测试设计与用例.md)
> 阶段：本文描述的方案作为**二期（v3.1）**实现，在一期 v3（≤ 15 m）M3 验收后启动；一期只预留接口字段。
> 问题：两卡相距 15~100 m，中间有树丛、围墙、金属栏等阻挡时会发生什么？算法能否发现并修正？

---

## 0. 结论摘要

1. **15~100 m 本身就是"RSSI 单观测量"区间**：FTM 需要对端 RSSI ≥ −70 dBm，按本机标定（−42 dBm@1 m，n=2.2）折算约 18 m；再远 FTM 成功率陡降，只能作为低频"探针"。因此远距测距精度天花板是 RSSI 模型的精度：**LOS 下 ±15%~30%，有遮挡时无法从 RSSI 单独分离"远"与"被挡"**。
2. **两条被 v1/v2 忽略的物理事实决定远距行为**：
   - **两射线地面反射折点**：双方持卡高 1.2 m 时折点 ≈ 45 m，之后路径损耗指数从 ~2.2 变为 ~4。用单一 n=2.2 外推会把 80 m 算成 200 m+；v3 改分段模型后可纠正。
   - **v1 的 −88 dBm "有效阈值"人为封顶量程**：1 Mbps ESP-NOW 灵敏度约 −98 dBm，−88 dBm 折点后对应约 78 m，即使包还在收也被判 OOR。v3 以"是否收到包"判失联，RSSI 低只降低等级。
3. **遮挡的量级**（2.4 GHz）：灌木/树丛 0.5~2 dB/m；砖墙 10~20 dB；混凝土 12~25 dB；木栅栏 3~7 dB；金属网栏 10~20 dB+（网孔 <λ/2 时接近全反射）；越过 2 m 围墙的刀刃绕射额外 9~15 dB；人体 6~15 dB。对 RSSI 测距，**每 10 dB 额外损耗 ≈ 距离读数 ×2.85（折点前）或 ×1.8（折点后）**。
4. **算法能发现什么**：利用 5 个机上可算的特征——FTM/RSSI 超额损耗、Rician K 因子（快衰落深度）、1 s/10 s 双时标 RSSI 起伏、CSI 频率选择性、双端扫盘镜像一致性——可把链路分为 **真远 LOS / 静态衰减遮挡（墙）/ 动态散射遮挡（风中树丛）/ 反射路径** 四类，识别可靠度依次：树丛（高）> 墙（中，需 FTM 探针或步行测试）> 反射（中）> 真远（需排除前三者）。
5. **算法能修正什么**：
   - 树丛：用 RSSI **上分位（P80）** 代替均值（衰落只往下打），可去掉 3~6 dB 的衰落偏差；距离仍偏远但离谱率大幅下降。
   - 墙/栏：**无法**从 RSSI 单独恢复真距离；正确做法是输出 **上界**（"不超过 X m"）+ 遮挡标记，并在 ≤25 m 时用 FTM 探针给出真值；同时提示用户"举高/侧移"。
   - 反射：镜像不一致或步行测试失败 → 方向改扇区，距离标"存疑"。
   - 全部场景：**举高卡片到 2 m** 把折点从 45 m 推到 130 m，并越过 ≤2 m 的围墙/灌木顶部——这是本硬件上最有效的"修正"，靠 UI 提示由用户完成。
6. **15~100 m 的诚实目标**：LOS 距离 p50 ≤25%、方向 p50 ≤20°；树丛遮挡下距离 p50 ≤40%、方向扇区 ±45°；围墙/金属栏下只承诺"上界包含真值 ≥90%" 与扇区方向。

---

## 1. 远距链路预算（无遮挡）

以标定值 −42 dBm@1 m、n=2.2、折点 45 m、折点后 n=4 计算（两卡均胸前持握、高 1.2 m）：

| 距离 | 预期 RSSI | 距 1 Mbps 灵敏度(−98) 余量 | FTM 可用？ | v1/v2 判定 |
| --- | --- | --- | --- | --- |
| 15 m | −68 | 30 dB | 是（边缘） | 正常 |
| 20 m | −71 | 27 dB | 门控关闭（<−72） | 正常 |
| 30 m | −74.5 | 23.5 dB | 否 | 正常 |
| 45 m（折点） | −78.4 | 19.6 dB | 否 | 正常 |
| 60 m | −83.4 | 14.6 dB | 否 | 正常 |
| 78 m | −88 | 10 dB | 否 | **OOR（阈值）** |
| 100 m | −92.3 | 5.7 dB | 否 | OOR |
| 100 m，双方举高到 2 m（折点 130 m） | −86 | 12 dB | 否 | — |

推论：

- **量程**：1 Mbps 下 LOS 100 m 是可达但边缘（5~6 dB 余量，衰落时丢包 20%~50%）；举高后余量翻倍。必须显式把 ESP-NOW 速率固定为 `WIFI_PHY_RATE_1M_L`（见实现指南 §3.2.5）；若默认速率被改成 11n MCS0（−93 dBm），100 m 会直接失联。
- **BLE 哨兵**：Coded PHY S=8 灵敏度约 −103~−105 dBm，比 Wi-Fi 1 Mbps 多 5~7 dB，即 LOS 下多约 1.4 倍距离（折点后 6 dB ≈ ×1.41）。这就是 F-15 "FAR(BLE) 出现距离 ≥ OOR 距离 ×1.3" 的依据。
- **两射线折点的敏感度**：折点 `d_b = 4·h_t·h_r/λ`。h=1.2 m → 47 m；h=1.0 → 33 m；h=1.6 → 83 m；h=2.0 → 130 m。孩子/坐姿持卡时折点会缩到 30 m 以内。实现上折点常量取 45 m，但 UI 应提示"站立、胸前持卡"。

---

## 2. 遮挡物的物理效应

### 2.1 损耗量级（2.4 GHz）

| 障碍 | 额外损耗 | 机理 | 来源 |
| --- | --- | --- | --- |
| 稀疏灌木/单排树，穿透深度 d≤14 m | ≈0.57 dB/m（Weissberger：0.45·f^0.284·d，f 单位 GHz） | 吸收+散射 | Weissberger / ITU-R P.833 |
| 密叶树丛 | 1~2 dB/m；有叶比无叶高 ~20% | 同上 | ITU-R P.833 |
| 树丛 d>14 m | 1.33·f^0.284·d^0.588（20 m ≈ 7.8 dB） | 侧向绕射开始主导 | Weissberger |
| 木栅栏/木门 | 3~7 dB | 介质穿透 | ITU-R P.2040 |
| 砖墙 10~15 cm | 10~20 dB | 穿透 | ITU-R P.2040 |
| 混凝土 15~20 cm | 12~25 dB | 穿透 | ITU-R P.2040 |
| 金属网栏（网孔 5 cm < λ/2=6.2 cm） | 10~20 dB 以上；实心金属 >30 dB | 反射为主，能量走绕射 | — |
| 玻璃 | 2~4 dB（镀膜 6~10） | — | ITU-R P.2040 |
| 人体（持卡者自己或对端背身） | 6~15 dB | 吸收 | PMC6211019 |
| 越过障碍顶部的刀刃绕射 | 见 2.2 | 菲涅尔 | — |

### 2.2 越墙绕射（为什么"举高"有效）

刀刃绕射参数 `v = h·√(2(d₁+d₂)/(λ·d₁·d₂))`，h 为障碍顶超出视线的高度。损耗 `L ≈ 6.9 + 20·log10(√((v−0.1)²+1) + v − 0.1)` dB（v > −0.7）。

| 场景（λ=0.1228 m） | h | v | 额外损耗 |
| --- | --- | --- | --- |
| 2 m 围墙在 50 m 路径中点，卡高 1.2 m | 0.8 m | 0.90 | ≈13 dB |
| 同上，30 m 路径 | 0.8 m | 1.18 | ≈15 dB |
| 1.5 m 围墙，50 m 路径 | 0.3 m | 0.34 | ≈9 dB |
| 卡举高到 2.0 m（与墙顶齐平） | 0 | 0 | 6 dB |
| 卡举高到 2.3 m | −0.3 m | −0.34 | ≈3 dB |

结论：一面 2 m 砖墙对 30 m 链路的总代价是"穿透 15 dB" 与"绕射 15 dB" 两者中的较小者 ≈ 13~15 dB；举高到与墙顶齐平即可省下 7~9 dB。同理，第一菲涅尔区半径在 50 m 中点约 1.25 m，胸前持卡时**地面和低矮灌木本来就在第一菲涅尔区内**，举高同时改善两射线和绕射。

### 2.3 额外损耗对 RSSI 距离读数的影响

`d_读 / d_真 = 10^(L / (10·n))`：

| 额外损耗 L | n=2.2（折点前） | n=4（折点后） |
| --- | --- | --- |
| 3 dB | ×1.37 | ×1.19 |
| 6 dB | ×1.87 | ×1.41 |
| 10 dB | ×2.85 | ×1.78 |
| 15 dB | ×4.8 | ×2.37 |
| 20 dB | ×8.1 | ×3.16 |

示例：30 m 真距，中间一面砖墙（15 dB）→ RSSI ≈ −89.5 dBm → v1/v2 直接 OOR；v3 分段模型读作约 85 m。**没有第二观测量时，RSSI 无法区分"85 m 无遮挡"与"30 m 有墙"。**

### 2.4 对各观测量的具体影响

| 观测量 | 树丛（风中） | 砖/混凝土墙 | 金属网栏 | 说明 |
| --- | --- | --- | --- | --- |
| RSSI 均值 | 低 3~15 dB | 低 10~20 dB | 低 10~30 dB | 全部表现为"变远" |
| RSSI 1 s 内起伏（快衰落） | **大**（枝叶摆动，Rician K 低、类瑞利，起伏 8~15 dB） | 小（静态，K 高） | 中（多径反射） | 区分树丛与墙的主特征 |
| RSSI 10 s 均值漂移 | 大（风阵） | 小 | 小 | 辅助 |
| 双向 RSSI 差 | 无变化（信道互易，遮挡对两方向等效） | 同 | 同 | **对称平均消不掉遮挡**，只消天线/功率不对称 |
| FTM（若 ≤25 m 且成功） | 时延几乎不增加（穿叶直射仍是首达径），距离基本正确但成功率下降 | 穿墙直射仍首达 → 距离正确；越顶绕射路径超长仅厘米级 → 也正确 | 若能量走网孔/绕射 → 正确；若走远处建筑反射 → 偏大数十米 | FTM 是**唯一**能直接揭示"RSSI 被衰减"的观测量 |
| CSI 频率选择性（4 组能量差） | 中~高（散射多径） | 低~中 | 高（强反射） | 反射类遮挡的标志 |
| 扫盘方向图 | 幅度仍在（遮挡是接收端局部效应），但衰落周期 0.5~2 s 与 6 s 扫盘同量级 → **假峰**；CSI 4 组常不一致 | 方向图指向能量来源：越顶绕射≈真方位；绕端/穿门 → 指向缺口 | 指向缺口或反射体 | 方向误差 30°~90° 常见 |
| 双端镜像一致性 | 大体一致（前向散射） | 走同一缺口 → 一致；走反射 → 不一致 | 常不一致 | 不一致 = 反射路径证据 |
| BLE 哨兵 RSSI | 同 Wi-Fi 变化（同频段） | 同 | 同 | 无法帮助识别遮挡，只延长量程 |

---

## 3. 算法识别：机上可算的特征

全部由 `radar_obstruct.c`（纯逻辑，无 IDF 依赖）实现；输入是 TRACKING 态已有的数据流，不需要新的射频操作，除 F1 需要 FTM 探针。

### F1 超额损耗（FTM vs RSSI）

```
excess_loss_db = 10·n·log10(d_rssi / d_ftm)      // FTM 有效时；否则 NAN
```

- 含义：RSSI 比"FTM 距离应有的 RSSI"低了多少 dB，即遮挡损耗的直接估计。
- 判据：> 6 dB 判遮挡；< −3 dB（FTM 反而更远）判反射路径。
- 可用范围：≤25 m。为把范围拉到 25 m，v3 增加 **FTM 探针**：对端 RSSI 在 −72~−82 dBm 时每 5 s 尝试一次 4 帧 session（`FTM_PROBE`），成功即得到一次 F1 样本；失败不计入等级判据的成功率。

### F2 Rician K 因子（快衰落深度）

矩估计法（Greenstein），用 **原始整数 RSSI**（不能用卡尔曼后的），窗口 4 s（20 Hz × 80）：

```c
// 输入 rssi_dbm[n]，输出 K_dB；n<20 返回 NAN
float radar_obstruct_kfactor_db(const float *rssi_dbm, unsigned n) {
    double mean = 0, m2 = 0;
    for (i) { double p = pow(10.0, rssi_dbm[i] / 10.0); mean += p; m2 += p*p; }
    mean /= n; m2 /= n;
    double var = m2 - mean*mean;
    double gamma = var / (mean*mean);           // 归一化方差
    if (gamma >= 1.0) return -10.0f;             // 瑞利或更差
    double s = sqrt(1.0 - gamma);
    double K = s / (1.0 - s);
    return (float)(10.0 * log10(K + 1e-9));
}
```

经验阈值：LOS 静态 K > 6 dB；风中树丛 K < 2 dB；墙后静态 K 常仍 > 4 dB（因为主要是稳定衰减）。1 dB 量化会让 K 略偏高，阈值需外场校准（F-OBS 用例）。

### F3 双时标起伏

```
p2p_1s  = max−min(最近 1 s 原始 RSSI)         // 快衰落
p2p_10s = max−min(最近 10 s 滤波 RSSI 的 1 s 均值序列)   // 阵风/慢变
```

树丛：p2p_1s ≥ 8 且 p2p_10s ≥ 6；墙：两者都 < 5；人流：p2p_1s 大、p2p_10s 小。

### F4 CSI 频率选择性

`sel = max_g(P_g) − min_g(P_g)`（实现指南 §3.4），最近 1 s 平均。LOS < 4 dB；散射/反射 > 6~8 dB。

### F5 扫盘镜像一致性与方向图质量

- `mirror_err = |θ_local − (θ_peer + 180°)|`（环形）；> 45° → 反射路径证据。
- `n_used`（多观测量投票入选个数）与 `spread`：树丛下 CSI 4 组常有 ≥2 组不入选。
- 二次谐波能量占比（拟合 `a + b cos θ + c sin θ + d cos 2θ + e sin 2θ`，`radar_sweep_harmonic_fit` 的扩展版）：> 0.5 说明存在两个能量来源方向。

### F6 步行测试（单人可执行，用户执行）

UI 提示"朝箭头方向走 3 步（≈2 m）"。默认由操作者携带本卡、将对端卡固定不动。预期 RSSI 变化 `Δ = 10·n·log10(d/(d−2))`：真 30 m 应涨 ~0.6 dB（几乎测不出），真 10 m 应涨 ~2 dB。这个测试对远距无效，但对"RSSI 说 85 m、实际 30 m 有墙"有效：**若走 5 m 后 RSSI 变化 > 3 dB，说明真距离 < 25 m，RSSI 读数被遮挡放大**。成本低、无射频改动，单人即可完成，作为 L2 下的可选动作。

### F7 分类决策表

```c
typedef enum { OBS_NONE_LOS, OBS_FAR_LOS, OBS_ATTENUATING_STATIC, OBS_FOLIAGE_DYNAMIC,
               OBS_REFLECTION, OBS_UNKNOWN } radar_obstruct_class_t;
typedef struct {
    float excess_loss_db;   // F1, NAN=无 FTM
    float kfactor_db;       // F2
    float p2p_1s_db, p2p_10s_db;   // F3
    float csi_sel_db;       // F4, NAN=无
    float mirror_err_deg;   // F5, NAN=无双端结果
    float mean_rssi_dbm; radar_level_t level;
} radar_obstruct_features_t;
radar_obstruct_class_t radar_obstruct_classify(const radar_obstruct_features_t *f);
```

| 优先级 | 条件 | 结论 |
| --- | --- | --- |
| 1 | `excess < −3`（FTM 比 RSSI 还远）或 `mirror_err > 45` | `OBS_REFLECTION` |
| 2 | `p2p_1s ≥ 8 && (K < 2 \|\| p2p_10s ≥ 6)` | `OBS_FOLIAGE_DYNAMIC` |
| 3 | `excess > 6 && p2p_1s < 5` | `OBS_ATTENUATING_STATIC` |
| 4 | `isnan(excess) && level == L2 && p2p_1s < 5 && K > 4` | `OBS_ATTENUATING_STATIC`（疑似，置信度低：远处静态但 FTM 全失败、RSSI 长期 < −80 却起伏很小——真远 LOS 通常伴随中等起伏） |
| 5 | `mean_rssi < −80 && p2p_1s < 8 && K > 4 && (isnan(sel) \|\| sel < 5)` | `OBS_FAR_LOS` |
| 6 | 其余 | `OBS_UNKNOWN`（按 L1/L2 常规处理） |

第 4 行是最弱的推断，只用来触发"举高/步行测试"提示，不用来改距离。

---

## 4. 算法修正

```c
typedef enum { HINT_NONE, HINT_RAISE_CARD, HINT_WALK_TEST, HINT_PEER_STAND_STILL, HINT_SWEEP_AGAIN } radar_hint_t;
// 在 radar_fusion_update_v3 之后调用；就地修改 result
radar_hint_t radar_obstruct_apply(radar_obstruct_class_t cls, const radar_obstruct_features_t *f,
                                  const float *rssi_10s, unsigned n, float tx_ref, radar_env_t env,
                                  radar_fusion_result_t *r, float *sweep_spread_deg);
```

| 修正 | 适用类别 | 做法 | 效果 / 局限 |
| --- | --- | --- | --- |
| **C1 两射线分段模型** | 所有 LOS ≥45 m | 实现指南 §3.6.2 | 消除折点后 ×2~×4 的系统性高估；室内不启用 |
| **C2 量程解封** | 所有 | OOR 只由"5 s 无包"触发；RSSI < −88 仅使等级 → L2 并显示区间上界 | 量程 78 m → ~100 m |
| **C3 上分位 RSSI** | `FOLIAGE_DYNAMIC` | 用最近 10 s 原始 RSSI 的 **P80** 代入路径损耗（衰落只向下，P80 ≈ 无衰落 LOS 分量）；区间因子 e=0.5 | 去掉 3~6 dB 衰落偏差；穿叶吸收（均值损耗）仍在 → 仍偏远 1.3~2× |
| **C4 FTM 优先 + 遮挡标记** | `ATTENUATING_STATIC` 且 FTM 有效 | `d = d_ftm`；`obstructed=true`；RSSI 只用于区间下界 | ≤25 m 内可给出真距离 ±1~2 m |
| **C5 上界输出** | `ATTENUATING_STATIC`/`UNKNOWN` 且 FTM 无效 | `upper_bound_only=true`，显示 `≤ hi m`，`hi = d_rssi·(1+e)` | 不再给错数；"上界包含真值"是可验证承诺 |
| **C6 反射处理** | `REFLECTION` | 方向改扇区（spread=90）；距离取 `min(d_ftm, d_rssi)` 并标 `?`；提示再扫一次 | 反射几何未知，不试图求解 |
| **C7 举高提示** | 除 `NONE_LOS` 外全部，且 mean_rssi < −75 | `HINT_RAISE_CARD`，UI 显示"举高卡片过头顶 3 s"；同时把折点常量临时切换为 130 m 版本（h=2 m）用于该 3 s 的读数 | 越过 ≤2 m 障碍顶、折点外推 3 倍；实测预期 RSSI +3~+9 dB |
| **C8 步行测试** | `ATTENUATING_STATIC`(疑似) / `UNKNOWN` 且 L2 | `HINT_WALK_TEST`，5 m 后 ΔRSSI > 3 dB → 把上界收紧为 25 m | 廉价的"是否真远"判别 |
| **C9 重复扫盘共识** | `FOLIAGE_DYNAMIC` 下的定向 | 连续两次扫盘环形差 ≤ 30° 才给射线；否则扇区 + `HINT_SWEEP_AGAIN` | 抑制衰落假峰 |
| **C10 对端静止请求** | HOMING、远距扫盘 | `RADAR_V3_FLAG_HOMING` 让对端屏幕显示"请站着别动" | 消除对端移动/转身造成的 6~15 dB 起伏 |

融合顺序：`radar_fusion_update_v3` → `radar_obstruct_classify` → `radar_obstruct_apply` → UI。等级 L0~L3 与遮挡类别是两个正交标签：等级控制显示形式（点/区间/上界/扇区），类别控制修正与提示文案。

---

## 5. 各场景预期表现（15~100 m）

| 场景 | 距离（p50 / p90） | 方向 | UI 形式 | 关键依赖 |
| --- | --- | --- | --- | --- |
| LOS 15~45 m，胸前持卡 | ±15% / ±30% | p50 ≤20°，p90 ≤35° | 点值 + 区间，L0/L1 | 标定、两射线不触发 |
| LOS 45~100 m | ±25% / ±50% | p50 ≤25° | 区间，L1 | C1 分段模型、1 Mbps、C2 |
| LOS 45~100 m，举高 | ±20% / ±40% | 同上 | 同上 | C7 |
| 树丛 3~10 m 深，30 m | 偏远 1.3~2×，P80 后 p50 ≤40% | 扇区 ±45°；两次一致时射线 ±30° | 区间 e=0.5 + `FOLIAGE?` | C3、C9、F2/F3 |
| 砖墙/围墙，≤25 m | FTM 探针成功时 ±1~2 m；否则上界 | 扇区（可能指向缺口） | `<= X m BLOCKED?` 或 FTM 点值 | C4/C5、FTM 探针 |
| 砖墙/围墙，25~100 m | 只有上界（真值 ≤ 上界 ≥90%） | 扇区 | `≤ X m` + 举高/步行提示 | C5、C7、C8 |
| 金属栏/实心金属 | 上界或 OOR；可能反射 | 扇区 | `≤ X m ?` | C6 |
| 人群/移动遮挡 | ±40%，起伏大 | 扇区 | 区间 | F3 识别为动态 |

与"±15° / ±1 m"目标的关系：**15 m 以外任何场景都不满足 ±1 m**，这是 RSSI 物理极限（评估文档 §3.1），不是实现问题；方向在 LOS 远距可勉强守住 p50 ≤20°。

---

## 6. 实现落点（二期 v3.1；逐函数代码与实施顺序见 [v3.1 实施指南](pass-radar-v3.1-远距与遮挡实施指南.md)）

| 文件 | 内容 |
| --- | --- |
| 新增 `main/radar_obstruct.c/.h` | F1~F5 特征计算（`radar_obstruct_kfactor_db`、`radar_obstruct_excess_loss_db`、`radar_obstruct_upper_quantile`）、F7 分类 `radar_obstruct_classify`、§4 修正 `radar_obstruct_apply`（纯逻辑） |
| `main/radar_ftm.c` | FTM 探针模式（−72~−82 dBm，每 5 s，4 帧），成功率单独计数 `DIAG_CNT_FTM_PROBE_OK` |
| `main/radar_fusion.c` | 两射线分段；OOR 判定改为"无包 5 s"；`RADAR_RSSI_MIN_VALID` 仅用于等级不用于 OOR |
| `main/radar_espnow.c` | 原始 RSSI 环形缓冲（10 s × 20 Hz = 200 × 1 B）供 F2/F3；`WIFI_PHY_RATE_1M_L` |
| `main/radar_sweep.c` | 二次谐波拟合扩展（F5） |
| `main/radar_ui.c` | 遮挡类别标签（英文：`FOLIAGE?`/`BLOCKED?`/`REFLECT?`/`FAR`）、提示行（`RAISE CARD 3s`/`WALK 5m THEN DOWN`/`SWEEP AGAIN`/`PEER: HOLD STILL`）、`<=` 上界样式 |
| `tests/test_radar_obstruct.c` | U-OBS-01~12（测试文档 §2.6） |

参数（首版，需外场校准）：`OBS_EXCESS_MIN_DB 6`、`OBS_REFLECT_EXCESS_DB −3`、`OBS_K_FOLIAGE_DB 2`、`OBS_K_STATIC_DB 4`、`OBS_P2P1S_DYNAMIC_DB 8`、`OBS_P2P1S_STATIC_DB 5`、`OBS_P2P10S_DYNAMIC_DB 6`、`OBS_UPPER_QUANTILE 0.8`、`OBS_RAISE_HINT_RSSI −75`、`FTM_PROBE_MIN_RSSI −82`、`FTM_PROBE_PERIOD_MS 5000`。

---

## 7. 外场验证用例（对应测试文档 F-11~F-14）

统一记录模板见测试文档 §6.3；每个测点 ≥20 次读数，两位测试者。

### F-OBS-1 树丛（F-11）

- 场地：公园，两卡 30 m，中间一排 3~5 m 厚、高于 1.5 m 的绿篱/灌木；记录风力（Beaufort）。
- 步骤：无风/有风各测一组；先胸前持卡 60 s，再按提示举高 3 s × 5 次。
- 期望：`FOLIAGE_DYNAMIC` 识别率 ≥70%（有风）；P80 修正后距离 p50 误差 < 均值法；举高后 RSSI 提升 ≥3 dB 的比例 >50%；扫盘两次一致率与显示形式记录。
- 记录：K、p2p_1s/10s、sel、类别、修正前后距离。

### F-OBS-2 砖墙/围墙（F-12）

- 场地：2 m 左右砖墙或混凝土围墙，两卡各距墙 10~15 m（总 20~30 m），无门洞；再测总距 50 m。
- 步骤：静止 60 s；触发步行测试；举高测试。
- 期望：≤25 m 时 FTM 探针成功率 ≥20%，成功时 `ATTENUATING_STATIC` 且距离误差 ≤3 m；不成功时显示 `≤ X m` 且 X ≥ 真值（覆盖率 ≥90%）；步行 5 m 后上界收紧；方向以扇区显示，记录是否指向墙的缺口。

### F-OBS-3 金属网栏（F-13）

- 场地：球场铁丝网/停车场金属栏，两卡 30 m。
- 期望：不给点值；类别为 `REFLECTION`/`ATTENUATING_STATIC`/`UNKNOWN` 任一均可，但**离谱率 <15%**（以上界/扇区计）；记录 sel 与镜像误差供阈值校准。

### F-OBS-4 真远 LOS 对照（F-03 / F-14）

- 场地：空地 60/80/100 m，无遮挡。
- 期望：类别为 `OBS_NONE_LOS`/`OBS_FAR_LOS`（**不得**误判为墙或树丛 >15%）；两射线模型下距离 p50 ≤25%；100 m 不 OOR；举高后余量提升 ≥3 dB。

这组对照是阈值校准的关键：先用 F-OBS-4 与 F-OBS-1 的数据画 K / p2p_1s 的分布，再定 `OBS_K_*` 与 `OBS_P2P*` 阈值，最后用 F-OBS-2/3 验证误报率。

---

## 8. 参考资料

- 两射线地面反射模型与折点：https://en.wikipedia.org/wiki/Two-ray_ground-reflection_model
- Weissberger 植被衰减模型：https://en.wikipedia.org/wiki/Weissberger%27s_model
- ITU-R P.833 植被衰减：https://www.itu.int/dms_pubrec/itu-r/rec/p/R-REC-P.833-9-201609-S!!PDF-E.pdf
- ITU-R P.2040 建筑材料电磁特性与穿透损耗：https://www.itu.int/dms_pubrec/itu-r/rec/p/R-REC-P.2040-2-202109-S!!PDF-E.pdf
- 单刀刃绕射模型：https://www.gaussianwaves.com/2019/04/modeling-diffraction-loss-single-knife-edge-diffraction-model/
- 人体遮挡对 2.4 GHz 的影响：https://pmc.ncbi.nlm.nih.gov/articles/PMC6211019/
- Wi-Fi RTT 测距测试与 NLOS 表现：https://pmc.ncbi.nlm.nih.gov/articles/PMC10007519/
- FUSIC：多径下的 Wi-Fi 测距（RTT/RSSI 一致性等 NLOS 特征）：https://kjiokeng.github.io/assets/pdf/papers/fusic-infocom20.pdf
- ESP32-C3 数据手册（接收灵敏度：11b 1 Mbps、11n MCS0、LE Coded）：https://www.espressif.com/sites/default/files/documentation/esp32-c3_datasheet_en.pdf
- ESP-IDF ESP-NOW 速率配置（`esp_now_set_peer_rate_config`）：https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-reference/network/esp_now.html
- ESP-IDF FTM 示例（−70 dBm 建议、偏移标定）：https://github.com/espressif/esp-idf/blob/master/examples/wifi/ftm/README.md
- Rician K 因子矩估计（Greenstein et al., 1999）："Moment-method estimation of the Ricean K-factor", IEEE Communications Letters
