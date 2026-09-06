"""M6.5 tests for complete spatial/temporal campaign matrix coverage."""

from __future__ import annotations

import copy
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REQUIRED_METRICS = [
    "psnr_db",
    "ssim",
    "edge_ssim",
    "static_flicker",
    "edge_variance",
    "motion_compensated_error",
    "ghost_duration_frames",
    "reset_recovery_frames",
]


def _campaign() -> dict:
    candidates = []
    for candidate_id, binary in (("baseline", "a"), ("learned", "b")):
        candidates.append(
            {
                "id": candidate_id,
                "configPath": f"config/{candidate_id}.json",
                "dimensions": {"source": "426x240", "output": "1920x1080"},
                "binarySha256": binary * 64,
                "reviewAssets": [
                    {
                        "scene": "tos_daylight",
                        "frame": 62,
                        "path": f"review/{candidate_id}.png",
                        "width": 1920,
                        "height": 1080,
                    }
                ],
            }
        )
    return {
        "schemaVersion": 2,
        "campaignId": "m6-matrix-fixture",
        "baselineCandidateId": "baseline",
        "corpus": {
            "manifestPath": "benchmarks/video_corpus/manifest.csv",
            "selection": ["tos_daylight", "tos_debris"],
        },
        "classes": ["daylight", "occlusion"],
        "classSelections": {
            "tos_daylight": ["daylight", "occlusion"],
            "tos_debris": ["daylight", "occlusion"],
        },
        "requiredMetrics": REQUIRED_METRICS,
        "candidates": candidates,
    }


def _rows(campaign: dict) -> list[dict]:
    rows = []
    for candidate in campaign["candidates"]:
        for scene in campaign["corpus"]["selection"]:
            scene_classes = campaign.get("classSelections", {}).get(
                scene, campaign["classes"]
            )
            for quality_class in scene_classes:
                rows.append(
                    {
                        "candidateId": candidate["id"],
                        "scene": scene,
                        "qualityClass": quality_class,
                        "inputResolution": "426x240",
                        "outputResolution": "1920x1080",
                        "frame": 62,
                        "provenance": {
                            "gitCommit": "1234567890abcdef1234567890abcdef12345678",
                            "binarySha256": candidate["binarySha256"],
                            "configPath": candidate["configPath"],
                        },
                        "metrics": {metric: 1.0 for metric in REQUIRED_METRICS},
                    }
                )
    return rows


class QualityCampaignMatrixTests(unittest.TestCase):
    """Reject plausible-looking but incomplete campaign joins."""

    def test_complete_spatial_and_temporal_matrix_is_valid(self) -> None:
        from benchmarks.quality_sweeps.campaign_matrix import validate_complete_matrix

        campaign = _campaign()
        rows = _rows(campaign)
        validate_complete_matrix(campaign, rows, copy.deepcopy(rows))

    def test_unrecorded_git_provenance_is_explicitly_allowed(self) -> None:
        from benchmarks.quality_sweeps.campaign_matrix import validate_complete_matrix

        campaign = _campaign()
        campaign["executionProvenance"] = {
            "gitCommit": None,
            "gitCommitStatus": "unrecorded",
            "gitCommitAuthoritative": False,
        }
        for candidate in campaign["candidates"]:
            candidate["gitCommit"] = None
        rows = _rows(campaign)
        for row in rows:
            row["provenance"]["gitCommit"] = None

        validate_complete_matrix(campaign, rows, copy.deepcopy(rows))

    def test_scene_specific_class_selection_does_not_require_cartesian_rows(self) -> None:
        from benchmarks.quality_sweeps.campaign_matrix import validate_complete_matrix

        campaign = _campaign()
        campaign["classSelections"] = {
            "tos_daylight": ["daylight"],
            "tos_debris": [],
        }
        rows = _rows(campaign)
        self.assertEqual(len(rows), 2)
        self.assertEqual(
            {(row["scene"], row["qualityClass"]) for row in rows},
            {("tos_daylight", "daylight")},
        )
        self.assertTrue(
            all(row["qualityClass"] in campaign["classSelections"][row["scene"]] for row in rows)
        )
        validate_complete_matrix(campaign, rows, copy.deepcopy(rows))

    def test_missing_class_selection_mapping_is_rejected_instead_of_expanding_classes(self) -> None:
        from benchmarks.quality_sweeps.campaign_matrix import MatrixError, validate_complete_matrix

        campaign = _campaign()
        campaign.pop("classSelections")
        rows = _rows(campaign)
        with self.assertRaisesRegex(MatrixError, "classSelections"):
            validate_complete_matrix(campaign, rows, copy.deepcopy(rows))

    def test_missing_temporal_key_is_rejected(self) -> None:
        from benchmarks.quality_sweeps.campaign_matrix import MatrixError, validate_complete_matrix

        campaign = _campaign()
        rows = _rows(campaign)
        with self.assertRaises(MatrixError):
            validate_complete_matrix(campaign, rows, rows[:-1])

    def test_duplicate_spatial_key_is_rejected(self) -> None:
        from benchmarks.quality_sweeps.campaign_matrix import MatrixError, validate_complete_matrix

        campaign = _campaign()
        rows = _rows(campaign)
        with self.assertRaises(MatrixError):
            validate_complete_matrix(campaign, rows + [copy.deepcopy(rows[0])], rows)

    def test_dimension_drift_is_rejected(self) -> None:
        from benchmarks.quality_sweeps.campaign_matrix import MatrixError, validate_complete_matrix

        campaign = _campaign()
        rows = _rows(campaign)
        rows[0]["outputResolution"] = "1280x720"
        with self.assertRaises(MatrixError):
            validate_complete_matrix(campaign, rows, rows)

    def test_undeclared_quality_class_is_rejected(self) -> None:
        from benchmarks.quality_sweeps.campaign_matrix import MatrixError, validate_complete_matrix

        campaign = _campaign()
        rows = _rows(campaign)
        rows[0]["qualityClass"] = "not-declared"
        with self.assertRaises(MatrixError):
            validate_complete_matrix(campaign, rows, rows)

    def test_incomplete_provenance_is_rejected(self) -> None:
        from benchmarks.quality_sweeps.campaign_matrix import MatrixError, validate_complete_matrix

        campaign = _campaign()
        rows = _rows(campaign)
        del rows[0]["provenance"]["configPath"]
        with self.assertRaises(MatrixError):
            validate_complete_matrix(campaign, rows, rows)

    def test_multiple_frames_cannot_multiply_one_coverage_key(self) -> None:
        from benchmarks.quality_sweeps.campaign_matrix import MatrixError, validate_complete_matrix

        campaign = _campaign()
        rows = _rows(campaign)
        duplicate_coverage = copy.deepcopy(rows[0])
        duplicate_coverage["frame"] = 63
        with self.assertRaises(MatrixError):
            validate_complete_matrix(campaign, rows + [duplicate_coverage], rows + [duplicate_coverage])

    def test_provenance_drift_is_rejected(self) -> None:
        from benchmarks.quality_sweeps.campaign_matrix import MatrixError, validate_complete_matrix

        campaign = _campaign()
        rows = _rows(campaign)
        rows[0]["provenance"]["binarySha256"] = "c" * 64
        with self.assertRaises(MatrixError):
            validate_complete_matrix(campaign, rows, rows)

    def test_pending_event_metrics_are_rejected_by_complete_verifier(self) -> None:
        from benchmarks.quality_sweeps.campaign_matrix import MatrixError, validate_complete_matrix

        campaign = _campaign()
        rows = _rows(campaign)
        rows[0]["metrics"]["ghost_duration_frames"] = None
        with self.assertRaises(MatrixError):
            validate_complete_matrix(campaign, rows, copy.deepcopy(rows))

    def test_standalone_matrix_verifier_accepts_complete_json_artifact(self) -> None:
        campaign = _campaign()
        rows = _rows(campaign)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            campaign_path = root / "campaign.json"
            matrix_path = root / "matrix.json"
            campaign_path.write_text(json.dumps(campaign), encoding="utf-8")
            matrix_path.write_text(
                json.dumps({"spatial": rows, "temporal": rows}),
                encoding="utf-8",
            )
            result = subprocess.run(
                [
                    sys.executable,
                    "tools/verify_quality_matrix.py",
                    str(campaign_path),
                    str(matrix_path),
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
