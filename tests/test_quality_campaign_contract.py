"""M6 tests for corrected learned-campaign provenance and metric coverage."""

from __future__ import annotations

import copy
import csv
import json
import tempfile
import unittest
from pathlib import Path
import subprocess
import sys


VALID = {
    "schemaVersion": 2,
    "campaignId": "m6-corrected-learned",
    "baselineCandidateId": "spatial-bilinear",
    "corpus": {
        "manifestPath": "benchmarks/video_corpus/manifest.csv",
        "selection": ["tos_daylight", "tos_debris", "sintel_rooftop"],
    },
    "classes": ["natural_daylight", "foliage", "text_ui", "occlusion"],
    "classSelections": {
        "tos_daylight": ["natural_daylight", "foliage", "text_ui", "occlusion"],
        "tos_debris": ["natural_daylight", "foliage", "text_ui", "occlusion"],
        "sintel_rooftop": ["natural_daylight", "foliage", "text_ui", "occlusion"],
    },
    "candidates": [
        {
            "id": "spatial-bilinear",
            "configPath": "config/quality_lab.json",
            "dimensions": {"source": "426x240", "output": "1920x1080"},
            "binarySha256": "a" * 64,
            "reviewAssets": [
                {
                    "scene": "tos_daylight",
                    "frame": 62,
                    "path": "review/tos_daylight_frame62.png",
                    "width": 1920,
                    "height": 1080,
                }
            ],
        }
    ],
    "requiredMetrics": [
        "psnr_db",
        "ssim",
        "edge_ssim",
        "static_flicker",
        "edge_variance",
        "motion_compensated_error",
        "ghost_duration_frames",
        "reset_recovery_frames",
    ],
}


class QualityCampaignContractTests(unittest.TestCase):
    def test_data_only_experiment_record_is_not_failed_for_pruned_output(self) -> None:
        from tools.audit_quality_campaign_evidence import inspect_record

        with tempfile.TemporaryDirectory() as directory:
            record_path = Path(directory) / "experiment.json"
            record_path.write_text(json.dumps({
                "schema": "temporal_forge.quality_experiment.v2",
                "experiment_id": "exp-1", "run_id": "run-1", "status": "complete",
                "output_artifact": str(Path(directory) / "candidate.png"),
                "output_sha256": "a" * 64, "output_retained": False,
                "runtime_trace": {"run_id": "run-1"}, "metrics": {"ssim": "0.9"},
                "binary_sha256": "b" * 64, "config_sha256": "c" * 64,
                "source_sha256": "d" * 64,
            }), encoding="utf-8")

            status, reasons, _ = inspect_record(record_path)

            self.assertEqual(status, "VALID")
            self.assertNotIn("output artifact unavailable", " ".join(reasons))

    def test_data_only_record_without_output_hash_is_incomplete(self) -> None:
        from tools.audit_quality_campaign_evidence import inspect_record

        with tempfile.TemporaryDirectory() as directory:
            record_path = Path(directory) / "experiment.json"
            record_path.write_text(json.dumps({
                "schema": "temporal_forge.quality_experiment.v2",
                "experiment_id": "exp-1", "run_id": "run-1", "status": "complete",
                "output_artifact": str(Path(directory) / "candidate.png"),
                "output_retained": False,
                "runtime_trace": {"run_id": "run-1"}, "metrics": {"ssim": "0.9"},
                "binary_sha256": "b" * 64, "config_sha256": "c" * 64,
                "source_sha256": "d" * 64,
            }), encoding="utf-8")

            status, reasons, _ = inspect_record(record_path)

            self.assertEqual(status, "INCOMPLETE PROVENANCE")
            self.assertIn("data-only output hash is missing or malformed", reasons)

    def test_audit_rejects_cross_method_reuse_without_explicit_alias(self) -> None:
        from tools.audit_quality_campaign_evidence import audit

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for method in ("current_cas20", "fsr_direct_cas20"):
                scene_root = root / method / "tos_daylight"
                scene_root.mkdir(parents=True)
                (scene_root / "experiment.json").write_text(json.dumps({
                    "schema": "temporal_forge.quality_experiment.v2",
                    "experiment_id": "same-experiment", "run_id": "same-run",
                    "arm_id": method, "status": "complete",
                    "output_artifact": str(scene_root / "candidate.png"),
                    "output_sha256": "a" * 64, "output_retained": False,
                    "runtime_trace": {"run_id": "same-run"}, "metrics": {"ssim": "0.9"},
                    "binary_sha256": "b" * 64, "config_sha256": "c" * 64,
                    "source_sha256": "d" * 64,
                }), encoding="utf-8")

            result = audit(root)

            self.assertEqual(result["methods"]["current_cas20"], "DUPLICATED ARM")
            self.assertEqual(result["methods"]["fsr_direct_cas20"], "DUPLICATED ARM")
    """Keep M6 candidate evidence complete and human-reviewable."""

    def test_complete_campaign_is_valid(self) -> None:
        from benchmarks.quality_sweeps.quality_campaign_contract import validate_campaign

        validate_campaign(VALID)

    def test_metrics_only_campaign_does_not_require_review_images(self) -> None:
        from benchmarks.quality_sweeps.quality_campaign_contract import validate_campaign

        metrics_only = copy.deepcopy(VALID)
        metrics_only["evidenceMode"] = "metrics_only"
        metrics_only["candidates"][0].pop("reviewAssets")
        validate_campaign(metrics_only)

    def test_unknown_evidence_mode_is_rejected(self) -> None:
        from benchmarks.quality_sweeps.quality_campaign_contract import CampaignError, validate_campaign

        invalid = copy.deepcopy(VALID)
        invalid["evidenceMode"] = "numbers_only_but_unverified"
        with self.assertRaises(CampaignError):
            validate_campaign(invalid)

    def test_candidate_requires_binary_and_dimensions(self) -> None:
        from benchmarks.quality_sweeps.quality_campaign_contract import CampaignError, validate_campaign

        invalid = copy.deepcopy(VALID)
        del invalid["candidates"][0]["binarySha256"]
        with self.assertRaises(CampaignError):
            validate_campaign(invalid)

    def test_candidate_requires_config_identity_and_each_dimension(self) -> None:
        from benchmarks.quality_sweeps.quality_campaign_contract import CampaignError, validate_campaign

        for field in ("configPath",):
            invalid = copy.deepcopy(VALID)
            invalid["candidates"][0].pop(field)
            with self.subTest(field=field), self.assertRaises(CampaignError):
                validate_campaign(invalid)
        for dimension in ("source", "output"):
            invalid = copy.deepcopy(VALID)
            invalid["candidates"][0]["dimensions"].pop(dimension)
            with self.subTest(dimension=dimension), self.assertRaises(CampaignError):
                validate_campaign(invalid)

    def test_temporal_metrics_cannot_be_dropped(self) -> None:
        from benchmarks.quality_sweeps.quality_campaign_contract import CampaignError, validate_metric_row

        row = {name: 0.5 for name in VALID["requiredMetrics"]}
        del row["ghost_duration_frames"]
        with self.assertRaises(CampaignError):
            validate_metric_row(row, VALID["requiredMetrics"])

    def test_campaign_requires_every_temporal_metric(self) -> None:
        from benchmarks.quality_sweeps.quality_campaign_contract import CampaignError, validate_campaign

        invalid = copy.deepcopy(VALID)
        invalid["requiredMetrics"].remove("edge_variance")
        with self.assertRaises(CampaignError):
            validate_campaign(invalid)

    def test_synthetic_review_asset_is_rejected(self) -> None:
        from benchmarks.quality_sweeps.quality_campaign_contract import CampaignError, validate_campaign

        invalid = copy.deepcopy(VALID)
        invalid["candidates"][0]["reviewAssets"][0]["scene"] = "synthetic_motion"
        with self.assertRaises(CampaignError):
            validate_campaign(invalid)

    def test_synthetic_corpus_selection_is_rejected(self) -> None:
        from benchmarks.quality_sweeps.quality_campaign_contract import CampaignError, validate_campaign

        invalid = copy.deepcopy(VALID)
        invalid["corpus"]["selection"].append("synthetic_edges_text")
        with self.assertRaises(CampaignError):
            validate_campaign(invalid)

    def test_metric_csv_must_cover_every_declared_class(self) -> None:
        from benchmarks.quality_sweeps.quality_campaign_contract import (
            CampaignError,
            validate_metrics_csv,
        )

        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "metrics.csv"
            with path.open("w", newline="", encoding="utf-8") as stream:
                writer = csv.DictWriter(
                    stream,
                    fieldnames=["class", *VALID["requiredMetrics"]],
                )
                writer.writeheader()
                writer.writerow({
                    "class": "natural_daylight",
                    **{name: "0.5" for name in VALID["requiredMetrics"]},
                })
            with self.assertRaises(CampaignError):
                validate_metrics_csv(path, VALID["classes"], VALID["requiredMetrics"])

    def test_campaign_verifier_accepts_the_class_attributed_matrix(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            campaign_path = root / "campaign.json"
            campaign_path.write_text(__import__("json").dumps(VALID), encoding="utf-8")
            rows = []
            for scene in VALID["corpus"]["selection"]:
                for scene_class in VALID["classes"]:
                    rows.append({
                        "candidateId": "spatial-bilinear",
                        "scene": scene,
                        "qualityClass": scene_class,
                        "inputResolution": "426x240",
                        "outputResolution": "1920x1080",
                        "frame": 48,
                        "provenance": {
                            "gitCommit": "1234567890abcdef1234567890abcdef12345678",
                            "binarySha256": "a" * 64,
                            "configPath": "config/quality_lab.json",
                        },
                        "metrics": {
                            name: 0.5 for name in VALID["requiredMetrics"]
                        },
                    })
            matrix_path = root / "matrix.json"
            matrix_path.write_text(
                __import__("json").dumps({"spatial": rows, "temporal": rows}),
                encoding="utf-8",
            )
            command = [
                sys.executable,
                "tools/verify_quality_campaign.py",
                str(campaign_path),
                str(matrix_path),
            ]
            result = subprocess.run(command, capture_output=True, text=True, check=False)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_campaign_verifier_rejects_legacy_csv_input(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            campaign_path = root / "campaign.json"
            campaign_path.write_text(__import__("json").dumps(VALID), encoding="utf-8")
            csv_path = root / "metrics.csv"
            with csv_path.open("w", newline="", encoding="utf-8") as stream:
                writer = csv.DictWriter(
                    stream,
                    fieldnames=["class", *VALID["requiredMetrics"]],
                )
                writer.writeheader()
                for scene_class in VALID["classes"]:
                    writer.writerow({
                        "class": scene_class,
                        **{name: "0.5" for name in VALID["requiredMetrics"]},
                    })
            result = subprocess.run(
                [
                    sys.executable,
                    "tools/verify_quality_campaign.py",
                    str(campaign_path),
                    "--metrics",
                    f"spatial-bilinear={csv_path}",
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("schema-v2 matrix", result.stderr)

    def test_runner_plan_preserves_each_candidate_output_dimension(self) -> None:
        from benchmarks.quality_sweeps.quality_campaign_contract import runner_plans

        plans = runner_plans(VALID)
        self.assertEqual(len(plans), 1)
        self.assertEqual(plans[0]["dimensions"], "426x240")
        self.assertEqual(plans[0]["outputDimensions"], "1920x1080")
        self.assertEqual(plans[0]["experiments"][0]["config"], "config/quality_lab.json")
        self.assertEqual(
            plans[0]["corpusManifest"],
            "benchmarks/video_corpus/manifest.csv",
        )

    def test_runner_plan_preserves_declared_capture_environment(self) -> None:
        """Schema-v2 settings must reach the isolated candidate sweep."""
        from benchmarks.quality_sweeps.quality_campaign_contract import runner_plans

        campaign = copy.deepcopy(VALID)
        campaign["environment"] = {
            "TFORGE_FSR4_ENABLE_COLOR_HISTORY": "1",
            "TFORGE_FSR4_ENABLE_RECURRENT": "1",
        }
        campaign["candidates"][0]["environment"] = {
            "TFORGE_FSR4_LEARNED_STRENGTH": "0.25"
        }
        plan = runner_plans(campaign)[0]
        self.assertEqual(plan["environment"], campaign["environment"])
        self.assertEqual(
            plan["experiments"][0]["environment"],
            campaign["candidates"][0]["environment"],
        )

    def test_runner_plan_emits_bash_compatible_scene_selector(self) -> None:
        """The shell runner receives POSIX ERE, not a Python/PCRE selector."""
        from benchmarks.quality_sweeps.quality_campaign_contract import runner_plans

        selector = runner_plans(VALID)[0]["clipRegex"]
        self.assertEqual(
            selector,
            "^(tos_daylight|tos_debris|sintel_rooftop)$",
        )
        self.assertNotIn("(?:", selector)
        for scene in (*VALID["corpus"]["selection"], "synthetic_edges_text"):
            expected = scene in VALID["corpus"]["selection"]
            result = subprocess.run(
                ["bash", "-c", '[[ "$1" =~ $2 ]]', "bash", scene, selector],
                check=False,
            )
            self.assertEqual(result.returncode == 0, expected, scene)


if __name__ == "__main__":
    unittest.main()
