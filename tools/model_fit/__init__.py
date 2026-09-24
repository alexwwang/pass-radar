"""PC-side capture-data tooling for pass-radar (minimal collection plan, package A).

Modules:
- parser:   USB monitor log -> runs (BATCH_*/CAPTURE*/CSV/SWEEP_RESULT/CALIB_RESULT)
- sidecar:  measured-distance sidecar + DISTANCE_SIDECAR_* alignment checks
- report:   per-run quality table (reconciliation/CRC/labels/truth), PASS/SUSPECT/REJECTED
- features: pure-stdlib replay of radar_obstruct.c / radar_fusion.c features

Fitting modules (fit_rssi/fit_ftm/...) are intentionally not part of this
minimal subset; see docs/assets/pass-radar-v3-model-parameter-update-program
§6.2 for the full module plan.
"""

__version__ = "0.1.0"
