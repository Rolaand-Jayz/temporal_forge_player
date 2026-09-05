"""Contract tests for the Phase 2 dense-motion diagnostic.

The diagnostic is campaign tooling, not a playback dependency. These tests
lock its output shape before implementation: a caller must receive dense
forward/backward fields, a confidence mask, an occlusion mask, and a report
that identifies the exact source window and thresholds used.
"""

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools" / "validate_dense_motion.py"


def load_module():
    """Load the diagnostic module directly without making tools a package."""

    spec = importlib.util.spec_from_file_location("validate_dense_motion", MODULE_PATH)
    if spec is None or spec.loader is None:
        raise AssertionError(f"cannot load {MODULE_PATH}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class DenseMotionDiagnosticTests(unittest.TestCase):
    def test_module_exposes_a_deterministic_pair_analyzer(self) -> None:
        module = load_module()
        self.assertTrue(callable(module.analyze_frames))

    def test_pair_analyzer_returns_dense_fields_and_masks(self) -> None:
        module = load_module()
        try:
            import numpy as np
        except ImportError as error:  # pragma: no cover - environment contract
            self.skipTest(f"numpy unavailable: {error}")

        previous = np.zeros((24, 32), dtype=np.uint8)
        current = previous.copy()
        current[7:15, 10:18] = 255
        previous[7:15, 8:16] = 255
        result = module.analyze_frames(
            previous,
            current,
            forward_backward_threshold=1.0,
            photometric_threshold=13.0 / 255.0,
        )

        self.assertEqual(result.forward.shape, (24, 32, 2))
        self.assertEqual(result.backward.shape, (24, 32, 2))
        self.assertEqual(result.confidence.shape, (24, 32))
        self.assertEqual(result.occlusion.shape, (24, 32))
        self.assertGreaterEqual(float(result.confidence.min()), 0.0)
        self.assertLessEqual(float(result.confidence.max()), 1.0)
        self.assertEqual(result.occlusion.dtype, np.bool_)

    def test_backward_flow_warps_current_pixels_to_previous_coordinates(self) -> None:
        module = load_module()
        try:
            import cv2
            import numpy as np
        except ImportError as error:  # pragma: no cover - environment contract
            self.skipTest(f"dense-flow dependencies unavailable: {error}")

        previous = np.zeros((48, 64), dtype=np.uint8)
        previous[16:32, 16:32] = 255
        current = np.zeros_like(previous)
        current[16:32, 20:36] = 255
        result = module.analyze_frames(
            previous,
            current,
            forward_backward_threshold=1.0,
            photometric_threshold=13.0 / 255.0,
        )

        # The object moved right by four pixels. Current->previous flow is
        # therefore negative, and the correct reprojection is current + flow.
        self.assertLess(float(result.photometric_error[20, 24]), 0.10)

    def test_pair_analyzer_can_select_dis_flow_for_a_separate_diagnostic(self) -> None:
        module = load_module()
        try:
            import numpy as np
        except ImportError as error:  # pragma: no cover - environment contract
            self.skipTest(f"numpy unavailable: {error}")

        previous = np.zeros((32, 48), dtype=np.uint8)
        previous[10:22, 12:24] = 255
        current = np.zeros_like(previous)
        current[10:22, 15:27] = 255
        result = module.analyze_frames(
            previous,
            current,
            forward_backward_threshold=1.0,
            photometric_threshold=13.0 / 255.0,
            method="dis",
        )

        self.assertEqual(result.forward.shape, (32, 48, 2))
        self.assertEqual(result.backward.shape, (32, 48, 2))

    def test_dis_analyzer_accepts_explicit_quality_settings(self) -> None:
        module = load_module()
        try:
            import numpy as np
        except ImportError as error:  # pragma: no cover - environment contract
            self.skipTest(f"numpy unavailable: {error}")

        previous = np.zeros((24, 32), dtype=np.uint8)
        current = previous.copy()
        current[8:16, 12:20] = 255
        previous[8:16, 10:18] = 255
        result = module.analyze_frames(
            previous,
            current,
            forward_backward_threshold=1.0,
            photometric_threshold=13.0 / 255.0,
            method="dis",
            dis_finest_scale=0,
            dis_variational_refinement_iterations=5,
        )

        self.assertEqual(result.backward.shape, (24, 32, 2))

    def test_pair_analyzer_can_select_tvl1_flow_for_a_separate_diagnostic(self) -> None:
        module = load_module()
        try:
            import numpy as np
        except ImportError as error:  # pragma: no cover - environment contract
            self.skipTest(f"numpy unavailable: {error}")

        previous = np.zeros((32, 48), dtype=np.uint8)
        previous[10:22, 12:24] = 255
        current = np.zeros_like(previous)
        current[10:22, 15:27] = 255
        result = module.analyze_frames(
            previous,
            current,
            forward_backward_threshold=1.0,
            photometric_threshold=13.0 / 255.0,
            method="tvl1",
        )

        self.assertEqual(result.forward.shape, (32, 48, 2))
        self.assertEqual(result.backward.shape, (32, 48, 2))

    def test_replay_export_uses_only_validated_source_tiles(self) -> None:
        module = load_module()
        try:
            import numpy as np
        except ImportError as error:  # pragma: no cover - environment contract
            self.skipTest(f"numpy unavailable: {error}")

        result = module.PairAnalysis(
            forward=np.zeros((8, 8, 2), dtype=np.float32),
            backward=np.dstack(
                [np.full((8, 8), 1.5, dtype=np.float32), np.zeros((8, 8), dtype=np.float32)]
            ),
            confidence=np.ones((8, 8), dtype=np.float32),
            occlusion=np.zeros((8, 8), dtype=np.bool_),
            consistency_error=np.zeros((8, 8), dtype=np.float32),
            photometric_error=np.zeros((8, 8), dtype=np.float32),
        )
        sidecar = module.replay_frame_vectors(
            result, frame_index=7, source_width=8, source_height=8, tile_size=4
        )
        self.assertEqual(sidecar["frameIndex"], 7)
        self.assertTrue(sidecar["vectors"])
        self.assertEqual(sidecar["vectors"][0]["mvX"], 1.5)
        self.assertEqual(sidecar["vectors"][0]["source"], -1)
        self.assertIn("confidence", sidecar["vectors"][0])
        self.assertEqual(sidecar["vectors"][0]["confidence"], 1.0)


if __name__ == "__main__":
    unittest.main()
