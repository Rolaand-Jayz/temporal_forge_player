#!/usr/bin/env python3
"""Contract tests for the grounded, spatial-only M6 schema-v2 campaign."""

from __future__ import annotations

import copy
import json
import unittest
from pathlib import Path

from benchmarks.quality_sweeps.quality_campaign_contract import CampaignError, validate_campaign


ROOT = Path(__file__).resolve().parents[1]
CAMPAIGN_PATH = ROOT / "benchmarks/quality_sweeps/m6_schema2_spatial_campaign.json"


class M6Schema2CampaignTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        with CAMPAIGN_PATH.open(encoding="utf-8") as stream:
            cls.campaign = json.load(stream)

    def test_grounded_campaign_validates(self) -> None:
        validate_campaign(self.campaign)

    def test_class_selections_preserve_scene_specific_annotations(self) -> None:
        self.assertEqual(
            self.campaign["classSelections"],
            {
                "tos_daylight": ["faces-hair-skin", "fine-fabric-texture"],
                "tos_debris": [],
                "sintel_rooftop": ["high-contrast-architecture"],
                "sintel_cave": ["low-light-shadow-detail"],
            },
        )

    def test_class_selections_match_the_checked_in_annotation_manifest(self) -> None:
        with (ROOT / "benchmarks/quality_sweeps/m6_quality_class_annotations.json").open(
            encoding="utf-8"
        ) as stream:
            annotations = json.load(stream)["annotations"]
        expected = {
            scene: [] for scene in self.campaign["corpus"]["selection"]
        }
        for annotation in annotations:
            expected[annotation["scene"]].append(annotation["qualityClass"])
            if self.campaign.get("evidenceMode", "visual_and_metrics") == "visual_and_metrics":
                self.assertTrue((ROOT / annotation["assetPath"]).is_file())
        for scene in expected:
            expected[scene] = sorted(set(expected[scene]))
        actual = {
            scene: sorted(classes)
            for scene, classes in self.campaign["classSelections"].items()
        }
        self.assertEqual(actual, expected)

    def test_temporal_completeness_is_explicitly_recorded(self) -> None:
        self.assertTrue(self.campaign["temporalEvidence"]["complete"])
        self.assertEqual(self.campaign["temporalEvidence"]["status"], "complete")
        self.assertEqual(self.campaign["temporalEvidence"]["rows"], [])
        self.assertEqual(self.campaign["temporalEvidence"]["rowCount"], 20)

    def test_scene_without_annotation_is_not_promoted_to_a_phantom_class(self) -> None:
        invalid = copy.deepcopy(self.campaign)
        invalid["classSelections"]["tos_debris"] = ["invented-class"]
        with self.assertRaises(CampaignError):
            validate_campaign(invalid)

    def test_visual_campaign_retains_review_asset_files(self) -> None:
        candidates = self.campaign["candidates"]
        self.assertEqual(len(candidates), 5)
        for candidate in candidates:
            self.assertEqual(candidate["dimensions"], {"source": "426x240", "output": "1920x1080"})
            self.assertEqual(self.campaign.get("evidenceMode"), "visual_and_metrics")


if __name__ == "__main__":
    unittest.main()
