"""Domain regression tests for dense-flow forward/backward consistency.

The exported backward vectors, the photometric error, and the occlusion mask
all live in the current-frame domain. These tests inject exact NONUNIFORM flow
fields (divergence and rotation) so a consistency term evaluated in the wrong
domain is displaced and detectable. Translation-only pairs cannot expose the
misregistration, because a constant field maps every pixel to itself.
"""

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools" / "validate_dense_motion.py"


def load_module():
    spec = importlib.util.spec_from_file_location("validate_dense_motion", MODULE_PATH)
    if spec is None or spec.loader is None:
        raise AssertionError(f"cannot load {MODULE_PATH}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def textured_frame(np, cv2, height: int, width: int):
    """Deterministic high-texture grayscale frame for photometric checks."""

    yy, xx = np.mgrid[0:height, 0:width].astype(np.float32)
    frame = (
        96.0
        + 64.0 * np.sin(xx * 0.31 + 1.7)
        + 48.0 * np.cos(yy * 0.23 - 0.4)
        + 32.0 * np.sin((xx + yy) * 0.11)
    )
    frame = np.clip(frame, 0.0, 255.0)
    return frame.astype(np.uint8)


class DenseFlowConsistencyDomainTests(unittest.TestCase):
    def setUp(self) -> None:
        try:
            import cv2  # noqa: F401
            import numpy as np
        except ImportError as error:  # pragma: no cover - environment contract
            self.skipTest(f"dense-flow dependencies unavailable: {error}")
        self.np = np
        self.cv2 = cv2
        self.module = load_module()

    def _analyze(self, previous, current, forward, backward):
        return self.module._validated_pair(
            previous,
            current,
            forward,
            backward,
            forward_backward_threshold=1.0,
            photometric_threshold=13.0 / 255.0,
        )

    def _grid(self, height, width):
        yy, xx = self.np.mgrid[0:height, 0:width].astype(self.np.float32)
        return yy, xx

    def _warp(self, frame, map_x, map_y):
        return self.cv2.remap(
            frame.astype(self.np.float32),
            map_x,
            map_y,
            self.cv2.INTER_LINEAR,
            borderMode=self.cv2.BORDER_REPLICATE,
        ).astype(self.np.uint8)

    def test_consistent_divergence_field_is_valid_in_interior(self) -> None:
        np = self.np
        height, width = 96, 128
        center = self.np.array([width / 2.0, height / 2.0], dtype=np.float32)
        yy, xx = self._grid(height, width)
        previous = textured_frame(np, self.cv2, height, width)

        # Backward (current -> previous) contracts toward the center: a
        # nonuniform divergence field. forward is its exact inverse.
        k = 0.2
        backward = self.np.dstack(
            [-k * (xx - center[0]), -k * (yy - center[1])]
        ).astype(np.float32)
        k_forward = k / (1.0 - k)
        forward = self.np.dstack(
            [k_forward * (xx - center[0]), k_forward * (yy - center[1])]
        ).astype(np.float32)
        current = self._warp(previous, xx + backward[..., 0], yy + backward[..., 1])

        pair = self._analyze(previous, current, forward, backward)
        margin = 6
        interior = self.np.zeros((height, width), dtype=bool)
        interior[margin:-margin, margin:-margin] = True
        self.assertLess(float(np.max(pair.consistency_error[interior])), 0.1)
        self.assertFalse(bool(pair.occlusion[interior].any()))
        self.assertGreater(float(np.mean(~pair.occlusion)), 0.9)

    def test_corrupted_divergence_correspondence_fails_in_place(self) -> None:
        np = self.np
        height, width = 96, 128
        center = self.np.array([width / 2.0, height / 2.0], dtype=np.float32)
        yy, xx = self._grid(height, width)
        previous = textured_frame(np, self.cv2, height, width)

        k = 0.2
        backward = self.np.dstack(
            [-k * (xx - center[0]), -k * (yy - center[1])]
        ).astype(np.float32)
        k_forward = k / (1.0 - k)
        forward = self.np.dstack(
            [k_forward * (xx - center[0]), k_forward * (yy - center[1])]
        ).astype(np.float32)
        current = self._warp(previous, xx + backward[..., 0], yy + backward[..., 1])

        # Corrupt the backward field inside a disk: the reported correspondence
        # no longer inverts forward there. The failure must be reported at the
        # corrupted CURRENT-frame pixels, not displaced into the previous-frame
        # domain as the pre-fix elementwise combination did.
        disk = (xx - 32.0) ** 2 + (yy - 48.0) ** 2 < 14.0 ** 2
        backward_corrupt = backward.copy()
        backward_corrupt[disk, 0] += 20.0

        pair = self._analyze(previous, current, forward, backward_corrupt)
        eroded_disk = self.cv2.erode(
            disk.astype(np.uint8), self.np.ones((5, 5), np.uint8)
        ).astype(bool)
        outside = (~disk) & (self.cv2.erode(
            (~disk).astype(np.uint8), self.np.ones((5, 5), np.uint8)
        ).astype(bool))

        self.assertGreater(
            float(np.median(pair.consistency_error[eroded_disk])), 5.0
        )
        self.assertTrue(bool(pair.occlusion[eroded_disk].all()))
        margin = 6
        far_outside = outside.copy()
        far_outside[:margin, :] = False
        far_outside[-margin:, :] = False
        far_outside[:, :margin] = False
        far_outside[:, -margin:] = False
        self.assertLess(float(np.median(pair.consistency_error[far_outside])), 0.1)
        self.assertFalse(bool(pair.occlusion[far_outside].any()))

    def test_backward_correspondence_leaving_frame_is_occluded(self) -> None:
        np = self.np
        height, width = 96, 128
        yy, xx = self._grid(height, width)
        previous = textured_frame(np, self.cv2, height, width)

        shift = 18.0
        backward = self.np.zeros((height, width, 2), dtype=np.float32)
        backward[..., 0] = shift
        forward = self.np.zeros((height, width, 2), dtype=np.float32)
        forward[..., 0] = -shift
        current = self._warp(previous, xx + backward[..., 0], yy + backward[..., 1])

        pair = self._analyze(previous, current, forward, backward)
        # Current pixels whose backward correspondence leaves the frame have no
        # previous-frame evidence and must be marked occluded.
        self.assertTrue(bool(pair.occlusion[:, width - 15:].all()))
        self.assertFalse(bool(pair.occlusion[10:-10, 5:width - 40].any()))

    def test_consistent_rotation_field_is_valid_in_interior(self) -> None:
        np = self.np
        height, width = 96, 128
        center = self.np.array([width / 2.0, height / 2.0], dtype=np.float32)
        yy, xx = self._grid(height, width)
        previous = textured_frame(np, self.cv2, height, width)

        theta = 0.05  # radians, rotation about the frame center
        c, s = np.cos(theta), np.sin(theta)
        dx = xx - center[0]
        dy = yy - center[1]
        backward = self.np.dstack(
            [(c - 1.0) * dx - s * dy, s * dx + (c - 1.0) * dy]
        ).astype(np.float32)
        # Exact inverse of a rotation is the opposite rotation.
        ci, si = np.cos(-theta), np.sin(-theta)
        forward = self.np.dstack(
            [(ci - 1.0) * dx - si * dy, si * dx + (ci - 1.0) * dy]
        ).astype(np.float32)
        current = self._warp(previous, xx + backward[..., 0], yy + backward[..., 1])

        pair = self._analyze(previous, current, forward, backward)
        margin = 6
        interior = self.np.zeros((height, width), dtype=bool)
        interior[margin:-margin, margin:-margin] = True
        self.assertLess(float(np.max(pair.consistency_error[interior])), 0.1)
        self.assertFalse(bool(pair.occlusion[interior].any()))


if __name__ == "__main__":
    unittest.main()
