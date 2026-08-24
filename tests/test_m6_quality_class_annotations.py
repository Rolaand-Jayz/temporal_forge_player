"""Tests for visually verified, capture-free M6 quality-class annotations."""

from __future__ import annotations

import json
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "benchmarks/quality_sweeps/m6_quality_class_annotations.json"


class M6QualityClassAnnotationTests(unittest.TestCase):
    """Keep annotations grounded in inspected real-world still assets."""

    def test_manifest_has_explicit_visual_evidence_and_temporal_gap(self) -> None:
        self.assertTrue(MANIFEST.is_file())
        document = json.loads(MANIFEST.read_text(encoding="utf-8"))
        self.assertEqual(document["schemaVersion"], 1)
        self.assertEqual(document["evidenceBasis"], "visual-inspection-of-existing-stills")
        self.assertEqual(document["temporalEvidence"]["available"], False)
        self.assertTrue(
            any("adjacent" in item for item in document["temporalEvidence"]["missingEvidence"])
        )

    def test_annotations_use_only_existing_real_world_assets(self) -> None:
        document = json.loads(MANIFEST.read_text(encoding="utf-8"))
        synthetic_markers = ("synthetic", "generated", "diagnostic")
        self.assertGreater(len(document["annotations"]), 0)
        for annotation in document["annotations"]:
            path = ROOT / annotation["assetPath"]
            self.assertTrue(path.is_file(), annotation["assetPath"])
            lowered = annotation["assetPath"].lower()
            self.assertFalse(any(marker in lowered for marker in synthetic_markers))
            self.assertNotIn(annotation["scene"], {"synthetic_edges_text", "synthetic_motion"})

    def test_each_annotation_has_exact_frame_and_bounded_optional_region(self) -> None:
        document = json.loads(MANIFEST.read_text(encoding="utf-8"))
        for annotation in document["annotations"]:
            self.assertIsInstance(annotation["frame"], int)
            self.assertGreaterEqual(annotation["frame"], 0)
            self.assertTrue(annotation["qualityClass"])
            region = annotation.get("staticRegion")
            if region is None:
                continue
            for field in ("width", "height", "imageWidth", "imageHeight"):
                self.assertIsInstance(region[field], int)
                self.assertGreater(region[field], 0)
            self.assertGreaterEqual(region["x"], 0)
            self.assertGreaterEqual(region["y"], 0)
            self.assertLessEqual(region["x"] + region["width"], region["imageWidth"])
            self.assertLessEqual(region["y"] + region["height"], region["imageHeight"])

    def test_annotations_are_unique_and_do_not_claim_unverified_temporal_events(self) -> None:
        document = json.loads(MANIFEST.read_text(encoding="utf-8"))
        keys = [(item["scene"], item["frame"], item["qualityClass"]) for item in document["annotations"]]
        self.assertEqual(len(keys), len(set(keys)))
        self.assertNotIn("motionVector", document)
        self.assertNotIn("events", document)


if __name__ == "__main__":
    unittest.main()
