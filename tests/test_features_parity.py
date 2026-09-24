#!/usr/bin/env python3
"""C↔Python feature-replay parity (package A3).

Runs tests/test_features_parity.c (path given as argv[1]) and compares every
printed feature value against tools/model_fit/features.py on identical
synthetic vectors. Allowed deviation: 1e-4 (update program §6.2).

Vector generators must stay bit-identical with the C side.
"""

from __future__ import annotations

import math
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from model_fit import features  # noqa: E402

N = 48
NOW_MS = 7051  # 150*(N-1) + 1
TOLERANCE = 1e-4


def fill_const():
    return [-60.0] * N


def fill_ramp():
    return [-50.0 - 0.5 * i for i in range(N)]


def fill_mod():
    return [
        -65.0 + (((i * 7) % 11) - 5) + (i % 3) * 0.5 - 0.5 for i in range(N)
    ]


def fill_lcg():
    x = []
    s = 12345
    for _ in range(N):
        s = (s * 1103515245 + 12345) & 0xFFFFFFFF
        x.append(-90.0 + ((s >> 16) % 51))
    return x


def fill_ts():
    return [150 * i for i in range(N)]


def expected_values():
    expected = {}
    ts = fill_ts()
    for name, x in (
        ("const", fill_const()),
        ("ramp", fill_ramp()),
        ("mod", fill_mod()),
        ("lcg", fill_lcg()),
    ):
        p1, p10 = features.p2p(x, ts, NOW_MS)
        expected["kfactor." + name] = features.kfactor_db(x)
        expected["uquant80." + name] = features.upper_quantile(x, 0.8)
        expected["uquant50." + name] = features.upper_quantile(x, 0.5)
        expected["p2p1." + name] = p1
        expected["p2p10." + name] = p10
        expected["meanr3." + name] = features.mean_recent(x, ts, NOW_MS, 3000)
        expected["meanr10." + name] = features.mean_recent(x, ts, NOW_MS, 10000)

    raw = [3.0, 3.1, 2.9, 3.3, 0.0, -1.0, 3.05, 1500.0, 3.02, 2.95, 3.2, 3.15]
    calib = [1.02, 0.98, 1.05, 1.0, 0.9]
    hist = [2.0, 2.5, 3.0, 3.5, 4.0, 4.5]
    median, spread = features.ftm_filter(raw)
    empty_median, _ = features.ftm_filter([])

    for i, (d_rssi, d_ftm, n) in enumerate(
        [(3.0, 2.6, 2.2), (8.0, 6.5, 3.5), (2.0, 3.0, 2.2), (1.0, 0.0, 2.2),
         (5.0, 4.9, 2.2)]
    ):
        expected["excess.%d" % i] = features.excess_loss_db(d_rssi, d_ftm, n)
    for i, (sym, ref, env) in enumerate(
        [(-55.0, -45.0, 0), (-55.0, -45.0, 1), (-55.0, -45.0, 2),
         (-90.0, -45.0, 0), (-127.0, -45.0, 0), (-55.0, -35.0, 1),
         (-70.0, -45.0, 0)]
    ):
        expected["symdist.%d" % i] = features.sym_distance(sym, ref, env)
    for i, dist in enumerate([3.0, 5.0, 8.0, 25.0, -1.0, 0.03, 60.0]):
        expected["ftmw.%d" % i] = features.ftm_weight(dist)
    expected["ftmfilter.median"] = median
    expected["ftmfilter.spread"] = spread
    expected["ftmfilter.empty"] = empty_median
    expected["ftmoffset.0"] = features.ftm_offset_from_calib(calib)
    expected["ftmoffset.1"] = features.ftm_offset_from_calib(calib[:2])
    expected["ftmoffset.2"] = features.ftm_offset_from_calib([])
    expected["applyoff.0"] = features.ftm_apply_offset(2.5, 0.3)
    expected["applyoff.1"] = features.ftm_apply_offset(0.2, 0.3)
    expected["applyoff.2"] = features.ftm_apply_offset(float("nan"), 0.3)
    expected["lowquant.0"] = features.ftm_low_quantile(hist)
    expected["lowquant.1"] = features.ftm_low_quantile([])
    expected["calibref.0"] = features.calib_ref(-25.0)
    expected["calibref.1"] = features.calib_ref(-70.0)
    expected["calibref.2"] = features.calib_ref(-45.0)
    expected["kfactor.short"] = features.kfactor_db(fill_ramp()[:10])
    return expected


BINARY = sys.argv[1] if len(sys.argv) > 1 else ""


class FeatureParityTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if not BINARY:
            raise unittest.SkipTest("no parity binary path (argv[1])")
        output = subprocess.run(
            [BINARY], capture_output=True, text=True, check=True
        ).stdout
        cls.actual = {}
        for line in output.splitlines():
            name, _, value = line.partition(" ")
            cls.actual[name.strip()] = float(value)

    def test_c_and_python_features_match(self) -> None:
        expected = expected_values()
        self.assertEqual(
            set(expected), set(self.actual),
            "feature name sets diverged: vectors/outputs out of sync",
        )
        for name, want in expected.items():
            got = self.actual[name]
            if math.isnan(want) or math.isnan(got):
                self.assertTrue(
                    math.isnan(want) and math.isnan(got),
                    "%s: C=%r python=%r" % (name, got, want),
                )
                continue
            self.assertLessEqual(
                abs(got - want), TOLERANCE,
                "%s: C=%.9f python=%.9f diff=%.2e" % (name, got, want, got - want),
            )


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
