#!/usr/bin/env python3
"""Host tests for tools/model_fit (package A2): parse/report + sidecar checks.

Covers, against synthetic USB monitor logs:
- run assembly from BATCH_CAPTURE_CONDITION / *_META / *_CSV rows (对账)
- SWEEP_RESULT / CALIB_RESULT line parsing (package B)
- cleaning rules (records 95~102, interval 250~450 ms)
- label checks (propagation↔link, scene↔(env,link)) -> REJECTED on conflict
- sidecar merge with DISTANCE_SIDECAR_* flags and tiered truth
- CLI parse/report end to end
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from model_fit import parser, report, sidecar  # noqa: E402

BATCH = 493512032
CSV_HEADER = (
    "target_m,elapsed_ms,local_rssi,peer_rssi,peer_alive,ftm_valid,ftm_status,"
    "ftm_distance_m,ftm_spread_m,ftm_session_count,ble_ready,ble_peer_alive,"
    "ble_rssi,ble_age_ms,csi_frames,env_level"
)


def condition_line(ts, run, scene, env, link, prop, obs=0, bearing=0):
    return (
        f"I ({ts}) radar_capture: BATCH_CAPTURE_CONDITION batch={BATCH} "
        f"run={run} scene={scene} schema=1 env={env} link={link} obs={obs} "
        f"prop={prop} room=5 size=3 material=0 furniture=0 boundary=0 "
        f"height=1 pose=0 bearing={bearing} observed_level=0 "
        f"observed_obstruction=0"
    )


def candidate_line(ts, run, scene, target, records):
    return (
        f"I ({ts}) radar_capture: MODEL_CANDIDATE batch={BATCH} run={run} "
        f"scene={scene} target_m={target} records={records} ftm_ok=80 "
        f"ftm_rate_pct=80 rssi_mean=-65 ftm_mean_cm=310"
    )


def meta_line(ts, run, target, records, crc="0x12345678",
              tag="BATCH_CAPTURE_META"):
    return (
        f"I ({ts}) radar_capture: {tag} capture_id={run} "
        f"firmware=slot2-batch boot_sequence=2 reset_reason=9 "
        f"target_m={target} records={records} crc32={crc} generation=1 "
        f"compressed=800/1600 ftm_raw_median_m=3.100 ftm_raw_iqr_m=0.200 "
        f"ftm_raw_sessions=8 ftm_raw_bursts=12"
    )


def csv_block(ts0, target, count, step=300, marker="BATCH_CAPTURE_CSV"):
    rows = [f"I ({ts0}) radar_capture: {marker} {CSV_HEADER}"]
    for i in range(count):
        rows.append(
            f"I ({ts0 + 1 + i}) radar_capture: {marker} "
            f"{target},{i * step},-64,-63,1,1,0,3.100,0.200,4,1,1,-63,250,10,0"
        )
    return rows


def build_log(step=300):
    lines = []
    # run 1: clean O-LOS, 100 records @300ms
    lines.append(condition_line(100000, 1, "O-LOS", 0, 0, 0))
    lines.append(candidate_line(100500, 1, "O-LOS", "3.0", 100))
    lines.append(meta_line(101000, 1, "3.0", 100))
    lines.extend(csv_block(101001, "3.0", 100, step))
    # run 2: I-STATIC, only 80 records (below the 95 cleaning floor)
    lines.append(condition_line(200000, 2, "I-STATIC", 1, 1, 1))
    lines.append(meta_line(201000, 2, "6.0", 80))
    lines.extend(csv_block(201001, "6.0", 80, step))
    # run 3: O-MULTIPATH with a hard propagation conflict (prop=0, expect 2)
    lines.append(condition_line(300000, 3, "O-MULTIPATH", 0, 3, 0))
    lines.append(meta_line(301000, 3, "8.0", 100))
    lines.extend(csv_block(301001, "8.0", 100, step))
    # package B result lines (v5: bearing annotation on every sweep)
    lines.append("I (400000) main: SWEEP_RESULT bearing=3 angle=123.4 "
                 "spread=10.0 dist=3.05 conf=3")
    lines.append("I (401000) main: CALIB_RESULT ref=-45.3 offset=0.12")
    return lines


def parse_log(step=300):
    return parser.parse_lines(build_log(step), source_name="synthetic")


class ParseRunsTest(unittest.TestCase):
    def test_run_assembly_and_reconciliation(self) -> None:
        result = parse_log()
        self.assertEqual(len(result.runs), 3)
        run1 = result.runs[0]
        self.assertEqual(run1.batch_id, BATCH)
        self.assertEqual(run1.run_id, 1)
        self.assertEqual(run1.scene, "O-LOS")
        self.assertEqual(len(run1.records), 100)
        self.assertEqual(run1.record_count_meta, 100)
        self.assertEqual(run1.target_m, 3.0)
        self.assertEqual(run1.log_ts_ms, 101000)
        self.assertEqual(run1.candidate.get("records"), 100)
        self.assertEqual(run1.records[0]["elapsed_ms"], 0)
        self.assertEqual(run1.records[0]["ftm_distance_m"], 3.1)

    def test_package_b_result_lines(self) -> None:
        result = parse_log()
        self.assertEqual(len(result.sweeps), 1)
        self.assertEqual(result.sweeps[0].bearing, 3)
        self.assertAlmostEqual(result.sweeps[0].angle_deg, 123.4)
        self.assertAlmostEqual(result.sweeps[0].dist_m, 3.05)
        self.assertEqual(result.sweeps[0].conf, 3)
        self.assertEqual(len(result.calibs), 1)
        self.assertAlmostEqual(result.calibs[0].ref_sym_dbm, -45.3)
        self.assertAlmostEqual(result.calibs[0].offset_m, 0.12)


class VerdictTest(unittest.TestCase):
    def test_pass_suspect_rejected(self) -> None:
        result = parse_log()
        rows, summary = report.evaluate(result)
        self.assertEqual([r["verdict"] for r in rows],
                         ["PASS", "SUSPECT", "REJECTED"])
        self.assertEqual(summary["pass"], 1)
        self.assertEqual(summary["suspect"], 1)
        self.assertEqual(summary["rejected"], 1)
        self.assertIn("RECORD_COUNT_BAND", rows[1]["warnings"])
        self.assertIn("CONDITION_PROPAGATION_CONFLICT", rows[2]["flags"])
        # tiered truth default: paced -> truth_m == target tier
        self.assertEqual(rows[0]["truth"], "paced")
        self.assertEqual(rows[0]["truth_m"], 3.0)

    def test_interval_band(self) -> None:
        result = parse_log(step=200)
        rows, _ = report.evaluate(result)
        self.assertIn("INTERVAL_BAND", rows[0]["warnings"])
        self.assertEqual(rows[0]["verdict"], "SUSPECT")

    def test_candidate_mismatch(self) -> None:
        lines = [
            condition_line(1, 1, "O-LOS", 0, 0, 0),
            candidate_line(2, 1, "O-LOS", "3.0", 99),
            meta_line(3, 1, "3.0", 100),
            *csv_block(4, "3.0", 100),
        ]
        result = parser.parse_lines(lines)
        rows, _ = report.evaluate(result)
        self.assertIn("CANDIDATE_RECORD_MISMATCH", rows[0]["warnings"])

    def test_scene_conflict_is_rejected(self) -> None:
        lines = [
            condition_line(1, 1, "O-LOS", 1, 3, 2, bearing=5),  # label says (0,0)
            meta_line(2, 1, "3.0", 100),
            *csv_block(3, "3.0", 100),
        ]
        result = parser.parse_lines(lines)
        rows, _ = report.evaluate(result)
        self.assertIn("CONDITION_SCENE_CONFLICT", rows[0]["flags"])
        self.assertEqual(rows[0]["verdict"], "REJECTED")

    def test_nvs_fallback_without_labels_is_suspect(self) -> None:
        lines = [
            meta_line(2, 1, "3.0", 100, tag="CAPTURE_META"),
            *csv_block(3, "3.0", 100, marker="CAPTURE_CSV"),
        ]
        result = parser.parse_lines(lines)
        rows, _ = report.evaluate(result)
        self.assertIsNone(result.runs[0].bearing_deg)
        self.assertEqual(rows[0]["source"], "nvs")
        self.assertIn("NO_CONDITION_LABELS", rows[0]["warnings"])
        self.assertEqual(rows[0]["verdict"], "SUSPECT")


def load_entries(yaml_text: str):
    with tempfile.NamedTemporaryFile("w", suffix=".yaml", delete=False) as fh:
        fh.write(yaml_text)
        path = fh.name
    try:
        _, entries = sidecar.load_sidecar(path)
    finally:
        Path(path).unlink()
    return entries


VALID_ENTRY = f"""
batch_id: {BATCH}
measurement_session: outdoor-2026-09-23-a
operator: alex
runs:
  - run_id: 1
    measured_distance_m: 3.04
    method: tape
    uncertainty_m: 0.03
    window_ms: [0, 38000]
    capture: 101000
"""


class SidecarTest(unittest.TestCase):
    def test_valid_entry_upgrades_truth(self) -> None:
        result = parse_log()
        rows, summary = report.evaluate(result, load_entries(VALID_ENTRY))
        self.assertEqual(rows[0]["truth"], "measured")
        self.assertAlmostEqual(rows[0]["truth_m"], 3.04)
        self.assertEqual(rows[0]["bearing_deg"], 0)
        self.assertEqual(rows[0]["verdict"], "PASS")
        self.assertEqual(summary["truth_measured"], 1)
        # v5: every run carries the device bearing annotation (default FRONT=0)
        self.assertEqual(summary["with_bearing"], 3)

    def test_sidecar_bearing_backfills_when_unset(self) -> None:
        # device annotation UNSET (v4-style log): sidecar bearing backfills
        entries = load_entries(f"""
batch_id: {BATCH}
runs:
  - run_id: 1
    measured_distance_m: 3.02
    method: tape
    capture: 101000
    bearing_deg: 45
""")
        # rebuild run 1 without a device bearing annotation
        result = parser.parse_lines(build_log())
        result.runs[0].condition["bearing"] = parser.BEARING_UNSET
        result.runs[0].meta["bearing"] = parser.BEARING_UNSET
        rows, _ = report.evaluate(result, entries)
        self.assertEqual(rows[0]["bearing_deg"], 45)
        self.assertEqual(rows[0]["truth"], "measured")

    def test_bearing_conflict_warns_and_device_wins(self) -> None:
        entries = load_entries(f"""
batch_id: {BATCH}
runs:
  - run_id: 1
    measured_distance_m: 3.02
    method: tape
    capture: 101000
    bearing_deg: 45
""")
        result = parser.parse_lines(build_log())
        result.runs[0].condition["bearing"] = 2  # device sector 2 = RIGHT (90 deg)
        rows, _ = report.evaluate(result, entries)
        self.assertEqual(rows[0]["bearing_deg"], 90)  # device annotation wins
        self.assertIn("BEARING_CONFLICT", rows[0]["warnings"])
        self.assertEqual(rows[0]["truth"], "measured")

    def test_missing_time_anchor_rejected(self) -> None:
        entries = load_entries(f"""
batch_id: {BATCH}
runs:
  - run_id: 1
    measured_distance_m: 3.04
    method: tape
    window_ms: [0, 38000]
""")
        result = parse_log()
        rows, _ = report.evaluate(result, entries)
        self.assertIn("DISTANCE_SIDECAR_NO_TIME_ANCHOR", rows[0]["flags"])
        self.assertEqual(rows[0]["truth"], "paced")  # never silent downgrade
        self.assertEqual(rows[0]["verdict"], "SUSPECT")

    def test_misaligned_anchor_and_window(self) -> None:
        entries = load_entries(f"""
batch_id: {BATCH}
runs:
  - run_id: 1
    measured_distance_m: 3.04
    method: laser
    uncertainty_m: 0.02
    window_ms: [50000, 60000]
    capture: 999999999
""")
        result = parse_log()
        rows, _ = report.evaluate(result, entries)
        codes = [c for c in rows[0]["flags"] if c.startswith("DISTANCE_SIDECAR_")]
        self.assertIn("DISTANCE_SIDECAR_MISALIGNED", codes)
        self.assertEqual(rows[0]["verdict"], "SUSPECT")

    def test_duplicate_and_unknown_run(self) -> None:
        entries = load_entries(f"""
batch_id: {BATCH}
runs:
  - run_id: 1
    measured_distance_m: 3.04
    method: tape
    capture: 101000
  - run_id: 1
    measured_distance_m: 3.06
    method: tape
    capture: 101000
  - run_id: 9
    measured_distance_m: 5.0
    method: laser
    capture: 101000
""")
        result = parse_log()
        _, summary = report.evaluate(result, entries)
        self.assertEqual(entries[0].errors.count("DISTANCE_SIDECAR_DUPLICATE"), 1)
        self.assertEqual(entries[1].errors.count("DISTANCE_SIDECAR_DUPLICATE"), 1)
        self.assertIn("DISTANCE_SIDECAR_UNKNOWN_RUN", entries[2].errors)
        self.assertEqual(summary["sidecar_orphans"], 1)

    def test_invalid_distance_and_bad_method(self) -> None:
        entries = load_entries(f"""
batch_id: {BATCH}
runs:
  - run_id: 1
    measured_distance_m: -2.0
    method: pace
    capture: 101000
""")
        result = parse_log()
        rows, _ = report.evaluate(result, entries)
        self.assertIn("DISTANCE_SIDECAR_INVALID_DISTANCE", rows[0]["flags"])
        self.assertIn("DISTANCE_SIDECAR_BAD_METHOD", rows[0]["flags"])

    def test_label_mismatch_warns_but_measured_wins(self) -> None:
        entries = load_entries(f"""
batch_id: {BATCH}
runs:
  - run_id: 1
    measured_distance_m: 8.0
    method: laser
    uncertainty_m: 0.05
    capture: 101000
""")
        result = parse_log()
        rows, _ = report.evaluate(result, entries)
        self.assertEqual(rows[0]["truth"], "measured")
        self.assertAlmostEqual(rows[0]["truth_m"], 8.0)
        self.assertIn("DISTANCE_LABEL_MISMATCH", rows[0]["warnings"])
        self.assertEqual(rows[0]["verdict"], "SUSPECT")

    def test_string_anchor_rejected_as_unverifiable(self) -> None:
        entries = load_entries(f"""
batch_id: {BATCH}
runs:
  - run_id: 1
    measured_distance_m: 3.04
    method: tape
    window_ms: [0, 38000]
    capture: 2026-09-23T10:00:00
""")
        result = parse_log()
        rows, _ = report.evaluate(result, entries)
        self.assertIn("DISTANCE_SIDECAR_UNVERIFIABLE_ANCHOR",
                      rows[0]["flags"])
        self.assertEqual(rows[0]["truth"], "paced")
        self.assertEqual(rows[0]["verdict"], "SUSPECT")

    def test_missing_uncertainty_is_reported(self) -> None:
        entries = load_entries(f"""
batch_id: {BATCH}
runs:
  - run_id: 1
    measured_distance_m: 3.02
    method: tape
    capture: 101000
""")
        result = parse_log()
        rows, _ = report.evaluate(result, entries)
        self.assertIn("DISTANCE_SIDECAR_NO_UNCERTAINTY", rows[0]["warnings"])
        self.assertEqual(rows[0]["truth"], "measured")


class CliTest(unittest.TestCase):
    def test_parse_and_report_end_to_end(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            log_path = tmp_path / "monitor.log"
            log_path.write_text("\n".join(build_log()), encoding="utf-8")
            sidecar_path = tmp_path / "measured.yaml"
            sidecar_path.write_text(VALID_ENTRY, encoding="utf-8")

            proc = subprocess.run(
                [sys.executable, str(ROOT / "tools" / "model_fit.py"),
                 "parse", str(log_path), "-o", str(tmp_path / "dataset"),
                 "--sidecar", str(sidecar_path)],
                capture_output=True, text=True,
            )
            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertIn("PASS=1", proc.stdout)
            self.assertIn("SUSPECT=1", proc.stdout)
            self.assertIn("REJECTED=1", proc.stdout)
            self.assertIn("SWEEP_RESULT: 1 lines", proc.stdout)
            self.assertIn("CALIB_RESULT: 1 lines", proc.stdout)
            self.assertTrue((tmp_path / "dataset" / "runs.json").is_file())

            proc = subprocess.run(
                [sys.executable, str(ROOT / "tools" / "model_fit.py"),
                 "report", str(tmp_path / "dataset"),
                 "--distance-sidecar", str(sidecar_path)],
                capture_output=True, text=True,
            )
            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertIn("truth(measured=1,paced=2)", proc.stdout)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
