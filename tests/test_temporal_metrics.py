"""Tests for the pure temporal-quality metric primitives.

These fixtures are deliberately tiny, controlled arrays.  They validate the
meaning of each metric before any captured video is allowed to depend on it;
they are not review-corpus assets and are never shipped to human reviewers.
"""

from __future__ import annotations

import unittest


class TemporalMetricTests(unittest.TestCase):
    """Lock the metric semantics before implementing the extractor."""

    def test_static_flicker_uses_only_the_declared_static_region(self) -> None:
        from benchmarks.quality_sweeps.temporal_metrics import static_flicker

        frames = [
            [[0.0, 0.0], [0.0, 0.0]],
            [[0.0, 1.0], [0.0, 0.0]],
        ]
        static_mask = [[True, False], [True, True]]

        self.assertAlmostEqual(static_flicker(frames, static_mask), 0.0)
        self.assertAlmostEqual(static_flicker(frames), 0.25)

    def test_edge_variance_is_zero_for_a_temporally_stable_edge(self) -> None:
        from benchmarks.quality_sweeps.temporal_metrics import edge_variance

        edge = [[0.0, 1.0], [0.0, 1.0]]
        self.assertAlmostEqual(edge_variance([edge, edge]), 0.0)

    def test_edge_variance_reports_temporal_change_in_edge_energy(self) -> None:
        from benchmarks.quality_sweeps.temporal_metrics import edge_variance

        frames = [
            [[0.0, 0.0], [0.0, 1.0]],
            [[0.0, 0.0], [0.0, 0.0]],
        ]

        # The two edge pixels have values 1 then 0.  Population variance is
        # 0.25 at each of those pixels and the frame mean is 0.125.
        self.assertAlmostEqual(edge_variance(frames), 0.125)

    def test_motion_compensated_error_removes_a_declared_integer_translation(self) -> None:
        from benchmarks.quality_sweeps.temporal_metrics import motion_compensated_error

        reference = [[0.0, 0.25, 0.5, 0.75]]
        candidate = [[0.25, 0.5, 0.75, 0.75]]
        motion = [[(1.0, 0.0), (1.0, 0.0), (1.0, 0.0), (1.0, 0.0)]]

        # The motion field maps each candidate pixel to its reference sample.
        # The final pixel is excluded because its translated sample would be
        # outside the reference image.
        self.assertAlmostEqual(
            motion_compensated_error(candidate, reference, motion),
            0.0,
        )

    def test_motion_compensated_error_preserves_real_residual(self) -> None:
        from benchmarks.quality_sweeps.temporal_metrics import motion_compensated_error

        reference = [[0.0, 0.25, 0.5]]
        candidate = [[0.0, 0.5, 0.5]]
        motion = [[(0.0, 0.0), (0.0, 0.0), (0.0, 0.0)]]

        self.assertAlmostEqual(
            motion_compensated_error(candidate, reference, motion),
            1.0 / 12.0,
        )

    def test_motion_compensated_error_supports_fractional_x_and_y_sampling(self) -> None:
        from benchmarks.quality_sweeps.temporal_metrics import motion_compensated_error

        reference = [
            [0.0, 1.0, 2.0],
            [10.0, 11.0, 12.0],
            [20.0, 21.0, 22.0],
        ]
        # Candidate values are exactly the bilinear samples one half pixel
        # right and down from each interior reference coordinate. The border
        # samples that would leave the frame are intentionally omitted from
        # the score by the metric's documented boundary policy.
        candidate = [
            [5.5, 6.5, 0.0],
            [15.5, 16.5, 0.0],
            [0.0, 0.0, 0.0],
        ]
        motion = [[(0.5, 0.5)] * 3 for _ in range(3)]
        mask = [[True, True, False], [True, True, False], [False, False, False]]

        self.assertAlmostEqual(
            motion_compensated_error(candidate, reference, motion, mask),
            0.0,
        )

    def test_motion_compensated_error_rejects_a_mask_with_no_correspondence(self) -> None:
        from benchmarks.quality_sweeps.temporal_metrics import motion_compensated_error

        frame = [[0.0, 1.0], [2.0, 3.0]]
        motion = [[(0.0, 0.0), (0.0, 0.0)], [(0.0, 0.0), (0.0, 0.0)]]

        with self.assertRaisesRegex(ValueError, "at least one pixel"):
            motion_compensated_error(frame, frame, motion, [[False, False], [False, False]])

    def test_temporal_motion_error_compares_candidate_and_reference_transitions(self) -> None:
        from benchmarks.quality_sweeps.temporal_metrics import (
            temporal_motion_compensated_errors,
        )

        reference = [
            [[0.0, 1.0, 0.0, 0.0]],
            [[0.0, 0.0, 1.0, 0.0]],
        ]
        candidate = [
            [[0.0, 1.0, 0.0, 0.0]],
            [[0.0, 0.0, 0.8, 0.0]],
        ]
        # The current bright pixel at x=2 samples the previous x=1 pixel.
        motion = [
            None,
            [[(0.0, 0.0), (0.0, 0.0), (-1.0, 0.0), (0.0, 0.0)]],
        ]

        errors = temporal_motion_compensated_errors(candidate, reference, motion)

        self.assertEqual(len(errors), 1)
        self.assertAlmostEqual(errors[0], 0.05)

    def test_temporal_motion_error_preserves_an_unmeasured_reset_transition(self) -> None:
        from benchmarks.quality_sweeps.temporal_metrics import (
            temporal_motion_compensated_errors,
        )

        frames = [[[0.0]], [[1.0]], [[1.0]]]
        zero_motion = [[(0.0, 0.0)]]

        errors = temporal_motion_compensated_errors(
            frames,
            frames,
            [None, None, zero_motion],
        )

        self.assertEqual(errors, [None, 0.0])

    def test_ghost_duration_counts_the_contiguous_post_event_run(self) -> None:
        from benchmarks.quality_sweeps.temporal_metrics import ghost_duration_frames

        errors = [0.0, 0.8, 0.7, 0.6, 0.1, 0.0]
        self.assertEqual(ghost_duration_frames(errors, threshold=0.5, event_index=1), 3)
        self.assertEqual(ghost_duration_frames(errors, threshold=0.5, event_index=4), 0)

    def test_reset_recovery_returns_frames_until_threshold_is_reached(self) -> None:
        from benchmarks.quality_sweeps.temporal_metrics import reset_recovery_frames

        errors = [0.2, 0.8, 0.7, 0.2, 0.1]
        self.assertEqual(reset_recovery_frames(errors, threshold=0.3, reset_index=1), 2)
        self.assertIsNone(reset_recovery_frames([0.8, 0.7], threshold=0.3, reset_index=0))

    def test_reset_recovery_counts_from_an_unmeasured_reset_transition(self) -> None:
        from benchmarks.quality_sweeps.temporal_metrics import reset_recovery_frames

        self.assertEqual(
            reset_recovery_frames([None, 0.8, 0.2], threshold=0.3, reset_index=0),
            2,
        )


if __name__ == "__main__":
    unittest.main()
