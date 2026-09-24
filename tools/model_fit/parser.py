"""USB monitor log parser: BATCH_*/CAPTURE*/CSV/SWEEP_RESULT/CALIB_RESULT rows.

Regexes are kept strictly in sync with the firmware emitters:

- ``main/radar_capture.c``: ``emit_csv()`` (``BATCH_CAPTURE_META`` /
  ``CAPTURE_META`` + ``*_CSV`` rows), ``BATCH_CAPTURE_CONDITION``,
  ``MODEL_CANDIDATE``
- ``main/main.c``: ``SWEEP_RESULT`` (package B), ``CALIB_RESULT`` (package B)

Every run is assembled from its CONDITION + META + CSV rows and carries the
log timestamp of its export line as the time anchor used by the sidecar
alignment checks (``DISTANCE_SIDECAR_*``, remediation design §4.1).
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from typing import List, Optional

FORMAT_VERSION = 1

# CSV record columns, matching emit_csv() row format in radar_capture.c.
CSV_COLUMNS = (
    "target_m",
    "elapsed_ms",
    "local_rssi",
    "peer_rssi",
    "peer_alive",
    "ftm_valid",
    "ftm_status",
    "ftm_distance_m",
    "ftm_spread_m",
    "ftm_session_count",
    "ble_ready",
    "ble_peer_alive",
    "ble_rssi",
    "ble_age_ms",
    "csi_frames",
    "env_level",
)

# scene label (radar_capture_scene_label) -> (environment, link)
SCENE_LABEL_MAP = {
    "I-LOS": (1, 0),
    "I-STATIC": (1, 1),
    "I-DYNAMIC": (1, 2),
    "I-MULTIPATH": (1, 3),
    "O-LOS": (0, 0),
    "O-STATIC": (0, 1),
    "O-DYNAMIC": (0, 2),
    "O-MULTIPATH": (0, 3),
}

_LOG_PREFIX_RE = re.compile(r"^[IWEF]\s*\((\d+)\)\s+\S+\s*:\s+(.*)$")
_KV_RE = re.compile(r"(\w+)=([^\s]+)")
_MARKER_RE = re.compile(
    r"\b(BATCH_CAPTURE_CONDITION|BATCH_CAPTURE_META|CAPTURE_META|"
    r"MODEL_CANDIDATE|BATCH_CAPTURE_CSV|CAPTURE_CSV|"
    r"SWEEP_RESULT|CALIB_RESULT)\b\s*(.*)$"
)
_CSV_HEADER_PREFIX = "target_m,"


def _to_number(value):
    if not isinstance(value, str):
        return value
    try:
        if re.fullmatch(r"-?\d+", value):
            return int(value)
        return float(value)
    except ValueError:
        return value


def _parse_kvs(text: str) -> dict:
    return {key: _to_number(val) for key, val in _KV_RE.findall(text)}


# Bearing values share the enum of radar_capture_bearing_id_t (v5): 0..7 =
# device-relative sector centers (0=FRONT, then clockwise FRONT_RIGHT ..
# FRONT_LEFT; angle measured from the holder's forward axis),
# 8 (0xFF on v4 records) = UNSET.
BEARING_SECTOR_MAX = 7
BEARING_UNSET = 8


@dataclass
class SweepResult:
    angle_deg: float
    spread_deg: float
    dist_m: float
    conf: int
    bearing: int = BEARING_UNSET  # v5: operator annotation, 0..7 (UNSET on v4)
    log_ts_ms: Optional[int] = None


@dataclass
class CalibResult:
    ref_sym_dbm: float
    offset_m: float
    log_ts_ms: Optional[int] = None


@dataclass
class Run:
    batch_id: int = 0
    run_id: int = 0
    source: str = "batch"  # 'batch' (BATCH_CAPTURE_*) or 'nvs' (CAPTURE_*)
    scene: str = ""
    condition: dict = field(default_factory=dict)
    meta: dict = field(default_factory=dict)
    candidate: dict = field(default_factory=dict)
    records: List[dict] = field(default_factory=list)
    log_ts_ms: Optional[int] = None  # export-line time anchor
    # sidecar merge results (sidecar.py)
    truth: str = "paced"  # 'paced' | 'measured'
    truth_m: Optional[float] = None
    bearing_deg: Optional[float] = None
    sidecar: dict = field(default_factory=dict)
    flags: List[str] = field(default_factory=list)
    warnings: List[str] = field(default_factory=list)

    @property
    def target_m(self) -> float:
        value = self.meta.get("target_m")
        if value is None:
            value = self.condition.get("target_m")
        return float(value) if value is not None else float("nan")

    @property
    def record_count_meta(self) -> int:
        value = self.meta.get("records")
        return int(value) if value is not None else 0

    @property
    def elapsed_span_ms(self) -> int:
        if not self.records:
            return 0
        return max(int(r["elapsed_ms"]) for r in self.records)


@dataclass
class ParseResult:
    runs: List[Run] = field(default_factory=list)
    sweeps: List[SweepResult] = field(default_factory=list)
    calibs: List[CalibResult] = field(default_factory=list)
    sources: List[str] = field(default_factory=list)
    unparsed_markers: int = 0


def _split_line(line: str):
    line = line.rstrip("\r\n")
    match = _LOG_PREFIX_RE.match(line)
    if match:
        return int(match.group(1)), match.group(2)
    return None, line


def _record_from_csv(row: str) -> Optional[dict]:
    parts = [p.strip() for p in row.split(",")]
    if len(parts) != len(CSV_COLUMNS):
        return None
    record = {}
    for name, raw in zip(CSV_COLUMNS, parts):
        record[name] = _to_number(raw)
    return record


def parse_lines(lines, source_name: str = "<log>") -> ParseResult:
    result = ParseResult()
    result.sources.append(source_name)

    pending_condition = None
    pending_candidate = None
    current = None

    def finalize():
        nonlocal current
        if current is not None:
            result.runs.append(current)
            current = None

    for line in lines:
        log_ts, text = _split_line(line)
        match = _MARKER_RE.search(text)
        if not match:
            continue
        marker, rest = match.group(1), match.group(2).strip()

        if marker == "BATCH_CAPTURE_CONDITION":
            pending_condition = _parse_kvs(rest)
            continue
        if marker == "MODEL_CANDIDATE":
            pending_candidate = _parse_kvs(rest)
            continue
        if marker in ("BATCH_CAPTURE_META", "CAPTURE_META"):
            finalize()
            kv = _parse_kvs(rest)
            run = Run()
            run.source = "batch" if marker.startswith("BATCH_") else "nvs"
            run.batch_id = int(kv.get("batch_id", pending_condition.get("batch", 0))
                               if pending_condition else kv.get("batch_id", 0))
            run.run_id = int(kv.get("capture_id", 0))
            run.meta = kv
            run.log_ts_ms = log_ts
            if pending_condition is not None:
                run.condition = dict(pending_condition)
                run.scene = str(run.condition.get("scene", ""))
                if "batch" in run.condition:
                    run.batch_id = int(run.condition["batch"])
                if "run" in run.condition:
                    run.run_id = int(run.condition["run"])
                pending_condition = None
            if pending_candidate is not None:
                run.candidate = dict(pending_candidate)
                if "batch" in run.candidate and not run.batch_id:
                    run.batch_id = int(run.candidate["batch"])
                if "run" in run.candidate and not run.run_id:
                    run.run_id = int(run.candidate["run"])
                if not run.scene:
                    run.scene = str(run.candidate.get("scene", ""))
                pending_candidate = None
            current = run
            continue
        if marker in ("BATCH_CAPTURE_CSV", "CAPTURE_CSV"):
            if rest.startswith(_CSV_HEADER_PREFIX):
                continue
            if current is not None:
                record = _record_from_csv(rest)
                if record is not None:
                    current.records.append(record)
            continue
        if marker == "SWEEP_RESULT":
            kv = _parse_kvs(rest)
            result.sweeps.append(SweepResult(
                angle_deg=float(kv.get("angle", float("nan"))),
                spread_deg=float(kv.get("spread", float("nan"))),
                dist_m=float(kv.get("dist", float("nan"))),
                conf=int(kv.get("conf", -1)),
                bearing=int(kv.get("bearing", BEARING_UNSET)),
                log_ts_ms=log_ts,
            ))
            continue
        if marker == "CALIB_RESULT":
            kv = _parse_kvs(rest)
            result.calibs.append(CalibResult(
                ref_sym_dbm=float(kv.get("ref", float("nan"))),
                offset_m=float(kv.get("offset", float("nan"))),
                log_ts_ms=log_ts,
            ))
            continue
        result.unparsed_markers += 1

    finalize()
    return result


def parse_file(path) -> ParseResult:
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        return parse_lines(handle, source_name=str(path))


def run_to_dict(run: Run) -> dict:
    return {
        "batch_id": run.batch_id,
        "run_id": run.run_id,
        "source": run.source,
        "scene": run.scene,
        "condition": run.condition,
        "meta": run.meta,
        "candidate": run.candidate,
        "records": run.records,
        "log_ts_ms": run.log_ts_ms,
        "truth": run.truth,
        "truth_m": run.truth_m,
        "bearing_deg": run.bearing_deg,
        "sidecar": run.sidecar,
        "flags": run.flags,
        "warnings": run.warnings,
    }


def run_from_dict(data: dict) -> Run:
    run = Run()
    for name in ("batch_id", "run_id", "source", "scene", "condition", "meta",
                 "candidate", "records", "log_ts_ms", "truth", "truth_m",
                 "bearing_deg", "sidecar", "flags", "warnings"):
        if name in data:
            setattr(run, name, data[name])
    return run
