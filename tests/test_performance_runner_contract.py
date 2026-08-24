#!/usr/bin/env python3
"""Test-first contract for the M7.1 timing runner output."""

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class PerformanceRunnerContractTests(unittest.TestCase):
    """Require stage-separated timing data before performance capture."""

    def test_runner_declares_stage_separated_timing_columns(self) -> None:
        source = (ROOT / "benchmarks/video_corpus/run_performance.sh").read_text()

        header = next(
            line.strip(" '\")")
            for line in source.splitlines()
            if "clip_id,width,height" in line and "gpu_p95_ms" in line
        )

        for field in (
            "decode_mean_ms",
            "upload_mean_ms",
            "presentation_mean_ms",
            "pipeline_mean_ms",
            "gpu_mean_ms",
        ):
            self.assertIn(field, header)

    def test_runner_parses_structured_stage_timing_line(self) -> None:
        source = (ROOT / "benchmarks/video_corpus/run_performance.sh").read_text()

        self.assertIn("decodeCPU=", source)
        self.assertIn("uploadCPU=", source)
        self.assertIn("presentationCPU=", source)


if __name__ == "__main__":
    unittest.main()
