"""Failing-first tests for dense motion validity reaching temporal metrics.

The sidecar loader keeps block coverage separate from numeric motion because a
valid static vector is indistinguishable from an uncovered zero-initialized
pixel.  These tests lock the required hand-off before production metrics are
changed: invalid pixels must not contribute, and a transition with no valid
pixels must not become fabricated zero-motion evidence.
"""

from __future__ import annotations

import unittest


def _partially_covered_sidecar() -> dict[str, object]:
    """Return a two-frame sidecar covering only the first target pixel."""

    return {
        "schema": "temporal_forge.codec_motion.v1",
        "coordinateDomain": "current_destination_to_previous_reference",
        "motionUnits": "source_pixels",
        "sampleConvention": "destination_plus_motion",
        "sourceWidth": 2,
        "sourceHeight": 1,
        "targetWidth": 2,
        "targetHeight": 1,
        "frames": [
            {
                "frameIndex": 0,
                "ptsUs": 0,
                "reset": True,
                "motionAvailable": False,
                "vectors": [],
            },
            {
                "frameIndex": 1,
                "ptsUs": 33333,
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
            },
        ],
    }


class MotionValidityMetricsContractTests(unittest.TestCase):
    """Prevent sparse sidecar coverage from contaminating temporal metrics."""

    def test_temporal_metric_excludes_uncovered_zero_motion_pixels(self) -> None:
        """A changed uncovered pixel must not affect the measured transition."""
        from benchmarks.quality_sweeps.motion_sidecar import load_motion_fields
        from benchmarks.quality_sweeps.temporal_metrics import (
            temporal_motion_compensated_errors,
        )

        fields = load_motion_fields(
            _partially_covered_sidecar(),
            expected_frames=2,
            target_width=2,
            target_height=1,
        )
        candidate = [[[0.0, 0.0]], [[0.0, 1.0]]]
        reference = [[[0.0, 0.0]], [[0.0, 0.0]]]

        errors = temporal_motion_compensated_errors(candidate, reference, fields)

        # Only x=0 has a valid correspondence and its temporal residual is 0.
        self.assertEqual(errors, [0.0])

    def test_temporal_metric_rejects_transition_with_no_valid_pixels(self) -> None:
        """An all-invalid dense field must fail rather than score as identity motion."""
        from benchmarks.quality_sweeps.motion_sidecar import MotionFieldWithValidity
        from benchmarks.quality_sweeps.temporal_metrics import (
            temporal_motion_compensated_errors,
        )

        invalid_field = MotionFieldWithValidity(
            [[(0.0, 0.0), (0.0, 0.0)]],
            [[False, False]],
        )

        with self.assertRaisesRegex(ValueError, "valid"):
            temporal_motion_compensated_errors(
                [[[0.0, 0.0]], [[0.0, 0.0]]],
                [[[0.0, 0.0]], [[0.0, 0.0]]],
                [None, invalid_field],
            )


if __name__ == "__main__":
    unittest.main()
