#!/usr/bin/env python3
"""Regression gate for the human-calibrated periodic-lattice detector."""
import hashlib
import json
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "benchmarks/quality_sweeps/lattice_corruption_diagnostic"))
import periodic_lattice_detector as detector  # noqa: E402


class LatticeDetectorRegression(unittest.TestCase):
    def test_failed_and_fixed_fixture_decisions(self):
        manifest = json.loads((ROOT / "benchmarks/quality_sweeps/lattice_corruption_diagnostic/detector_regression_fixtures.json").read_text())
        missing = [f["name"] for f in manifest["fixtures"] if not Path(f["candidate"]).exists() or not Path(f["control"]).exists()]
        if missing:
            self.skipTest("local hash-addressed capture fixtures unavailable: " + ", ".join(missing))
        for fixture in manifest["fixtures"]:
            candidate = Path(fixture["candidate"])
            self.assertEqual(hashlib.sha256(candidate.read_bytes()).hexdigest(), fixture["candidate_sha256"])
            result = detector.analyze(candidate, [Path(fixture["control"])])
            self.assertEqual(result["classification"], fixture["expected"], fixture["name"])


if __name__ == "__main__":
    unittest.main()
