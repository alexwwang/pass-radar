"""Per-run data-quality report (minimal subset of model_fit.py report).

Cleaning rules per the update program §4.1 and tiered truth per the
remediation design §4.1. One quality row per run with reconciliation / CRC /
label / truth columns and a PASS / SUSPECT / REJECTED verdict.
"""

from __future__ import annotations

import math
from typing import List

from . import parser, sidecar

# cleaning rules (update program §4.1)
RECORDS_EXPECTED = 100
RECORDS_MIN = 95
RECORDS_MAX = 102
INTERVAL_MIN_MS = 250
INTERVAL_MAX_MS = 450

VERDICT_PASS = "PASS"
VERDICT_SUSPECT = "SUSPECT"
VERDICT_REJECTED = "REJECTED"

# enum domains (radar_capture_batch.h)
ENV_MAX = 1
LINK_MAX = 3
OBSTRUCTION_MAX = 5
PROP_MAX = 2


def propagation_of(link: int) -> int:
    """radar_capture_condition_propagation(): LOS->LOS, STATIC/DYNAMIC->NLOS,
    MULTIPATH->MULTIPATH."""
    if link in (1, 2):
        return 1
    if link == 3:
        return 2
    return 0


def condition_check(run) -> List[str]:
    """Hard label conflicts (REJECTED) per remediation §4.2. Returns errors;
    legal-but-ambiguous combinations are collected into run.warnings."""
    errors = []
    cond = run.condition
    if not cond:
        return errors
    env = parser._to_number(cond.get("env", -1))
    link = parser._to_number(cond.get("link", -1))
    obs = parser._to_number(cond.get("obs", -1))
    prop = parser._to_number(cond.get("prop", -1))
    if not isinstance(env, int) or not 0 <= env <= ENV_MAX:
        errors.append("CONDITION_ENV_OUT_OF_DOMAIN")
    if not isinstance(link, int) or not 0 <= link <= LINK_MAX:
        errors.append("CONDITION_LINK_OUT_OF_DOMAIN")
    if not isinstance(obs, int) or not 0 <= obs <= OBSTRUCTION_MAX:
        errors.append("CONDITION_OBSTRUCTION_OUT_OF_DOMAIN")
    if not isinstance(prop, int) or not 0 <= prop <= PROP_MAX:
        errors.append("CONDITION_PROPAGATION_OUT_OF_DOMAIN")
    if isinstance(link, int) and isinstance(prop, int) and \
            0 <= link <= LINK_MAX and 0 <= prop <= PROP_MAX:
        if prop != propagation_of(link):
            errors.append("CONDITION_PROPAGATION_CONFLICT")
    scene = run.scene
    if scene in parser.SCENE_LABEL_MAP and isinstance(env, int) and \
            isinstance(link, int):
        if parser.SCENE_LABEL_MAP[scene] != (env, link):
            errors.append("CONDITION_SCENE_CONFLICT")
    elif scene == "CUSTOM":
        run.warnings.append("CONDITION_SCENE_CUSTOM")
    # legal-but-ambiguous combinations (firmware warning set)
    if isinstance(link, int) and isinstance(obs, int):
        ambiguous = (
            (link == 0 and obs in (4, 5)) or          # LOS + WALL/METAL
            (link == 3 and obs == 0) or               # MULTIPATH + NONE
            (link == 1 and obs == 1) or               # STATIC + HUMAN_STATIC
            (env == 0 and obs == 5)                   # OUTDOOR + METAL
        )
        if ambiguous:
            run.warnings.append("CONDITION_AMBIGUOUS")
    return errors


def mean_interval_ms(run) -> float:
    rows = sorted(run.records, key=lambda r: int(r["elapsed_ms"]))
    if len(rows) < 2:
        return float("nan")
    deltas = [
        int(b["elapsed_ms"]) - int(a["elapsed_ms"])
        for a, b in zip(rows, rows[1:])
    ]
    return sum(deltas) / len(deltas)


def evaluate_run(run) -> str:
    """Return PASS / SUSPECT / REJECTED for one run (mutates run.flags)."""
    errors = []
    warnings = []

    if run.source == "nvs" and not run.condition:
        # NVS-fallback exports carry no BATCH_CAPTURE_CONDITION line: labels
        # cannot be verified, so the run stays out of clean per-scene sets.
        warnings.append("NO_CONDITION_LABELS")

    meta = run.meta
    if not meta:
        errors.append("NO_META")
    if not run.records:
        errors.append("NO_RECORDS")
    if meta and "crc32" not in meta:
        errors.append("NO_CRC32")

    errors.extend(condition_check(run))

    # reconciliation (对账): META record count vs decoded CSV rows
    if meta and run.records and run.record_count_meta != len(run.records):
        warnings.append("RECORD_COUNT_MISMATCH")
    if run.candidate:
        cand_records = run.candidate.get("records")
        if cand_records is not None and run.records and \
                int(cand_records) != len(run.records):
            warnings.append("CANDIDATE_RECORD_MISMATCH")

    # cleaning rules
    if run.records:
        count = len(run.records)
        if count < RECORDS_MIN or count > RECORDS_MAX:
            warnings.append("RECORD_COUNT_BAND")
        interval = mean_interval_ms(run)
        if math.isfinite(interval) and \
                not INTERVAL_MIN_MS <= interval <= INTERVAL_MAX_MS:
            warnings.append("INTERVAL_BAND")

    # sidecar state
    if "SIDE_CAR_REJECTED" in run.flags:
        warnings.append("SIDE_CAR_REJECTED")

    run.flags.extend(f for f in errors if f not in run.flags)
    run.warnings.extend(w for w in warnings if w not in run.warnings)

    # PASS = clean, SUSPECT = usable with flags/warnings, REJECTED = hard
    # conflict (remediation §4.2). Sidecar warnings already live in
    # run.warnings and therefore also push the verdict to SUSPECT.
    if errors:
        return VERDICT_REJECTED
    if run.warnings or run.flags:
        return VERDICT_SUSPECT
    return VERDICT_PASS


def evaluate(result: parser.ParseResult, entries=None, sidecar_meta=None):
    """Evaluate all runs; returns (rows, summary)."""
    orphans = []
    if entries is not None:
        orphans = sidecar.merge_sidecar(result.runs, entries)
    rows = []
    for run in result.runs:
        if run.truth_m is None:
            # tiered truth default: fitting grade = the selected integer tier
            run.truth = "paced"
            run.truth_m = run.target_m
        verdict = evaluate_run(run)
        rows.append({
            "batch_id": run.batch_id,
            "run_id": run.run_id,
            "source": run.source,
            "scene": run.scene,
            "target_m": run.target_m,
            "records": len(run.records),
            "records_meta": run.record_count_meta,
            "interval_ms": mean_interval_ms(run),
            "crc32": run.meta.get("crc32"),
            "truth": run.truth,
            "truth_m": run.truth_m,
            "bearing_deg": run.bearing_deg,
            "flags": list(run.flags),
            "warnings": list(run.warnings),
            "verdict": verdict,
        })
    summary = {
        "runs": len(rows),
        "pass": sum(1 for r in rows if r["verdict"] == VERDICT_PASS),
        "suspect": sum(1 for r in rows if r["verdict"] == VERDICT_SUSPECT),
        "rejected": sum(1 for r in rows if r["verdict"] == VERDICT_REJECTED),
        "truth_measured": sum(1 for r in rows if r["truth"] == "measured"),
        "truth_paced": sum(1 for r in rows if r["truth"] == "paced"),
        "with_bearing": sum(1 for r in rows if r["bearing_deg"] is not None),
        "sweeps": len(result.sweeps),
        "calibs": len(result.calibs),
        "sidecar_orphans": len(orphans),
    }
    return rows, summary


_TABLE_COLUMNS = (
    ("batch", 10), ("run", 4), ("scene", 11), ("target", 6),
    ("rec", 5), ("meta", 5), ("int_ms", 6), ("crc32", 6),
    ("truth", 7), ("truth_m", 7), ("brg", 5), ("verdict", 8),
)


def format_report(rows, summary, result: parser.ParseResult) -> str:
    lines = []
    header = " ".join(name.ljust(width) for name, width in _TABLE_COLUMNS)
    lines.append(header)
    lines.append("-" * len(header))
    for row in rows:
        interval = row["interval_ms"]
        cells = [
            str(row["batch_id"]).ljust(10),
            str(row["run_id"]).ljust(4),
            str(row["scene"] or "-").ljust(11),
            f"{row['target_m']:.1f}".ljust(6),
            str(row["records"]).ljust(5),
            str(row["records_meta"]).ljust(5),
            (f"{interval:.0f}" if math.isfinite(interval) else "-").ljust(6),
            ("yes" if row["crc32"] is not None else "NO").ljust(6),
            str(row["truth"]).ljust(7),
            (f"{row['truth_m']:.2f}" if row["truth_m"] is not None
             else "-").ljust(7),
            ("%.0f" % row["bearing_deg"] if row["bearing_deg"] is not None
             else "-").ljust(5),
            row["verdict"].ljust(8),
        ]
        lines.append(" ".join(cells))
        notes = row["flags"] + row["warnings"]
        if notes:
            lines.append("    notes: " + " ".join(notes))
    lines.append("")
    lines.append(
        "runs={runs} PASS={pass} SUSPECT={suspect} REJECTED={rejected} "
        "truth(measured={truth_measured},paced={truth_paced}) "
        "bearing={with_bearing} sweeps={sweeps} calibs={calibs} "
        "sidecar_orphans={sidecar_orphans}".format(**summary)
    )
    if result.sweeps:
        angles = [s.angle_deg for s in result.sweeps if s.angle_deg >= 0.0]
        if angles:
            lines.append(
                "SWEEP_RESULT: %d lines, angle min=%.1f max=%.1f"
                % (len(result.sweeps), min(angles), max(angles))
            )
        else:
            lines.append("SWEEP_RESULT: %d lines, no valid angles"
                         % len(result.sweeps))
    if result.calibs:
        offsets = [c.offset_m for c in result.calibs if math.isfinite(c.offset_m)]
        refs = [c.ref_sym_dbm for c in result.calibs
                if math.isfinite(c.ref_sym_dbm)]
        lines.append(
            "CALIB_RESULT: %d lines, ref=[%s] offset=[%s]"
            % (len(result.calibs),
               ",".join("%.1f" % r for r in refs),
               ",".join("%.2f" % o for o in offsets))
        )
    return "\n".join(lines)
