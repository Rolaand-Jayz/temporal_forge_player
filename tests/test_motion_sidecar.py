"""Contract tests for the explicit causal codec-motion sidecar format."""

from __future__ import annotations

import unittest


def _valid_sidecar() -> dict[str, object]:
    """Return the smallest sidecar that describes one causal transition."""

    return {
        "schema": "temporal_forge.codec_motion.v1",
        "coordinateDomain": "current_destination_to_previous_reference",
        "motionUnits": "source_pixels",
        "sampleConvention": "destination_plus_motion",
        "sourceWidth": 2,
        "sourceHeight": 1,
        "targetWidth": 4,
        "targetHeight": 2,
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
                "vectors": [
                    {
                        "dstX": 0,
                        "dstY": 0,
                        "mvX": -1.0,
                        "mvY": 0.0,
                        "w": 2,
                        "h": 1,
                        "source": -1,
                    }
                ],
            },
        ],
    }


def _sparse_zero_motion_sidecar() -> dict[str, object]:
    """Return a sparse field where uncovered pixels resemble valid zero motion."""

    return {
        "schema": "temporal_forge.codec_motion.v1",
        "coordinateDomain": "current_destination_to_previous_reference",
        "motionUnits": "source_pixels",
        "sampleConvention": "destination_plus_motion",
        "sourceWidth": 8,
        "sourceHeight": 4,
        "targetWidth": 8,
        "targetHeight": 4,
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
                "vectors": [
                    {
                        "dstX": 0,
                        "dstY": 0,
                        "mvX": 0.0,
                        "mvY": 0.0,
                        "w": 4,
                        "h": 4,
                        "source": -1,
                    }
                ],
            },
        ],
    }


class MotionSidecarTests(unittest.TestCase):
    """Prevent ambiguous or non-causal motion from entering M6 evidence."""

    def test_sparse_expansion_exposes_validity_separate_from_zero_motion(self) -> None:
        """Uncovered pixels must not become valid identity correspondence."""
        from benchmarks.quality_sweeps.motion_sidecar import load_motion_fields

        fields = load_motion_fields(
            _sparse_zero_motion_sidecar(),
            expected_frames=2,
            target_width=8,
            target_height=4,
        )

        self.assertIsNotNone(fields[1])
        assert fields[1] is not None
        validity = fields[1].validity

        # The covered block is static, so zero motion is still valid motion.
        self.assertEqual(fields[1][0][0], (0.0, 0.0))
        self.assertTrue(validity[0][0])
        self.assertTrue(validity[3][3])

        # The uncovered half must carry an independent invalidity signal.
        self.assertEqual(fields[1][0][7], (0.0, 0.0))
        self.assertFalse(validity[0][7])
        self.assertFalse(validity[3][7])

    def test_sidecar_expands_source_pixels_to_the_captured_target_grid(self) -> None:
        from benchmarks.quality_sweeps.motion_sidecar import load_motion_fields

        fields = load_motion_fields(
            _valid_sidecar(), expected_frames=2, target_width=4, target_height=2
        )

        self.assertIsNone(fields[0])
        self.assertIsNotNone(fields[1])
        assert fields[1] is not None
        self.assertEqual(len(fields[1]), 2)
        self.assertEqual(len(fields[1][0]), 4)
        self.assertEqual(fields[1][0][0], (-2.0, 0.0))
        self.assertEqual(fields[1][1][3], (-2.0, 0.0))

    def test_raw_motion_frame_list_is_rejected_as_ambiguous(self) -> None:
        from benchmarks.quality_sweeps.motion_sidecar import load_motion_fields

        with self.assertRaises(ValueError):
            load_motion_fields(  # type: ignore[arg-type]
                {"frames": []}, expected_frames=0, target_width=1, target_height=1
            )

    def test_missing_coordinate_domain_is_rejected(self) -> None:
        from benchmarks.quality_sweeps.motion_sidecar import load_motion_fields

        sidecar = _valid_sidecar()
        del sidecar["coordinateDomain"]
        with self.assertRaises(ValueError):
            load_motion_fields(sidecar, expected_frames=2, target_width=4, target_height=2)

    def test_future_reference_vector_is_rejected(self) -> None:
        from benchmarks.quality_sweeps.motion_sidecar import load_motion_fields

        sidecar = _valid_sidecar()
        frames = sidecar["frames"]
        assert isinstance(frames, list)
        second = frames[1]
        assert isinstance(second, dict)
        vectors = second["vectors"]
        assert isinstance(vectors, list)
        vectors[0]["source"] = 1

        with self.assertRaises(ValueError):
            load_motion_fields(sidecar, expected_frames=2, target_width=4, target_height=2)

    def test_frame_pts_must_increase_in_presentation_order(self) -> None:
        """Motion evidence must not silently reorder displayed transitions."""
        from benchmarks.quality_sweeps.motion_sidecar import load_motion_fields

        for pts in (0, -1):
            with self.subTest(pts=pts):
                sidecar = _valid_sidecar()
                frames = sidecar["frames"]
                assert isinstance(frames, list)
                second = frames[1]
                assert isinstance(second, dict)
                second["ptsUs"] = pts
                with self.assertRaises(ValueError):
                    load_motion_fields(
                        sidecar, expected_frames=2, target_width=4, target_height=2
                    )

    def test_ambiguous_or_older_past_reference_is_rejected(self) -> None:
        """Only the explicit immediately-previous marker enters the causal path."""
        from benchmarks.quality_sweeps.motion_sidecar import load_motion_fields

        for source in (0, -2):
            with self.subTest(source=source):
                sidecar = _valid_sidecar()
                frames = sidecar["frames"]
                assert isinstance(frames, list)
                second = frames[1]
                assert isinstance(second, dict)
                vectors = second["vectors"]
                assert isinstance(vectors, list)
                vectors[0]["source"] = source
                with self.assertRaises(ValueError):
                    load_motion_fields(sidecar, expected_frames=2, target_width=4, target_height=2)

    def test_first_frame_cannot_claim_causal_motion(self) -> None:
        """Frame zero has no previous frame and must not provide a motion field."""
        from benchmarks.quality_sweeps.motion_sidecar import load_motion_fields

        sidecar = _valid_sidecar()
        frames = sidecar["frames"]
        assert isinstance(frames, list)
        first = frames[0]
        second = frames[1]
        assert isinstance(first, dict)
        assert isinstance(second, dict)
        first["motionAvailable"] = True
        first["vectors"] = second["vectors"]

        with self.assertRaises(ValueError):
            load_motion_fields(sidecar, expected_frames=2, target_width=4, target_height=2)

    def test_invalid_motion_availability_cannot_coexist_with_vectors(self) -> None:
        from benchmarks.quality_sweeps.motion_sidecar import load_motion_fields

        for available in (False, None, "false"):
            with self.subTest(motionAvailable=available):
                sidecar = _valid_sidecar()
                frames = sidecar["frames"]
                assert isinstance(frames, list)
                second = frames[1]
                assert isinstance(second, dict)
                second["motionAvailable"] = available

                with self.assertRaises(ValueError):
                    load_motion_fields(
                        sidecar, expected_frames=2, target_width=4, target_height=2
                    )

    def test_motion_components_must_fit_the_fp16_motion_texture_range(self) -> None:
        from benchmarks.quality_sweeps.motion_sidecar import load_motion_fields

        for component, value in (("mvX", 65505.0), ("mvY", -65505.0)):
            with self.subTest(component=component, value=value):
                sidecar = _valid_sidecar()
                frames = sidecar["frames"]
                assert isinstance(frames, list)
                second = frames[1]
                assert isinstance(second, dict)
                vectors = second["vectors"]
                assert isinstance(vectors, list)
                vector = vectors[0]
                assert isinstance(vector, dict)
                vector[component] = value

                with self.assertRaises(ValueError):
                    load_motion_fields(
                        sidecar, expected_frames=2, target_width=4, target_height=2
                    )

    def test_target_dimensions_must_match_the_captured_sequence(self) -> None:
        from benchmarks.quality_sweeps.motion_sidecar import load_motion_fields

        with self.assertRaises(ValueError):
            load_motion_fields(
                _valid_sidecar(), expected_frames=2, target_width=8, target_height=2
            )

    def test_reset_transition_may_explicitly_have_no_motion(self) -> None:
        from benchmarks.quality_sweeps.motion_sidecar import load_motion_fields

        sidecar = _valid_sidecar()
        frames = sidecar["frames"]
        assert isinstance(frames, list)
        frames.insert(1, {
            "frameIndex": 1,
            "ptsUs": 33333,
            "reset": True,
            "motionAvailable": False,
            "vectors": [],
        })
        frames[2]["frameIndex"] = 2
        frames[2]["ptsUs"] = 66666

        fields = load_motion_fields(
            sidecar, expected_frames=3, target_width=4, target_height=2
        )

        self.assertIsNone(fields[1])
        self.assertIsNotNone(fields[2])

    def test_frame_records_assemble_in_order_into_the_public_sidecar_schema(self) -> None:
        from benchmarks.quality_sweeps.motion_sidecar import assemble_motion_sidecar

        records = [
            {
                "frameIndex": 0,
                "ptsUs": 0,
                "reset": True,
                "motionAvailable": False,
                "vectors": [],
                "sourceWidth": 2,
                "sourceHeight": 1,
            },
            {
                "frameIndex": 1,
                "ptsUs": 33333,
                "reset": False,
                "motionAvailable": True,
                "vectors": _valid_sidecar()["frames"][1]["vectors"],
                "sourceWidth": 2,
                "sourceHeight": 1,
            },
        ]

        sidecar = assemble_motion_sidecar(
            records, target_width=4, target_height=2
        )

        self.assertEqual(sidecar["schema"], "temporal_forge.codec_motion.v1")
        self.assertEqual(sidecar["targetWidth"], 4)
        self.assertEqual(len(sidecar["frames"]), 2)

    def test_assembly_rejects_missing_frame_records(self) -> None:
        from benchmarks.quality_sweeps.motion_sidecar import assemble_motion_sidecar

        with self.assertRaises(ValueError):
            assemble_motion_sidecar(
                [{
                    "frameIndex": 1,
                    "ptsUs": 33333,
                    "reset": False,
                    "motionAvailable": True,
                    "vectors": [],
                    "sourceWidth": 2,
                    "sourceHeight": 1,
                }],
                target_width=4,
                target_height=2,
            )


if __name__ == "__main__":
    unittest.main()
