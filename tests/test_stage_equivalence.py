"""Tests for the format-aware stage-equivalence diagnostic.

These tests define the M7.2 contract before the comparison tool exists: it must
compare numeric stages, reject malformed/non-finite data, and choose tolerances
from the declared storage format rather than from a subjective visual score.
"""

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools" / "compare_stage_equivalence.py"


class StageEquivalenceTests(unittest.TestCase):
    def run_tool(self, reference, candidate, fmt):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            ref = directory / "reference.json"
            got = directory / "candidate.json"
            ref.write_text(json.dumps(reference))
            got.write_text(json.dumps(candidate))
            return subprocess.run(
                [sys.executable, str(TOOL), "--reference", str(ref),
                 "--candidate", str(got), "--format", fmt],
                text=True,
                capture_output=True,
                check=False,
            )

    def test_fp16_quantization_noise_passes_with_declared_tolerance(self):
        result = self.run_tool([1.0, 0.5, -0.25], [1.001, 0.499, -0.251], "tensor_fp16")
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        self.assertTrue(report["equivalent"])
        self.assertEqual(report["format"], "tensor_fp16")
        self.assertGreater(report["tolerance"], 0.0)

    def test_large_numeric_difference_fails_without_visual_preference(self):
        result = self.run_tool([0.0, 0.25, 1.0], [0.0, 0.5, 1.0], "rgba8")
        self.assertEqual(result.returncode, 1)
        report = json.loads(result.stdout)
        self.assertFalse(report["equivalent"])
        self.assertGreater(report["max_abs_error"], report["tolerance"])

    def test_shape_and_non_finite_data_are_hard_failures(self):
        shape = self.run_tool([1.0, 2.0], [1.0], "tensor_fp16")
        self.assertEqual(shape.returncode, 2)
        self.assertIn("shape mismatch", shape.stderr)

        nonfinite = self.run_tool([1.0], [float("nan")], "tensor_fp16")
        self.assertEqual(nonfinite.returncode, 2)
        self.assertIn("non-finite", nonfinite.stderr)


if __name__ == "__main__":
    unittest.main()
