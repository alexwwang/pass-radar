#pragma once

// Run 级 FTM 原始 session 聚合（FORMAT_VERSION 4，§10 of the model
// parameter-update program）。capture 的一个 run 内，把每个成功 session
// 的原始 median（offset 修正与 P25 之前）按 session id 去重后聚合，
// 供 PC 端拟合 P1：b0_run = ftm_raw_median_m − target_m。
//
// 纯 C 模块，无 IDF 依赖，host 测试直接编译。

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RADAR_CAPTURE_AGG_MAX_SESSIONS 16u

typedef struct {
    // 聚合结果（写入 capture header / META 行）。
    // ftm_raw_median_m：各 session 原始 median 的 median。
    // ftm_raw_iqr_m：session median 的 IQR。
    // sessions：完成聚合的 session 数；0 = 本 run 无原始数据。
    // bursts：有效 burst 总数（PC 端加权用）。
    float ftm_raw_median_m;
    float ftm_raw_iqr_m;
    uint16_t ftm_raw_sessions;
    uint16_t ftm_raw_bursts;

    // 内部状态（勿直接读写）。
    float medians[RADAR_CAPTURE_AGG_MAX_SESSIONS];
    uint16_t bursts[RADAR_CAPTURE_AGG_MAX_SESSIONS];
    unsigned count;
    unsigned last_id;
} radar_capture_session_agg_t;

// 复位聚合（capture start / init 时调用）。
void radar_capture_session_agg_reset(radar_capture_session_agg_t *agg);

// 以 session id 去重追加一个完成的原始 session。
// id == 0 视为无数据忽略；重复 id 忽略（tick 轮询同一快照）。
void radar_capture_session_agg_add(radar_capture_session_agg_t *agg,
                                   unsigned session_id, float raw_median_m,
                                   uint8_t raw_bursts);

// 重算 median/IQR/sessions/bursts 并返回 sessions 是否 > 0。
// 数值无变化时跳过重算；奇偶个数的 median 与 n=0..4 的 IQR 均有 host 测试。
bool radar_capture_session_agg_refresh(radar_capture_session_agg_t *agg);
