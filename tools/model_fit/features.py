"""Feature replay of the C modules ``radar_obstruct.c`` / ``radar_fusion.c``.

Pure-stdlib port (no numpy): every function mirrors its C counterpart 1:1,
including NaN conventions, index arithmetic and input caps. The parity test
(``tests/test_features_parity.c`` + ``tests/test_features_parity.py``) feeds
identical synthetic vectors to the C binaries and to this module and requires
the outputs to agree within 1e-4.
"""

from __future__ import annotations

import math
from typing import Sequence

NAN = float("nan")

# radar_fusion.c / radar_fusion.h constants.
RSSI_PRESENT_TH = -126.0
RSSI_ABSENT = -127.0
DIST_INVALID = -1.0
TWO_RAY_BREAK_M = 45.0
FAR_N = 4.0
FTM_MAX_BURST = 32
FTM_SUSPICIOUS_RATIO = 1.8
FTM_SUSPICIOUS_MARGIN_M = 2.0
FTM_ABS_SIGMA_M = 0.9
RSSI_RANDOM_REL = 0.12
RSSI_NSLOPE_REL = 0.0909

ENV_OUTDOOR = 0
ENV_INDOOR = 1
ENV_OBSTRUCTED = 2


def path_loss_n(env: int) -> float:
    """radar_env_path_loss_n(): indoor 3.0, obstructed 3.5, outdoor 2.2."""
    if env == ENV_INDOOR:
        return 3.0
    if env == ENV_OBSTRUCTED:
        return 3.5
    return 2.2


# ---- radar_fusion.c ----


def sym_valid(rssi_ab: float, rssi_ba: float) -> bool:
    return (
        math.isfinite(rssi_ab)
        and math.isfinite(rssi_ba)
        and rssi_ab > RSSI_PRESENT_TH
        and rssi_ab <= 20.0
        and rssi_ba > RSSI_PRESENT_TH
        and rssi_ba <= 20.0
    )


def sym_rssi(rssi_ab: float, rssi_ba: float) -> float:
    if not sym_valid(rssi_ab, rssi_ba):
        return RSSI_ABSENT
    return 0.5 * (rssi_ab + rssi_ba)


def sym_distance(
    rssi_sym: float,
    tx_ref_sym: float,
    env: int,
    two_ray_break_m: float = TWO_RAY_BREAK_M,
) -> float:
    """radar_fusion_sym_distance()."""
    if (
        not math.isfinite(rssi_sym)
        or not math.isfinite(tx_ref_sym)
        or rssi_sym <= RSSI_PRESENT_TH
        or tx_ref_sym < -100.0
        or tx_ref_sym > 20.0
    ):
        return DIST_INVALID
    n = path_loss_n(env)
    near_distance = 10.0 ** ((tx_ref_sym - rssi_sym) / (10.0 * n))
    if not math.isfinite(near_distance) or near_distance <= 0.0:
        return DIST_INVALID
    if env != ENV_OUTDOOR or near_distance <= two_ray_break_m:
        return near_distance
    rssi_break = tx_ref_sym - 10.0 * n * math.log10(two_ray_break_m)
    far_distance = two_ray_break_m * 10.0 ** ((rssi_break - rssi_sym) / (10.0 * FAR_N))
    if math.isfinite(far_distance) and far_distance > 0.0:
        return far_distance
    return DIST_INVALID


def ftm_filter(raw_m: Sequence[float]) -> tuple:
    """radar_fusion_ftm_filter(): returns (median_m, iqr_spread_m)."""
    values = []
    n = min(len(raw_m), FTM_MAX_BURST)
    for i in range(n):
        v = raw_m[i]
        if math.isfinite(v) and 0.0 < v <= 1000.0:
            values.append(v)
    if not values:
        return DIST_INVALID, NAN
    values.sort()
    n = len(values)
    median = (
        values[n // 2]
        if n & 1
        else 0.5 * (values[n // 2 - 1] + values[n // 2])
    )
    q1 = n // 4
    q3 = (3 * n) // 4
    if q3 >= n:
        q3 = n - 1
    return median, values[q3] - values[q1]


def ftm_suspicious(ftm_m: float, rssi_m: float) -> bool:
    return (
        ftm_m > rssi_m * FTM_SUSPICIOUS_RATIO
        and ftm_m > rssi_m + FTM_SUSPICIOUS_MARGIN_M
    )


def ftm_weight(dist_m: float) -> float:
    """radar_fusion_ftm_weight(): inverse-variance FTM weight (D8)."""
    if not math.isfinite(dist_m) or dist_m <= 0.05:
        return 0.5
    d = max(dist_m, 0.5)
    sigma_ftm = FTM_ABS_SIGMA_M / d
    slope = RSSI_NSLOPE_REL * math.log(d)
    sigma_rssi = math.sqrt(RSSI_RANDOM_REL * RSSI_RANDOM_REL + slope * slope)
    v_ftm = sigma_ftm * sigma_ftm
    v_rssi = sigma_rssi * sigma_rssi
    return v_rssi / (v_ftm + v_rssi)


def calib_ref(sym_rssi_at_1m: float) -> float:
    """radar_fusion_calib_ref(): clamp to [-60, -30] dBm."""
    if sym_rssi_at_1m > -30.0:
        return -30.0
    if sym_rssi_at_1m < -60.0:
        return -60.0
    return sym_rssi_at_1m


def ftm_offset_from_calib(session_medians_m: Sequence[float]) -> float:
    """radar_fusion_ftm_offset_from_calib(): median - 1.0 clamped to [-2, 15]."""
    if session_medians_m is None or len(session_medians_m) < 3:
        return NAN
    values = []
    for v in session_medians_m[:32]:
        if math.isfinite(v) and v > 0.0:
            values.append(v)
    if len(values) < 3:
        return NAN
    values.sort()
    n = len(values)
    median = (
        values[n // 2]
        if n & 1
        else 0.5 * (values[n // 2 - 1] + values[n // 2])
    )
    offset = median - 1.0
    return min(max(offset, -2.0), 15.0)


def ftm_apply_offset(raw_m: float, offset_m: float) -> float:
    if not math.isfinite(raw_m) or not math.isfinite(offset_m):
        return NAN
    return max(raw_m - offset_m, 0.1)


def ftm_low_quantile(hist_m: Sequence[float]) -> float:
    """radar_fusion_ftm_low_quantile(): sorted[(valid-1)//4], cap 32."""
    if not hist_m:
        return DIST_INVALID
    values = [v for v in hist_m[:32] if math.isfinite(v) and v > 0.0]
    if not values:
        return DIST_INVALID
    values.sort()
    return values[(len(values) - 1) // 4]


# ---- radar_obstruct.c ----


def kfactor_db(rssi_dbm: Sequence[float]) -> float:
    """radar_obstruct_kfactor_db(): n >= 20 required, NaN on non-finite."""
    if rssi_dbm is None or len(rssi_dbm) < 20:
        return NAN
    mean = 0.0
    second = 0.0
    for x in rssi_dbm:
        if not math.isfinite(x):
            return NAN
        power = 10.0 ** (x / 10.0)
        mean += power
        second += power * power
    mean /= len(rssi_dbm)
    second /= len(rssi_dbm)
    if not mean > 0.0:
        return NAN
    gamma = (second - mean * mean) / (mean * mean)
    if gamma >= 1.0:
        return -10.0
    if gamma < 0.0:
        gamma = 0.0
    s = math.sqrt(1.0 - gamma)
    k = s / (1.0 - s + 1e-9)
    return 10.0 * math.log10(k + 1e-9)


def excess_loss_db(d_rssi_m: float, d_ftm_m: float, path_loss_n_value: float) -> float:
    """radar_obstruct_excess_loss_db()."""
    if (
        not d_rssi_m > 0.0
        or not d_ftm_m > 0.0
        or not path_loss_n_value > 0.0
        or not math.isfinite(d_rssi_m)
        or not math.isfinite(d_ftm_m)
        or not math.isfinite(path_loss_n_value)
    ):
        return NAN
    return 10.0 * path_loss_n_value * math.log10(d_rssi_m / d_ftm_m)


def upper_quantile(values: Sequence[float], q: float) -> float:
    """radar_obstruct_upper_quantile(): n >= 10 required, cap 256."""
    if values is None or len(values) < 10 or not 0.0 < q < 1.0:
        return NAN
    copy = []
    for v in values[:256]:
        if not math.isfinite(v):
            return NAN
        copy.append(v)
    copy.sort()
    index = int(q * (len(copy) - 1) + 0.5)
    return copy[index]


def p2p(
    rssi_dbm: Sequence[float], timestamps_ms: Sequence[int], now_ms: int
) -> tuple:
    """radar_obstruct_p2p(): returns (p2p_1s_db, p2p_10s_db), NaN when short."""
    if rssi_dbm is None or timestamps_ms is None:
        return NAN, NAN
    min_1s = math.inf
    max_1s = -math.inf
    count_1s = 0
    sums = [0.0] * 10
    counts = [0] * 10
    for x, ts in zip(rssi_dbm, timestamps_ms):
        age = (now_ms - ts) & 0xFFFFFFFF
        if age < 1000:
            if x < min_1s:
                min_1s = x
            if x > max_1s:
                max_1s = x
            count_1s += 1
        if age < 10000:
            bucket = age // 1000
            if bucket < 10:
                sums[bucket] += x
                counts[bucket] += 1
    p1 = max_1s - min_1s if count_1s >= 5 else NAN

    min_bucket = math.inf
    max_bucket = -math.inf
    bucket_count = 0
    for i in range(10):
        if counts[i] < 3:
            continue
        mean = sums[i] / counts[i]
        if mean < min_bucket:
            min_bucket = mean
        if mean > max_bucket:
            max_bucket = mean
        bucket_count += 1
    p10 = max_bucket - min_bucket if bucket_count >= 5 else NAN
    return p1, p10


def mean_recent(
    rssi_dbm: Sequence[float],
    timestamps_ms: Sequence[int],
    now_ms: int,
    window_ms: int,
) -> float:
    """radar_obstruct_mean_recent()."""
    if rssi_dbm is None or timestamps_ms is None:
        return NAN
    total = 0.0
    count = 0
    for x, ts in zip(rssi_dbm, timestamps_ms):
        if (now_ms - ts) & 0xFFFFFFFF < window_ms and math.isfinite(x):
            total += x
            count += 1
    if count:
        return total / count
    return NAN
