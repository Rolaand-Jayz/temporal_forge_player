"""Tests for capture-free temporal matrix assembly."""

from __future__ import annotations

import copy
import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TEMPORAL_MATRIX = ROOT / "benchmarks/video_corpus/run_temporal_quality_matrix.sh"
TEMPORAL_RUNNER = ROOT / "benchmarks/video_corpus/run_temporal_quality.sh"


class TemporalMatrixTests(unittest.TestCase):
    def test_temporal_matrix_dry_run_describes_isolated_retry_attempts(self) -> None:
        """The matrix planner must expose retry isolation without launching the player."""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            player = root / "player"
            input_path = root / "input.mkv"
            reference_path = root / "reference.mkv"
            player.write_text("#!/usr/bin/env bash\nexit 0\n", encoding="utf-8")
            player.chmod(0o755)
            input_path.write_bytes(b"input")
            reference_path.write_bytes(b"reference")
            output_dir = root / "matrix"
            result = subprocess.run(
                [
                    str(TEMPORAL_MATRIX),
                    str(player),
                    str(input_path),
                    str(reference_path),
                    str(output_dir),
                    "8",
                    "--retries",
                    "2",
                    "--dry-run",
                ],
                cwd=ROOT,
                env=os.environ.copy(),
                capture_output=True,
                text=True,
                check=False,
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("preset=Quality attempts=0..2", result.stdout)
        self.assertIn("matrix dry run", result.stdout)
        self.assertFalse(output_dir.exists())

    def test_temporal_matrix_script_is_valid_bash(self) -> None:
        result = subprocess.run(
            ["bash", "-n", str(TEMPORAL_MATRIX)],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_temporal_runner_forwards_matrix_preset_to_player(self) -> None:
        """Preset rows must configure the player rather than only rename output files."""
        source = TEMPORAL_RUNNER.read_text(encoding="utf-8")
        self.assertIn("TFORGE_BENCHMARK_PRESET", source)

    def test_temporal_runner_forwards_experimental_phase_override(self) -> None:
        """Temporal A/B captures must preserve the spatial phase under test."""
        source = TEMPORAL_RUNNER.read_text(encoding="utf-8")
        self.assertIn("TFORGE_FSR4_EXPERIMENTAL_BASE_UNJITTERED", source)


    def test_existing_m6_campaign_has_no_temporal_rows_until_assembled(self) -> None:
        campaign = json.loads(
            (ROOT / "benchmarks/quality_sweeps/m6_schema2_spatial_campaign.json").read_text(
                encoding="utf-8"
            )
        )
        self.assertFalse(campaign["temporalEvidence"]["complete"])
        self.assertEqual(campaign["temporalEvidence"]["rows"], [])

    def test_assembly_preserves_blank_event_metrics_as_pending_evidence(self) -> None:
        from benchmarks.quality_sweeps.temporal_matrix import assemble_temporal_matrix

        campaign = {
            "schemaVersion": 2,
            "campaignId": "fixture",
            "baselineCandidateId": "baseline",
            "corpus": {"manifestPath": "manifest.csv", "selection": ["scene"]},
            "dimensions": {"source": "426x240", "output": "1920x1080"},
            "frame": 48,
            "quality": "high",
            "classes": ["class"],
            "classSelections": {"scene": ["class"]},
            "requiredMetrics": [
                "psnr_db", "ssim", "edge_ssim", "static_flicker", "edge_variance",
                "motion_compensated_error", "ghost_duration_frames", "reset_recovery_frames",
            ],
            "candidates": [{
                "id": "baseline",
                "configPath": "config/baseline.json",
                "binarySha256": "a" * 64,
                "configSha256": "b" * 64,
                "dimensions": {"source": "426x240", "output": "1920x1080"},
                "reviewAssets": [{"scene": "scene", "frame": 48, "path": "asset.png", "width": 1920, "height": 1080}],
            }],
        }
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            asset = root / "asset.png"
            asset.write_bytes(b"fixture")
            campaign["candidates"][0]["reviewAssets"][0]["path"] = str(asset)
            spatial = root / "spatial.json"
            spatial.write_text(json.dumps({
                "matrixType": "spatial",
                "rows": [{
                    "candidateId": "baseline", "scene": "scene", "qualityClass": "class",
                    "provenance": {"gitCommit": "1234567", "binarySha256": "a" * 64, "configPath": "config/baseline.json"},
                    "reviewAsset": campaign["candidates"][0]["reviewAssets"][0],
                    "metricSource": {"path": "spatial.csv"},
                    "metrics": {"fsr_psnr_db": "20", "fsr_ssim": "0.8", "fsr_edge_ssim": "0.7"},
                }],
            }), encoding="utf-8")
            csv_path = root / "temporal_metrics.csv"
            csv_path.write_text(
                "candidateId,scene,configId,startFrame,endFrame,class,frames,width,height,static_flicker,edge_variance,motion_compensated_error,ghost_duration_frames,reset_recovery_frames\n"
                "baseline,scene,config/baseline.json,48,55,class,8,1920,1080,0.1,0.2,0.3,,\n",
                encoding="utf-8",
            )
            (root / "codec_motion.json").write_text("{}", encoding="utf-8")
            (root / "static_mask.json").write_text("{}", encoding="utf-8")
            result = assemble_temporal_matrix(campaign, spatial, [csv_path], root)

        self.assertEqual(result["temporalCsvCount"], 1)
        self.assertFalse(result["complete"])
        self.assertEqual(result["temporal"][0]["metrics"]["ghost_duration_frames"], None)
        self.assertEqual(result["temporal"][0]["metrics"]["reset_recovery_frames"], None)
        self.assertEqual(result["issues"], [])
        self.assertEqual(
            result["evidenceGaps"][0]["code"],
            "missing_event_metrics",
        )
        self.assertEqual(
            result["temporalEvidence"]["unavailableMetrics"],
            ["ghost_duration_frames", "reset_recovery_frames"],
        )
        self.assertEqual(result["temporalEvidence"]["status"], "pending")

    def test_captured_candidate_config_id_is_resolved_without_relabeling(self) -> None:
        from benchmarks.quality_sweeps.temporal_matrix import assemble_temporal_matrix

        campaign = {
            "schemaVersion": 2,
            "campaignId": "fixture",
            "baselineCandidateId": "base_only_mitchell",
            "corpus": {"manifestPath": "manifest.csv", "selection": ["sintel_rooftop"]},
            "dimensions": {"source": "426x240", "output": "1920x1080"},
            "frame": 48,
            "quality": "high",
            "classes": ["high-contrast-architecture"],
            "classSelections": {"sintel_rooftop": ["high-contrast-architecture"]},
            "requiredMetrics": [
                "psnr_db", "ssim", "edge_ssim", "static_flicker", "edge_variance",
                "motion_compensated_error", "ghost_duration_frames", "reset_recovery_frames",
            ],
            "candidates": [{
                "id": "base_only_mitchell",
                "configPath": "benchmarks/quality_sweeps/stage_b/base_only_mitchell.json",
                "binarySha256": "a" * 64,
                "configSha256": "b" * 64,
                "gitCommit": "1234567",
                "dimensions": {"source": "426x240", "output": "1920x1080"},
                "reviewAssets": [{"scene": "sintel_rooftop", "frame": 48, "path": "asset.png", "width": 1920, "height": 1080}],
            }],
        }
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            asset = root / "asset.png"
            asset.write_bytes(b"fixture")
            campaign["candidates"][0]["reviewAssets"][0]["path"] = str(asset)
            spatial = root / "spatial.json"
            spatial.write_text(json.dumps({
                "matrixType": "spatial",
                "rows": [{
                    "candidateId": "base_only_mitchell", "scene": "sintel_rooftop", "qualityClass": "high-contrast-architecture",
                    "provenance": {"gitCommit": "1234567", "binarySha256": "a" * 64, "configPath": campaign["candidates"][0]["configPath"]},
                    "reviewAsset": campaign["candidates"][0]["reviewAssets"][0],
                    "metricSource": {"path": "spatial.csv"},
                    "metrics": {"fsr_psnr_db": "20", "fsr_ssim": "0.8", "fsr_edge_ssim": "0.7"},
                }],
            }), encoding="utf-8")
            csv_path = root / "temporal_metrics.csv"
            csv_path.write_text(
                "candidateId,scene,configId,startFrame,endFrame,class,frames,width,height,static_flicker,edge_variance,motion_compensated_error,ghost_duration_frames,reset_recovery_frames\n"
                "base_only_mitchell,sintel_rooftop,base_only_mitchell,48,55,high-contrast-architecture,8,1920,1080,0.1,0.2,0.3,,\n",
                encoding="utf-8",
            )
            (root / "codec_motion.json").write_text("{}", encoding="utf-8")
            (root / "static_mask.json").write_text("{}", encoding="utf-8")
            result = assemble_temporal_matrix(campaign, spatial, [csv_path], root)

        self.assertEqual(result["issues"], [])
        source = result["temporal"][0]["metricSource"]
        self.assertEqual(source["capturedConfigId"], "base_only_mitchell")
        self.assertEqual(source["configIdentityResolution"], "candidate_id_matches_campaign_candidate")
        self.assertEqual(result["temporal"][0]["provenance"]["capturedConfigId"], "base_only_mitchell")


if __name__ == "__main__":
    unittest.main()
