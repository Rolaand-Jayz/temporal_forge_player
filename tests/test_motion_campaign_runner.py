"""Failure-path and alignment contract tests for the motion campaign runner."""

from __future__ import annotations

import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "benchmarks" / "quality_sweeps" / "run_motion_campaign.py"


def load_module():
    spec = importlib.util.spec_from_file_location("run_motion_campaign", SCRIPT)
    if spec is None or spec.loader is None:
        raise AssertionError(f"cannot load {SCRIPT}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def fake_corpus(directory: Path) -> Path:
    """Minimal corpus layout so the planner can enumerate keys."""

    (directory / "clips").mkdir(parents=True)
    (directory / "references").mkdir(parents=True)
    for scene in ("tos_daylight",):
        for tier in ("426x240",):
            (directory / "clips" / f"{scene}_{tier}_high_crf12.mp4").touch()
        reference = directory / "references" / f"{scene}_2160p_lossless.mkv"
        reference.write_bytes(b"\0" * 8192)
    return directory


class MotionCampaignRunnerTests(unittest.TestCase):
    def run_main(self, module, argv: list[str]) -> int:
        original = sys.argv
        sys.argv = [str(SCRIPT)] + argv
        try:
            return module.main()
        finally:
            sys.argv = original

    def test_dry_run_plans_without_execution_only_artifacts(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            corpus = fake_corpus(root / "corpus")
            module.CORPUS = corpus
            output = root / "out"
            code = self.run_main(module, [
                    "--output-root", str(output),
                    "--player", str(root / "missing_player"),
                    "--config", str(root / "missing_config.json"),
                    "--scenes", "tos_daylight",
                    "--tiers", "426x240",
                    "--arms", "zero",
                ])
            self.assertEqual(code, 0)
            rows = [json.loads(line) for line in
                    (output / "manifest.jsonl").read_text(encoding="utf-8").splitlines()]
            self.assertTrue(rows)
            self.assertTrue(all(row["status"] == "planned" for row in rows))

    def test_executed_capture_failure_returns_nonzero(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            corpus = fake_corpus(root / "corpus")
            module.CORPUS = corpus
            # A non-executable stub player makes the capture runner fail
            # immediately without touching the GPU or real media.
            player = root / "stub_player"
            player.write_text("#!/bin/sh\nexit 0\n")
            config = root / "config.json"
            config.write_text("{}")
            code = self.run_main(module, [
                    "--run",
                    "--output-root", str(root / "out"),
                    "--player", str(player),
                    "--config", str(config),
                    "--scenes", "tos_daylight",
                    "--tiers", "426x240",
                    "--arms", "zero",
                    "--frames", "2",
                    "--no-confidence",
                ])
            self.assertNotEqual(code, 0)
            rows = [json.loads(line) for line in
                    (root / "out" / "manifest.jsonl").read_text(encoding="utf-8").splitlines()]
            self.assertTrue(any(row["status"].startswith("failed:") for row in rows))

    def test_offline_dense_and_start_frame_follow_configured_warmup(self) -> None:
        source = SCRIPT.read_text(encoding="utf-8")
        # The dumped window starts at the warmup frame; both the event-trace
        # identity and the offline dense-flow window must use it instead of
        # silently replaying source frames from 0.
        self.assertIn('"TFORGE_TEMPORAL_START_FRAME": str(args.warmup)', source)
        self.assertIn('"--start-frame", str(args.warmup)', source)

    def test_runner_is_syntax_valid(self) -> None:
        result = subprocess.run(
            [sys.executable, "-m", "py_compile", str(SCRIPT)],
            cwd=ROOT, capture_output=True, text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
