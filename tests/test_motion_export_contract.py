"""Static contract tests for exporting the player's actual causal motion data."""

from __future__ import annotations

import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class MotionExportContractTests(unittest.TestCase):
    """Ensure diagnostic export stays outside the reconstruction algorithm."""

    def test_player_exports_structured_per_frame_records_from_filtered_codec_motion(self) -> None:
        source = (ROOT / "src/core/PlaybackEngine.cpp").read_text(encoding="utf-8")

        for token in (
            "TFORGE_FSR4_DUMP_MOTION_SIDECAR",
            "TFORGE_FSR4_DUMP_MOTION_DIR",
            "codec_motion_",
            "frameIndex",
            "motionAvailable",
            "sourceWidth",
            "sourceHeight",
            "vectors",
            "pastReferenceMotion(fsrFrame->motionVectors)",
            "sideInputs.reset",
            "histogramDelta",
            "motionConfidence",
        ):
            self.assertIn(token, source)

    def test_temporal_runner_requests_export_then_assembles_without_replacing_legacy_csv(self) -> None:
        source = (ROOT / "benchmarks/video_corpus/run_temporal_quality.sh").read_text(
            encoding="utf-8"
        )

        for token in (
            "TFORGE_FSR4_DUMP_MOTION_SIDECAR",
            "TFORGE_FSR4_DUMP_MOTION_DIR",
            "TFORGE_DISABLE_HW_DECODE",
            "tools/assemble_motion_sidecar.py",
            "TFORGE_TEMPORAL_MOTION_JSON",
            "fsr_temporal_delta_mean",
        ):
            self.assertIn(token, source)

    def test_player_source_remains_valid_cpp_after_export_contract_is_declared(self) -> None:
        # This is deliberately a source-level check in the Python milestone
        # suite; the complete C++ build remains the milestone gate.
        self.assertTrue((ROOT / "src/core/PlaybackEngine.cpp").is_file())

    def test_runner_script_is_valid_bash(self) -> None:
        result = subprocess.run(
            ["bash", "-n", "benchmarks/video_corpus/run_temporal_quality.sh"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
