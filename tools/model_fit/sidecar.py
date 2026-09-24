"""Measured-distance sidecar loading and ``DISTANCE_SIDECAR_*`` checks.

Implements the sidecar spec of the remediation design §4.1 (R-03):

- ``batch_id/run_id`` must match a parsed run uniquely
- ``measured_distance_m > 0`` and within a sane range
- ``method`` must be a controlled measurement method (tape/laser)
- every entry needs a time anchor (``capture``: export time or export
  session reference) or it is flagged ``DISTANCE_SIDECAR_NO_TIME_ANCHOR``
- the entry time window must overlap the firmware run window (export-line
  log timestamp anchor) or it is flagged ``DISTANCE_SIDECAR_MISALIGNED``
- a sidecar distance far from the Capture ``target_m`` label is flagged
  ``DISTANCE_LABEL_MISMATCH`` (warning: the label is only a preset tier)
- entries without ``uncertainty_m`` are reported but not high-precision
  eligible (warning only)

A run whose sidecar entry is rejected never silently falls back to
``truth=paced``: it is flagged SUSPECT so bad transcription is visible.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field
from typing import List, Optional, Tuple

CONTROLLED_METHODS = ("tape", "laser")
MAX_REASONABLE_DISTANCE_M = 200.0
# alignment tolerance between the entry's capture anchor and the run's
# export-line log timestamp (ms)
ALIGN_TOLERANCE_MS = 60_000
# label mismatch threshold: max(0.5 m, 25% of the target tier)
LABEL_MISMATCH_ABS_M = 0.5
LABEL_MISMATCH_REL = 0.25


@dataclass
class SidecarEntry:
    batch_id: Optional[int]
    run_id: Optional[int]
    measured_distance_m: Optional[float]
    method: str = ""
    uncertainty_m: Optional[float] = None
    bearing_deg: Optional[float] = None
    window_ms: Optional[Tuple[int, int]] = None
    capture: object = None
    measurement_session: str = ""
    errors: List[str] = field(default_factory=list)
    warnings: List[str] = field(default_factory=list)

    @property
    def accepted(self) -> bool:
        return not self.errors


def _to_float(value) -> Optional[float]:
    try:
        result = float(value)
    except (TypeError, ValueError):
        return None
    return result if math.isfinite(result) else None


def _to_int(value) -> Optional[int]:
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def load_sidecar(path) -> Tuple[dict, List[SidecarEntry]]:
    try:
        import yaml
    except ImportError as exc:  # pragma: no cover - env has PyYAML
        raise RuntimeError(
            "PyYAML is required to read sidecar files (pip install pyyaml)"
        ) from exc
    with open(path, "r", encoding="utf-8") as handle:
        data = yaml.safe_load(handle) or {}
    meta = {key: val for key, val in data.items() if key != "runs"}
    entries = []
    for item in data.get("runs") or []:
        if not isinstance(item, dict):
            continue
        window = item.get("window_ms")
        if isinstance(window, (list, tuple)) and len(window) == 2:
            window = (int(window[0]), int(window[1]))
        else:
            window = None
        entries.append(SidecarEntry(
            batch_id=_to_int(item.get("batch_id", meta.get("batch_id",
                                                           item.get("batch",
                                                                    meta.get("batch"))))),
            run_id=_to_int(item.get("run_id", item.get("run"))),
            measured_distance_m=_to_float(item.get("measured_distance_m")),
            method=str(item.get("method", "")),
            uncertainty_m=_to_float(item.get("uncertainty_m")),
            bearing_deg=_to_float(item.get("bearing_deg")),
            window_ms=window,
            capture=item.get("capture"),
            measurement_session=str(item.get("measurement_session",
                                             meta.get("measurement_session", ""))),
        ))
    return meta, entries


def _entry_checks(entry: SidecarEntry, run) -> None:
    # measured_distance_m sanity
    distance = entry.measured_distance_m
    if distance is None or distance <= 0.0 or distance > MAX_REASONABLE_DISTANCE_M:
        entry.errors.append("DISTANCE_SIDECAR_INVALID_DISTANCE")
    # controlled measurement method
    if entry.method.lower() not in CONTROLLED_METHODS:
        entry.errors.append("DISTANCE_SIDECAR_BAD_METHOD")
    # time anchor: the remediation design pins the anchor to the USB-monitor
    # timestamp of the run's export (a number). A prose/wall-clock anchor
    # cannot be verified against the log timeline (esp_timer ms since boot),
    # so it is rejected instead of silently skipping the alignment check.
    anchor = entry.capture
    anchor_num = None
    if anchor is None or (isinstance(anchor, str) and not anchor.strip()):
        entry.errors.append("DISTANCE_SIDECAR_NO_TIME_ANCHOR")
    else:
        anchor_num = _to_int(anchor)
        if anchor_num is None:
            entry.errors.append("DISTANCE_SIDECAR_UNVERIFIABLE_ANCHOR")
    # time alignment against the run's export anchor / record window
    if anchor_num is not None and run.log_ts_ms is not None:
        if abs(anchor_num - run.log_ts_ms) > ALIGN_TOLERANCE_MS:
            entry.errors.append("DISTANCE_SIDECAR_MISALIGNED")
    if entry.window_ms is not None:
        start, end = entry.window_ms
        run_span = run.elapsed_span_ms
        if end < start or start > run_span or end < 0:
            entry.errors.append("DISTANCE_SIDECAR_MISALIGNED")
    # target_m is only a preset label; large divergence is suspicious but the
    # measured value wins -> warning (DISTANCE_LABEL_MISMATCH)
    target = run.target_m
    if distance and distance > 0.0 and math.isfinite(target):
        threshold = max(LABEL_MISMATCH_ABS_M, LABEL_MISMATCH_REL * target)
        if abs(distance - target) > threshold:
            entry.warnings.append("DISTANCE_LABEL_MISMATCH")
    if entry.uncertainty_m is None:
        entry.warnings.append("DISTANCE_SIDECAR_NO_UNCERTAINTY")


def merge_sidecar(runs, entries) -> None:
    """Attach sidecar entries to runs and assign truth levels in place."""
    seen = {}
    orphans = []
    for entry in entries:
        key = (entry.batch_id, entry.run_id)
        if key in seen:
            entry.errors.append("DISTANCE_SIDECAR_DUPLICATE")
            seen[key].errors.append("DISTANCE_SIDECAR_DUPLICATE")
            continue
        seen[key] = entry

    for run in runs:
        key = (run.batch_id, run.run_id)
        entry = seen.get(key)
        if entry is None:
            # no sidecar: fitting-grade truth = the selected integer tier
            run.truth = "paced"
            run.truth_m = run.target_m
            run.sidecar = {"present": False}
            continue
        _entry_checks(entry, run)
        # bearing precedence (v5): the on-device annotation wins; the sidecar
        # only backfills runs without one. Disagreement beyond a half-sector
        # is a conflict warning, not a silent overwrite.
        if entry.bearing_deg is not None:
            if run.bearing_deg is None:
                run.bearing_deg = entry.bearing_deg
            elif abs(entry.bearing_deg - run.bearing_deg) > 22.5:
                run.warnings.append("BEARING_CONFLICT")
        run.sidecar = {
            "present": True,
            "accepted": entry.accepted,
            "measured_distance_m": entry.measured_distance_m,
            "method": entry.method,
            "uncertainty_m": entry.uncertainty_m,
            "window_ms": list(entry.window_ms) if entry.window_ms else None,
            "capture": entry.capture,
            "measurement_session": entry.measurement_session,
            "errors": list(entry.errors),
            "warnings": list(entry.warnings),
        }
        run.warnings.extend(entry.warnings)
        run.flags.extend(e for e in entry.errors if e not in run.flags)
        if entry.accepted:
            run.truth = "measured"
            run.truth_m = entry.measured_distance_m
        else:
            # a present-but-rejected entry must stay visible (SUSPECT)
            run.truth = "paced"
            run.truth_m = run.target_m
            run.flags.append("SIDE_CAR_REJECTED")

    for key, entry in seen.items():
        if not any((run.batch_id, run.run_id) == key for run in runs):
            entry.errors.append("DISTANCE_SIDECAR_UNKNOWN_RUN")
            orphans.append(entry)
    return orphans
