"""CLI contract tests for the standalone temporal-sequence measurement tool."""

from __future__ import annotations

import csv
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def _write_p6(path: Path, width: int, height: int, pixels: bytes) -> None:
    """Write a tiny RGB PPM fixture for the command-level test."""

    path.write_bytes(f"P6\n{width} {height}\n255\n".encode("ascii") + pixels)


class MeasureTemporalSequenceCliTests(unittest.TestCase):
    """Ensure the standalone entrypoint produces truthful, parseable output."""

    def test_cli_writes_temporal_metrics_csv_from_ppm_and_metadata(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            candidate_dir = root / "candidate"
            reference_dir = root / "reference"
            candidate_dir.mkdir()
            reference_dir.mkdir()
            # A two-pixel grayscale-changing sequence, represented as RGB P6.
            for number, value in enumerate((0, 128, 96, 16)):
                _write_p6(
                    candidate_dir / f"frame{number}.ppm",
                    1,
                    1,
                    bytes([value, value, value]),
                )
                _write_p6(reference_dir / f"frame{number}.ppm", 1, 1, bytes([0, 0, 0]))
            motion_path = root / "motion.json"
            motion_path.write_text(
                json.dumps({
                    "schema": "temporal_forge.codec_motion.v1",
                    "coordinateDomain": "current_destination_to_previous_reference",
                    "motionUnits": "source_pixels",
                    "sampleConvention": "destination_plus_motion",
                    "sourceWidth": 1,
                    "sourceHeight": 1,
                    "targetWidth": 1,
                    "targetHeight": 1,
                    "frames": [
                        {
                            "frameIndex": 0,
                            "ptsUs": 0,
                            "reset": True,
                            "motionAvailable": False,
                            "vectors": [],
                        },
                        *[
                            {
                                "frameIndex": index,
                                "ptsUs": index * 33333,
                                "reset": False,
                                "motionAvailable": True,
                                "vectors": [{
                                    "dstX": 0,
                                    "dstY": 0,
                                    "mvX": 0.0,
                                    "mvY": 0.0,
                                    "w": 1,
                                    "h": 1,
                                    "source": -1,
                                }],
                            }
                            for index in range(1, 4)
                        ],
                    ],
                }),
                encoding="utf-8",
            )
            events_path = root / "events.json"
            events_path.write_text(
                json.dumps({
                    "ghostEventIndex": 0,
                    "ghostThreshold": 0.3,
                    "resetIndex": 0,
                    "resetThreshold": 0.2,
                }),
                encoding="utf-8",
            )
            static_mask_path = root / "static-mask.json"
            static_mask_path.write_text(
                json.dumps({
                    "schema": "temporal_forge.static_mask.v1",
                    "width": 1,
                    "height": 1,
                    "rectangles": [{"x": 0, "y": 0, "width": 1, "height": 1}],
                }),
                encoding="utf-8",
            )
            output_path = root / "metrics.csv"

            result = subprocess.run(
                [
                    sys.executable,
                    "tools/measure_temporal_sequence.py",
                    "--class",
                    "controlled",
                    "--candidate-id",
                    "current",
                    "--scene",
                    "tos_daylight",
                    "--config-id",
                    "current-neutral",
                    "--start-frame",
                    "48",
                    "--candidate-dir",
                    str(candidate_dir),
                    "--reference-dir",
                    str(reference_dir),
                    "--motion-json",
                    str(motion_path),
                    "--events-json",
                    str(events_path),
                    "--static-mask-json",
                    str(static_mask_path),
                    "--output",
                    str(output_path),
                ],
                cwd=ROOT,
                capture_output=True,
                text=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            with output_path.open("r", newline="", encoding="utf-8") as stream:
                row = next(csv.DictReader(stream))

            self.assertEqual(row["class"], "controlled")
            self.assertEqual(row["candidateId"], "current")
            self.assertEqual(row["scene"], "tos_daylight")
            self.assertEqual(row["configId"], "current-neutral")
            self.assertEqual(row["startFrame"], "48")
            self.assertEqual(row["endFrame"], "51")
            self.assertEqual(row["frames"], "4")
            self.assertAlmostEqual(
                float(row["motion_compensated_error"]), 0.313725, places=5
            )
            self.assertEqual(row["ghost_duration_frames"], "1")
            self.assertEqual(row["reset_recovery_frames"], "1")

            from benchmarks.quality_sweeps.quality_campaign_contract import (
                validate_metrics_csv,
            )

            validate_metrics_csv(
                output_path,
                ["controlled"],
                [
                    "static_flicker",
                    "edge_variance",
                    "motion_compensated_error",
                    "ghost_duration_frames",
                    "reset_recovery_frames",
                ],
            )

    def test_cli_fails_when_motion_metadata_is_missing(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            candidate_dir = root / "candidate"
            reference_dir = root / "reference"
            candidate_dir.mkdir()
            reference_dir.mkdir()
            _write_p6(candidate_dir / "frame0.ppm", 1, 1, bytes([0, 0, 0]))
            _write_p6(reference_dir / "frame0.ppm", 1, 1, bytes([0, 0, 0]))
            static_mask_path = root / "static-mask.json"
            static_mask_path.write_text(
                json.dumps({
                    "schema": "temporal_forge.static_mask.v1",
                    "width": 1,
                    "height": 1,
                    "rectangles": [{"x": 0, "y": 0, "width": 1, "height": 1}],
                }),
                encoding="utf-8",
            )

            result = subprocess.run(
                [
                    sys.executable,
                    "tools/measure_temporal_sequence.py",
                    "--class",
                    "missing-motion",
                    "--candidate-id",
                    "current",
                    "--scene",
                    "tos_daylight",
                    "--config-id",
                    "current-neutral",
                    "--start-frame",
                    "48",
                    "--candidate-dir",
                    str(candidate_dir),
                    "--reference-dir",
                    str(reference_dir),
                    "--static-mask-json",
                    str(static_mask_path),
                    "--output",
                    str(root / "metrics.csv"),
                ],
                cwd=ROOT,
                capture_output=True,
                text=True,
                check=False,
            )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("motion", result.stderr.lower())


if __name__ == "__main__":
    unittest.main()
