#!/usr/bin/env python3
"""tools/model_fit.py — PC-side capture-data tool (minimal subset).

Subcommands (full design: update program §6):

  parse  LOG... -o dataset/ [--sidecar measured.yaml]
      Parse USB monitor logs into dataset/runs.json, optionally merge a
      measured-distance / bearing sidecar and print the run quality table.

  report dataset/ [--distance-sidecar measured.yaml]
      Re-print the per-run quality table from a parsed dataset, optionally
      applying (or replacing) the distance sidecar with DISTANCE_SIDECAR_*
      alignment checks.

Zero third-party requirements beyond PyYAML (sidecar files only).
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from model_fit import parser, report, sidecar  # noqa: E402

DATASET_FILE = "runs.json"


def _merge_state_reset(runs) -> None:
    for run in runs:
        run.truth = "paced"
        run.truth_m = None
        run.bearing_deg = None
        run.sidecar = {}
        run.flags = []
        run.warnings = []


def _dump_dataset(path: Path, result: parser.ParseResult) -> None:
    data = {
        "format_version": parser.FORMAT_VERSION,
        "sources": result.sources,
        "runs": [parser.run_to_dict(run) for run in result.runs],
        "sweeps": [vars(s) for s in result.sweeps],
        "calibs": [vars(c) for c in result.calibs],
    }
    path.write_text(json.dumps(data, ensure_ascii=False, indent=1),
                    encoding="utf-8")


def _load_dataset(path: Path) -> parser.ParseResult:
    data = json.loads(path.read_text(encoding="utf-8"))
    result = parser.ParseResult()
    result.sources = list(data.get("sources", []))
    result.runs = [parser.run_from_dict(r) for r in data.get("runs", [])]
    for sweep in data.get("sweeps", []):
        result.sweeps.append(parser.SweepResult(**sweep))
    for calib in data.get("calibs", []):
        result.calibs.append(parser.CalibResult(**calib))
    return result


def cmd_parse(args) -> int:
    result = parser.ParseResult()
    result.sources = []
    for log in args.logs:
        partial = parser.parse_file(log)
        result.runs.extend(partial.runs)
        result.sweeps.extend(partial.sweeps)
        result.calibs.extend(partial.calibs)
        result.sources.extend(partial.sources)

    entries = None
    if args.sidecar:
        _, entries = sidecar.load_sidecar(args.sidecar)

    rows, summary = report.evaluate(result, entries)
    out_dir = Path(args.output)
    out_dir.mkdir(parents=True, exist_ok=True)
    _dump_dataset(out_dir / DATASET_FILE, result)
    print(report.format_report(rows, summary, result))
    print("dataset: %s" % (out_dir / DATASET_FILE))
    return 0


def cmd_report(args) -> int:
    dataset = Path(args.dataset) / DATASET_FILE
    if not dataset.is_file():
        print("ERROR: %s not found (run `model_fit.py parse` first)" % dataset,
              file=sys.stderr)
        return 2
    result = _load_dataset(dataset)

    entries = None
    if args.distance_sidecar:
        _merge_state_reset(result.runs)
        _, entries = sidecar.load_sidecar(args.distance_sidecar)

    rows, summary = report.evaluate(result, entries)
    print(report.format_report(rows, summary, result))
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="model_fit.py", description=__doc__)
    sub = ap.add_subparsers(dest="command", required=True)

    ap_parse = sub.add_parser("parse", help="USB monitor log -> dataset/")
    ap_parse.add_argument("logs", nargs="+", help="USB monitor log file(s)")
    ap_parse.add_argument("-o", "--output", required=True,
                          help="output dataset directory")
    ap_parse.add_argument("--sidecar", help="annotation/measured-distance YAML")
    ap_parse.set_defaults(func=cmd_parse)

    ap_report = sub.add_parser("report", help="per-run quality table")
    ap_report.add_argument("dataset", help="dataset directory from `parse`")
    ap_report.add_argument("--distance-sidecar",
                           help="measured-distance YAML (validation-grade truth)")
    ap_report.set_defaults(func=cmd_report)

    args = ap.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
