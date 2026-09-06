"""Contract tests for the exhaustive quality-candidate scheduler."""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "benchmarks/quality_sweeps/swarm/run_all_candidates.py"


class AllCandidateMatrixTests(unittest.TestCase):
    def test_all_mode_writes_runtime_and_quality_lab_candidates(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            manifest = Path(directory) / "all.json"
            result = subprocess.run(
                [sys.executable, str(SCRIPT), "--mode", "all", "--manifest-out", str(manifest)],
                cwd=ROOT,
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            document = json.loads(manifest.read_text(encoding="utf-8"))
        candidates = document["candidates"]
        self.assertGreater(len(candidates), 100)
        ids = [candidate["id"] for candidate in candidates]
        self.assertEqual(len(ids), len(set(ids)))
        environments = [candidate["environment"] for candidate in candidates]
        self.assertTrue(any("TFORGE_QUALITY_LAB_CONFIG" in value for value in environments))
        self.assertTrue(any("TFORGE_FSR4_CHROMA_PHASE" in value for value in environments))
        self.assertTrue(any(candidate["source"] == "runtime pairwise" for candidate in candidates))
        self.assertTrue(all(
            value.get("TFORGE_FSR4_DISABLE_NATIVE_INT8") == "1"
            for value in environments
        ))

    def test_runner_contains_every_declared_runtime_dimension(self) -> None:
        from benchmarks.quality_sweeps.swarm.run_all_candidates import RUNTIME_DIMENSIONS

        runner = (ROOT / "benchmarks/video_corpus/run_temporal_quality.sh").read_text(encoding="utf-8")
        for name, _ in RUNTIME_DIMENSIONS:
            with self.subTest(name=name):
                self.assertIn(name, runner)

    def test_pairwise_mode_does_not_replay_isolated_or_quality_lab_rows(self) -> None:
        from benchmarks.quality_sweeps.swarm.run_all_candidates import build_candidates

        candidates = build_candidates("pairwise")
        self.assertGreater(len(candidates), 6000)
        self.assertTrue(any(candidate["source"] == "runtime pairwise" for candidate in candidates))
        self.assertTrue(any(candidate["source"] == "qualityLab/runtime pairwise" for candidate in candidates))
        self.assertTrue(all(candidate["source"] in {"runtime pairwise", "qualityLab/runtime pairwise"} for candidate in candidates))

    def test_script_is_syntax_valid(self) -> None:
        result = subprocess.run([sys.executable, "-m", "py_compile", str(SCRIPT)], cwd=ROOT, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_capture_default_is_bounded_for_cpu_and_gpu_contention(self) -> None:
        source = SCRIPT.read_text(encoding="utf-8")
        self.assertIn('"--workers"', source)
        self.assertIn("default=2", source)
        self.assertIn("CPU/GPU contention", source)


if __name__ == "__main__":
    unittest.main()
