"""Tests for turning captured P6 sequences into honest temporal metrics."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path


def _write_p6(path: Path, width: int, height: int, pixels: bytes) -> None:
    """Write the smallest valid 8-bit RGB PPM needed by the parser tests."""

    path.write_bytes(f"P6\n{width} {height}\n255\n".encode("ascii") + pixels)


class TemporalSequenceTests(unittest.TestCase):
    """Keep PPM ingestion and event metadata requirements explicit."""

    def test_p6_reader_normalizes_rgb_pixels_to_luminance(self) -> None:
        from benchmarks.quality_sweeps.temporal_sequence import read_p6

        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "frame.ppm"
            _write_p6(path, 2, 1, bytes([255, 0, 0, 0, 255, 0]))

            frame = read_p6(path)

        self.assertEqual(len(frame), 1)
        self.assertEqual(len(frame[0]), 2)
        self.assertAlmostEqual(frame[0][0], 0.2126, places=4)
        self.assertAlmostEqual(frame[0][1], 0.7152, places=4)

    def test_sequence_loader_uses_natural_frame_order(self) -> None:
        from benchmarks.quality_sweeps.temporal_sequence import load_p6_sequence

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for number in (10, 2, 1):
                _write_p6(root / f"frame{number}.ppm", 1, 1, bytes([number, 0, 0]))

            frames = load_p6_sequence(root)

        self.assertEqual(len(frames), 3)
        self.assertLess(frames[0][0][0], frames[1][0][0])
        self.assertLess(frames[1][0][0], frames[2][0][0])

    def test_measurement_requires_motion_fields_instead_of_using_identity(self) -> None:
        from benchmarks.quality_sweeps.temporal_sequence import measure_temporal_sequence

        frames = [[[0.0, 0.0]], [[0.0, 0.5]]]
        with self.assertRaises(ValueError):
            measure_temporal_sequence(frames, frames)

    def test_measurement_uses_explicit_motion_and_event_metadata(self) -> None:
        from benchmarks.quality_sweeps.temporal_sequence import measure_temporal_sequence

        reference = [[[0.0, 0.0]] for _ in range(4)]
        candidate = [
            [[0.0, 0.0]],
            [[0.0, 0.8]],
            [[0.0, 0.7]],
            [[0.0, 0.1]],
        ]
        zero_motion = [[(0.0, 0.0), (0.0, 0.0)]]

        metrics = measure_temporal_sequence(
            candidate,
            reference,
            temporal_motion_fields=[None, zero_motion, zero_motion, zero_motion],
            static_mask=[[True, False]],
            events={
                "ghostEventIndex": 0,
                "ghostThreshold": 0.3,
                "resetIndex": 0,
                "resetThreshold": 0.2,
            },
        )

        self.assertAlmostEqual(metrics["static_flicker"], 0.0)
        self.assertAlmostEqual(metrics["motion_compensated_error"], 0.25)
        self.assertEqual(metrics["ghost_duration_frames"], 1)
        self.assertEqual(metrics["reset_recovery_frames"], 1)

    def test_missing_event_metadata_is_unavailable_not_fabricated(self) -> None:
        from benchmarks.quality_sweeps.temporal_sequence import measure_temporal_sequence

        frame = [[0.0, 0.0]]
        motion = [[(0.0, 0.0), (0.0, 0.0)]]
        metrics = measure_temporal_sequence(
            [frame, frame],
            [frame, frame],
            temporal_motion_fields=[None, motion],
            static_mask=[[True, True]],
        )

        self.assertIsNone(metrics["ghost_duration_frames"])
        self.assertIsNone(metrics["reset_recovery_frames"])

    def test_measurement_ignores_unavailable_reset_transitions_when_averaging(self) -> None:
        """A reset gap must not become a fake numeric temporal error."""
        from benchmarks.quality_sweeps.temporal_sequence import measure_temporal_sequence

        frame = [[0.0, 0.0]]
        motion = [[(0.0, 0.0), (0.0, 0.0)]]
        metrics = measure_temporal_sequence(
            [frame, frame, frame],
            [frame, frame, frame],
            temporal_motion_fields=[None, motion, None],
            static_mask=[[True, True]],
        )

        self.assertEqual(metrics["motion_compensated_error"], 0.0)

    def test_non_object_event_metadata_is_rejected(self) -> None:
        from benchmarks.quality_sweeps.temporal_sequence import measure_temporal_sequence

        frame = [[0.0]]
        motion = [[(0.0, 0.0)]]
        with self.assertRaises(ValueError):
            measure_temporal_sequence(
                [frame],
                [frame],
                temporal_motion_fields=[None],
                events=[],  # type: ignore[arg-type]
            )

    def test_event_analysis_window_controls_stable_metrics_only(self) -> None:
        from benchmarks.quality_sweeps.temporal_sequence import measure_temporal_sequence

        frames = [
            [[0.0, 0.0]],
            [[1.0, 0.0]],
            [[1.0, 0.0]],
            [[1.0, 0.0]],
        ]
        zero_motion = [[(0.0, 0.0), (0.0, 0.0)]]
        motion = [None, zero_motion, zero_motion, zero_motion]

        metrics = measure_temporal_sequence(
            frames,
            frames,
            temporal_motion_fields=motion,
            events={"analysisFrameIndices": [1, 2, 3]},
            static_mask=[[True, True]],
        )

        self.assertAlmostEqual(metrics["static_flicker"], 0.0)

    def test_measurement_rejects_same_frame_motion_as_non_causal(self) -> None:
        from benchmarks.quality_sweeps.temporal_sequence import measure_temporal_sequence

        with self.assertRaisesRegex(ValueError, "causal"):
            measure_temporal_sequence(
                [[[0.0]], [[0.0]]],
                [[[0.0]], [[0.0]]],
                motion_fields=[[[[0.0, 0.0]]], [[[0.0, 0.0]]]],
                static_mask=[[True]],
            )

    def test_measurement_requires_explicit_static_mask(self) -> None:
        from benchmarks.quality_sweeps.temporal_sequence import measure_temporal_sequence

        with self.assertRaisesRegex(ValueError, "static mask"):
            measure_temporal_sequence(
                [[[0.0]], [[0.0]]],
                [[[0.0]], [[0.0]]],
                temporal_motion_fields=[None, [[(0.0, 0.0)]]],
            )


if __name__ == "__main__":
    unittest.main()
