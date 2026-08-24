"""Capture-free tests for the grounded M6 schema-v2 gap report."""

from __future__ import annotations

import csv
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
LEGACY_MANIFEST = ROOT / "benchmarks/quality_sweeps/m6_6_1_real_spatial_controls.json"


class QualityMatrixGapReportTests(unittest.TestCase):
    """Prove the report exposes evidence and refuses to invent campaign fields."""

    def test_legacy_manifest_reports_grounded_selection_and_missing_schema_fields(self) -> None:
        from tools.report_quality_matrix_gaps import build_gap_report

        report = build_gap_report(LEGACY_MANIFEST)

        self.assertFalse(report["readyForSchemaVersion2"])
        self.assertEqual(report["grounded"]["candidateIds"], [
            "current",
            "base_only_bilinear",
            "base_only_mitchell",
            "base_only_catmull_rom",
            "base_only_lanczos2",
        ])
        self.assertEqual(report["grounded"]["scenes"], [
            "tos_daylight",
            "tos_debris",
            "sintel_rooftop",
            "sintel_cave",
        ])
        self.assertEqual(report["grounded"]["inputResolution"], "426x240")
        self.assertEqual(report["grounded"]["outputResolution"], "1920x1080")
        self.assertEqual(report["grounded"]["frame"], 48)
        self.assertIn("campaign.schemaVersion", report["missingKeys"])
        self.assertIn("campaign.classes", report["missingKeys"])
        self.assertIn("candidates.current.binarySha256", report["missingKeys"])
        self.assertIn("matrix.spatial.rows[*].qualityClass", report["missingKeys"])
        self.assertIn("matrix.temporal.rows[*].provenance.gitCommit", report["missingKeys"])

    def test_temporal_labels_do_not_become_declared_quality_classes(self) -> None:
        from tools.report_quality_matrix_gaps import build_gap_report

        with tempfile.TemporaryDirectory() as directory:
            metrics = Path(directory) / "metrics.csv"
            with metrics.open("w", newline="", encoding="utf-8") as stream:
                writer = csv.DictWriter(
                    stream,
                    fieldnames=[
                        "class",
                        "frames",
                        "width",
                        "height",
                        "static_flicker",
                        "edge_variance",
                        "motion_compensated_error",
                        "ghost_duration_frames",
                        "reset_recovery_frames",
                    ],
                )
                writer.writeheader()
                writer.writerow({
                    "class": "tos_daylight",
                    "frames": "8",
                    "width": "1920",
                    "height": "1080",
                    "static_flicker": "0.01",
                    "edge_variance": "0.02",
                    "motion_compensated_error": "0.03",
                    "ghost_duration_frames": "",
                    "reset_recovery_frames": "",
                })

            report = build_gap_report(LEGACY_MANIFEST, temporal_metrics=[metrics])

        self.assertEqual(report["observed"]["temporalLabels"], ["tos_daylight"])
        self.assertEqual(report["grounded"]["qualityClasses"], [])
        self.assertIn("campaign.classes", report["missingKeys"])
        self.assertIn("matrix.temporal.rows[*].candidateId", report["missingKeys"])
        self.assertIn("matrix.temporal.rows[*].qualityClass", report["missingKeys"])

    def test_sidecar_presence_is_not_candidate_attribution(self) -> None:
        from tools.report_quality_matrix_gaps import build_gap_report

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            motion = root / "motion.json"
            mask = root / "mask.json"
            motion.write_text(json.dumps({"schema": "temporal_forge.codec_motion.v1"}), encoding="utf-8")
            mask.write_text(json.dumps({"schema": "temporal_forge.static_mask.v1"}), encoding="utf-8")

            report = build_gap_report(
                LEGACY_MANIFEST,
                motion_sidecars=[motion],
                static_masks=[mask],
            )

        self.assertEqual(report["observed"]["motionSidecars"], [str(motion)])
        self.assertEqual(report["observed"]["staticMaskSidecars"], [str(mask)])
        self.assertIn("matrix.temporal.rows[*].provenance.gitCommit", report["missingKeys"])
        self.assertIn("matrix.temporal.rows[*].sidecars.candidateId", report["missingKeys"])

    def test_spatial_result_digest_is_joined_to_its_candidate(self) -> None:
        from tools.report_quality_matrix_gaps import build_gap_report

        with tempfile.TemporaryDirectory() as directory:
            result_path = Path(directory) / "result.json"
            result_path.write_text(
                json.dumps({
                    "candidateId": "current",
                    "binarySha256": "a" * 64,
                    "configSource": "stage_a/current.json",
                    "frame": 48,
                    "metrics": {"clips": [{
                        "clip_id": "tos_daylight",
                        "frame": "48",
                        "width": "426",
                        "height": "240",
                        "output_width": "1920",
                        "output_height": "1080",
                    }]},
                }),
                encoding="utf-8",
            )

            report = build_gap_report(LEGACY_MANIFEST, spatial_results=[result_path])

        self.assertNotIn("candidates.current.binarySha256", report["missingKeys"])

    def test_spatial_coverage_counts_unique_candidate_scene_pairs(self) -> None:
        from tools.report_quality_matrix_gaps import build_gap_report

        with tempfile.TemporaryDirectory() as directory:
            result_path = Path(directory) / "partial-result.json"
            result_path.write_text(
                json.dumps({
                    "candidateId": "current",
                    "binarySha256": "b" * 64,
                    "configSource": "stage_a/current.json",
                    "frame": 48,
                    "metrics": {"clips": [
                        {"clip_id": "tos_daylight", "frame": "48"},
                        {"clip_id": "tos_debris", "frame": "48"},
                    ]},
                }),
                encoding="utf-8",
            )
            second_result_path = Path(directory) / "second-partial-result.json"
            second_result_path.write_text(
                json.dumps({
                    "candidateId": "base_only_bilinear",
                    "binarySha256": "c" * 64,
                    "configSource": "stage_a/base_only_bilinear.json",
                    "frame": 48,
                    "metrics": {"clips": [
                        {"clip_id": "tos_daylight", "frame": "48"},
                    ]},
                }),
                encoding="utf-8",
            )

            report = build_gap_report(
                LEGACY_MANIFEST,
                spatial_results=[result_path, second_result_path],
            )

        self.assertEqual(report["coverage"]["groundedSpatialCandidateScenePairs"], 3)
        self.assertEqual(report["coverage"]["requiredSpatialCandidateScenePairs"], 20)

    def test_cli_is_capture_free_and_emits_machine_readable_gaps(self) -> None:
        command = [
            sys.executable,
            str(ROOT / "tools/report_quality_matrix_gaps.py"),
            str(LEGACY_MANIFEST),
        ]
        result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, check=False)

        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        self.assertFalse(report["readyForSchemaVersion2"])
        self.assertEqual(report["observed"]["captureInvocations"], 0)


if __name__ == "__main__":
    unittest.main()
