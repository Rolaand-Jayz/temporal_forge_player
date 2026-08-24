#!/usr/bin/env python3
"""Failing-first tests for the grounded M6.2 spatial matrix join."""

from __future__ import annotations

import copy
import csv
import json
import tempfile
import unittest
from pathlib import Path

from benchmarks.quality_sweeps.spatial_matrix import SpatialMatrixError, assemble_spatial_matrix


METRICS = {
    "psnr_db": "25.466596",
    "ssim": "0.756577",
    "edge_ssim": "0.615448",
    "fsr_lowfreq_luma_mae": "0.018085",
    "fsr_lowfreq_luma_bias": "0.003483",
}

SOURCE_METRICS = {
    "fsr_psnr_db": "25.466596",
    "fsr_ssim": "0.756577",
    "fsr_edge_ssim": "0.615448",
    "fsr_lowfreq_luma_mae": "0.018085",
    "fsr_lowfreq_luma_bias": "0.003483",
}


def _campaign(root: Path) -> dict:
    candidate = {
        "id": "base_only_bilinear",
        "configPath": "config/bilinear.json",
        "configSha256": "a" * 64,
        "binarySha256": "b" * 64,
        "gitCommit": "1234567890abcdef1234567890abcdef12345678",
        "dimensions": {"source": "426x240", "output": "1920x1080"},
        "reviewAssets": [{
            "scene": "tos_daylight", "frame": 48,
            "path": "review/tos-daylight.png", "width": 1920, "height": 1080,
        }],
    }
    asset = root / "review/tos-daylight.png"
    asset.parent.mkdir(parents=True)
    asset.write_bytes(b"existing-review-asset")
    return {
        "schemaVersion": 2,
        "campaignId": "test-m6-2",
        "baselineCandidateId": candidate["id"],
        "corpus": {"manifestPath": "manifest.csv", "selection": ["tos_daylight"],
                   "syntheticFamiliesExcluded": True},
        "dimensions": {"source": "426x240", "output": "1920x1080"},
        "frame": 48,
        "quality": "high",
        "classes": ["faces-hair-skin"],
        "classSelections": {"tos_daylight": ["faces-hair-skin"]},
        "requiredMetrics": list(METRICS) + [
            "static_flicker", "edge_variance", "motion_compensated_error",
            "ghost_duration_frames", "reset_recovery_frames",
        ],
        "candidates": [candidate],
    }


def _write_evidence(
    root: Path,
    campaign: dict,
    *,
    mutate=None,
    quality_class: str | None = "faces-hair-skin",
    extra_rows: list[dict] | None = None,
) -> Path:
    csv_path = root / "quality.csv"
    row = {
        "clip_id": "tos_daylight", "width": "426", "height": "240",
        "output_width": "1920", "output_height": "1080", "quality": "high",
        "crf": "12", "frame": "48", **SOURCE_METRICS,
        "control_source_path": "/captures/tos-daylight-gpu-raw.png",
        "control_source_sha256": "c" * 64,
    }
    if quality_class is not None:
        row["class"] = quality_class
    if mutate:
        mutate(row)
    with csv_path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(row))
        writer.writeheader()
        writer.writerow(row)
        for extra in extra_rows or []:
            writer.writerow({field: extra.get(field, row.get(field, "")) for field in row})
    result = [{
        "candidateId": campaign["candidates"][0]["id"],
        "csv": str(csv_path), "exitCode": 0, "frame": 48,
        "dimensions": "426x240", "outputDimensions": "1920x1080",
        "configSource": campaign["candidates"][0]["configPath"],
        "configSha256": campaign["candidates"][0]["configSha256"],
        "binarySha256": campaign["candidates"][0]["binarySha256"],
        "gitCommit": campaign["candidates"][0]["gitCommit"],
    }]
    result_path = root / "results.json"
    result_path.write_text(json.dumps(result), encoding="utf-8")
    return result_path


class SpatialMatrixTests(unittest.TestCase):
    def test_joins_existing_row_without_recomputing_metric_text(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            campaign = _campaign(root)
            result_path = _write_evidence(root, campaign)
            matrix = assemble_spatial_matrix(campaign, result_path, root)
            self.assertEqual(len(matrix["rows"]), 1)
            row = matrix["rows"][0]
            self.assertEqual(row["candidateId"], "base_only_bilinear")
            self.assertEqual(row["qualityClass"], "faces-hair-skin")
            self.assertEqual(row["inputResolution"], "426x240")
            self.assertEqual(row["outputResolution"], "1920x1080")
            self.assertEqual(row["frame"], 48)
            self.assertEqual(row["metrics"], METRICS)
            self.assertEqual(row["reviewAsset"]["path"], "review/tos-daylight.png")
            self.assertEqual(row["metricSource"]["capturedClass"], row["qualityClass"])
            self.assertEqual(
                row["metricSource"]["controlSource"],
                {
                    "path": "/captures/tos-daylight-gpu-raw.png",
                    "sha256": "c" * 64,
                },
            )
            self.assertNotIn("control_source_path", row["metrics"])
            self.assertNotIn("control_source_sha256", row["metrics"])

    def test_legacy_scene_row_is_not_expanded_into_selected_classes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            campaign = _campaign(root)
            campaign["classes"] = ["faces-hair-skin", "fine-fabric-texture"]
            campaign["classSelections"] = {
                "tos_daylight": ["faces-hair-skin", "fine-fabric-texture"],
            }
            result_path = _write_evidence(root, campaign, quality_class=None)
            with self.assertRaisesRegex(
                SpatialMatrixError,
                "class-attributed spatial CSV",
            ):
                assemble_spatial_matrix(campaign, result_path, root)

    def test_missing_scene_row_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            campaign = _campaign(root)
            result_path = _write_evidence(root, campaign, mutate=lambda row: row.update(clip_id="other"))
            with self.assertRaisesRegex(
                SpatialMatrixError, "missing required evidence row|not selected"
            ):
                assemble_spatial_matrix(campaign, result_path, root)

    def test_missing_class_selection_mapping_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            campaign = _campaign(root)
            campaign.pop("classSelections")
            result_path = _write_evidence(root, campaign)
            with self.assertRaisesRegex(SpatialMatrixError, "classSelections"):
                assemble_spatial_matrix(campaign, result_path, root)

    def test_ambiguous_review_asset_mapping_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            campaign = _campaign(root)
            campaign["candidates"][0]["reviewAssets"].append(
                dict(campaign["candidates"][0]["reviewAssets"][0])
            )
            result_path = _write_evidence(root, campaign)
            with self.assertRaisesRegex(SpatialMatrixError, "exactly one review asset"):
                assemble_spatial_matrix(campaign, result_path, root)

    def test_unselected_synthetic_family_row_is_not_silently_ignored(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            campaign = _campaign(root)
            result_path = _write_evidence(
                root,
                campaign,
                extra_rows=[{"clip_id": "synthetic_edges_text"}],
            )
            with self.assertRaisesRegex(SpatialMatrixError, "synthetic"):
                assemble_spatial_matrix(campaign, result_path, root)

    def test_duplicate_key_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            campaign = _campaign(root)
            result_path = _write_evidence(root, campaign)
            with result_path.open(encoding="utf-8") as stream:
                results = json.load(stream)
            csv_path = Path(results[0]["csv"])
            with csv_path.open("a", encoding="utf-8") as stream:
                stream.write(
                    "tos_daylight,426,240,1920,1080,high,12,48,"
                    + ",".join(SOURCE_METRICS.values())
                    + ",/captures/tos-daylight-gpu-raw.png,"
                    + "c" * 64
                    + ",faces-hair-skin"
                    + "\n"
                )
            with self.assertRaisesRegex(SpatialMatrixError, "duplicate pairing key"):
                assemble_spatial_matrix(campaign, result_path, root)

    def test_dimension_or_provenance_drift_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            campaign = _campaign(root)
            result_path = _write_evidence(root, campaign, mutate=lambda row: row.update(output_width="1280"))
            with self.assertRaisesRegex(SpatialMatrixError, "dimensions do not match campaign"):
                assemble_spatial_matrix(campaign, result_path, root)


if __name__ == "__main__":
    unittest.main()
