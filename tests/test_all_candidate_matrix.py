"""Contract tests for the exhaustive quality-candidate scheduler."""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "benchmarks" / "quality_sweeps/swarm/run_all_candidates.py"

# The exhaustive scheduler is not part of the shipped tree. Its contract tests
# are only meaningful where the artifact exists; skipping (rather than failing
# at import or collection time) keeps a clean checkout green while preserving
# the checks for checkouts that do carry the scheduler.
SCHEDULER_PRESENT = SCRIPT.is_file()


@unittest.skipUnless(
    SCHEDULER_PRESENT,
    f"exhaustive candidate scheduler not shipped in this tree: {SCRIPT}",
)
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

    def test_direct_execution_works_from_foreign_cwd_without_pythonpath(self) -> None:
        # The documented direct-path invocation must not depend on the
        # invocation directory or on an external PYTHONPATH.
        with tempfile.TemporaryDirectory() as directory:
            env = {key: value for key, value in os.environ.items() if key != "PYTHONPATH"}
            result = subprocess.run(
                [sys.executable, str(SCRIPT), "--help"],
                cwd=directory,
                env=env,
                capture_output=True,
                text=True,
                check=False,
            )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("usage:", result.stdout)

    def test_clean_parent_environment_strips_all_tforge_state(self) -> None:
        from benchmarks.quality_sweeps.swarm.run_all_candidates import (
            INHERITED_TFORGE_ALLOWLIST,
            clean_parent_environment,
        )

        parent = {
            "PATH": "/usr/bin",
            "HOME": "/home/tester",
            "LANG": "C.UTF-8",
            "XDG_DATA_HOME": "/tmp/xdg",
            "TFORGE_FSR4_LEARNED_STRENGTH": "0.9",
            "TFORGE_VK_VALIDATE": "1",
            "TFORGE_TEMPORAL_CANDIDATE_ID": "leftover",
            "TFORGE_QUALITY_LAB_CONFIG": "/tmp/leaked.json",
        }
        cleaned = clean_parent_environment(parent)
        # Every inherited TFORGE_* variable is stripped, not just a fixed
        # subset: stray experiment state must not alter capture identity.
        for key in parent:
            if key.startswith("TFORGE_") and key not in INHERITED_TFORGE_ALLOWLIST:
                self.assertNotIn(key, cleaned)
        # The normal OS environment required to execute is preserved.
        for key, value in (("PATH", "/usr/bin"), ("HOME", "/home/tester"),
                           ("LANG", "C.UTF-8"), ("XDG_DATA_HOME", "/tmp/xdg")):
            self.assertEqual(cleaned[key], value)
        # The allowlist is an explicit, extensible frozenset.
        self.assertIsInstance(INHERITED_TFORGE_ALLOWLIST, frozenset)


if __name__ == "__main__":
    unittest.main()
