"""Tests for compact, explicit temporal-capture metadata sidecars."""

from __future__ import annotations

import unittest


class TemporalMetadataTests(unittest.TestCase):
    """Keep static-region selection explicit without requiring giant JSON masks."""

    def test_rectangle_static_mask_expands_to_the_capture_grid(self) -> None:
        from benchmarks.quality_sweeps.temporal_sequence import load_static_mask

        mask = load_static_mask(
            {
                "schema": "temporal_forge.static_mask.v1",
                "width": 4,
                "height": 3,
                "rectangles": [{"x": 1, "y": 1, "width": 2, "height": 1}],
            },
            width=4,
            height=3,
        )

        self.assertEqual(mask, [
            [False, False, False, False],
            [False, True, True, False],
            [False, False, False, False],
        ])

    def test_static_mask_dimensions_must_match_the_captured_sequence(self) -> None:
        from benchmarks.quality_sweeps.temporal_sequence import load_static_mask

        with self.assertRaises(ValueError):
            load_static_mask(
                {
                    "schema": "temporal_forge.static_mask.v1",
                    "width": 8,
                    "height": 3,
                    "rectangles": [{"x": 0, "y": 0, "width": 1, "height": 1}],
                },
                width=4,
                height=3,
            )

    def test_empty_or_out_of_bounds_static_region_is_rejected(self) -> None:
        from benchmarks.quality_sweeps.temporal_sequence import load_static_mask

        for rectangles in ([], [{"x": 3, "y": 2, "width": 2, "height": 1}]):
            with self.assertRaises(ValueError):
                load_static_mask(
                    {
                        "schema": "temporal_forge.static_mask.v1",
                        "width": 4,
                        "height": 3,
                        "rectangles": rectangles,
                    },
                    width=4,
                    height=3,
                )

    def test_analysis_frame_indices_are_explicit_and_in_range(self) -> None:
        from benchmarks.quality_sweeps.temporal_sequence import analysis_frame_indices

        self.assertEqual(
            analysis_frame_indices({"analysisFrameIndices": [3, 4]}, frame_count=6),
            [3, 4],
        )
        self.assertEqual(
            analysis_frame_indices({}, frame_count=3),
            [0, 1, 2],
        )

    def test_analysis_frame_indices_reject_duplicates_and_out_of_range_values(self) -> None:
        from benchmarks.quality_sweeps.temporal_sequence import analysis_frame_indices

        for indices in ([2, 2], [3], []):
            with self.assertRaises(ValueError):
                analysis_frame_indices(
                    {"analysisFrameIndices": indices}, frame_count=3
                )

    def test_saved_cut_audit_window_cannot_be_applied_to_eight_frame_rows(self) -> None:
        """Do not reuse the unscoped six-frame window from the old audit JSON."""

        from benchmarks.quality_sweeps.temporal_sequence import analysis_frame_indices

        with self.assertRaisesRegex(ValueError, "out-of-range"):
            analysis_frame_indices(
                {"analysisFrameIndices": [6, 7, 8, 9, 10, 11]},
                frame_count=8,
            )
