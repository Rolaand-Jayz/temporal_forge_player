"""Failing-first tests for the remaining M6 spatial contract gap.

These tests intentionally describe the next tooling boundary.  The current
runner still emits legacy ``fsr_*`` columns and no producer class column, so
they are expected to fail until the capture/assembly slice is implemented.
They do not authorize a name-only alias or class inference from scene labels.
"""

from __future__ import annotations

import csv
import json
import tempfile
import unittest
from pathlib import Path

from benchmarks.quality_sweeps.spatial_matrix import assemble_spatial_matrix


PRIMARY_SOURCE_METRICS = {
    "fsr_psnr_db": "25.466596",
    "fsr_ssim": "0.756577",
    "fsr_edge_ssim": "0.615448",
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
            "scene": "tos_daylight",
            "frame": 48,
            "path": "review/tos-daylight.png",
            "width": 1920,
            "height": 1080,
        }],
    }
    asset = root / "review/tos-daylight.png"
    asset.parent.mkdir(parents=True)
    asset.write_bytes(b"existing-review-asset")
    annotation_path = root / "m6_quality_class_annotations.json"
    annotation_path.write_text(json.dumps({
        "schemaVersion": 1,
        "evidenceBasis": "visual-inspection-of-existing-stills",
        "annotations": [{
            "scene": "tos_daylight",
            "frame": 48,
            "candidateId": "current",
            "qualityClass": "faces-hair-skin",
            "assetPath": str(asset),
            "staticRegion": {
                "x": 10,
                "y": 20,
                "width": 100,
                "height": 120,
                "imageWidth": 1920,
                "imageHeight": 1080,
            },
        }],
    }), encoding="utf-8")
    return {
        "schemaVersion": 2,
        "campaignId": "test-spatial-metric-contract-gap",
        "baselineCandidateId": candidate["id"],
        "corpus": {"manifestPath": "manifest.csv", "selection": ["tos_daylight"]},
        "dimensions": {"source": "426x240", "output": "1920x1080"},
        "frame": 48,
        "quality": "high",
        "classes": ["faces-hair-skin"],
        "classSelections": {"tos_daylight": ["faces-hair-skin"]},
        "qualityClassAnnotationsPath": str(annotation_path),
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
        "candidates": [candidate],
    }


def _write_class_attributed_result(root: Path, campaign: dict) -> Path:
    csv_path = root / "quality.csv"
    fields = [
        "clip_id",
        "width",
        "height",
        "output_width",
        "output_height",
        "quality",
        "crf",
        "frame",
        *PRIMARY_SOURCE_METRICS,
        "fsr_lowfreq_luma_mae",
        "fsr_lowfreq_luma_bias",
        "lanczos_ssim",
        "custom_source_metric",
        "class",
    ]
    row = {
        "clip_id": "tos_daylight",
        "width": "426",
        "height": "240",
        "output_width": "1920",
        "output_height": "1080",
        "quality": "high",
        "crf": "12",
        "frame": "48",
        **PRIMARY_SOURCE_METRICS,
        "fsr_lowfreq_luma_mae": "0.018085",
        "fsr_lowfreq_luma_bias": "0.003483",
        "lanczos_ssim": "0.701234",
        "custom_source_metric": "0.42",
        "class": "faces-hair-skin",
    }
    with csv_path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerow(row)

    candidate = campaign["candidates"][0]
    result_path = root / "results.json"
    result_path.write_text(json.dumps([{
        "candidateId": candidate["id"],
        "csv": str(csv_path),
        "exitCode": 0,
        "frame": 48,
        "dimensions": "426x240",
        "outputDimensions": "1920x1080",
        "configSource": candidate["configPath"],
        "configSha256": candidate["configSha256"],
        "binarySha256": candidate["binarySha256"],
        "gitCommit": candidate["gitCommit"],
    }]), encoding="utf-8")
    return result_path


class SpatialMetricContractGapTests(unittest.TestCase):
    def test_source_backed_primary_metrics_are_exposed_under_schema_v2_names(self) -> None:
        """Only the three primary runner metrics may cross this boundary.

        The runner source establishes the equivalence: PSNR average, SSIM All,
        and edge-detect SSIM All.  The implementation must make that mapping
        explicit before schema-v2 validation; it must not infer it from a
        shared suffix or use comparator deltas.
        """
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            campaign = _campaign(root)
            result_path = _write_class_attributed_result(root, campaign)
            matrix = assemble_spatial_matrix(campaign, result_path, root)

        metrics = matrix["rows"][0]["metrics"]
        self.assertEqual(
            {name: metrics.get(name) for name in ("psnr_db", "ssim", "edge_ssim")},
            {
                "psnr_db": PRIMARY_SOURCE_METRICS["fsr_psnr_db"],
                "ssim": PRIMARY_SOURCE_METRICS["fsr_ssim"],
                "edge_ssim": PRIMARY_SOURCE_METRICS["fsr_edge_ssim"],
            },
        )
        self.assertEqual(metrics["fsr_lowfreq_luma_mae"], "0.018085")
        self.assertEqual(metrics["fsr_lowfreq_luma_bias"], "0.003483")
        self.assertEqual(metrics["lanczos_ssim"], "0.701234")
        self.assertEqual(metrics["custom_source_metric"], "0.42")
        self.assertNotIn("fsr_psnr_db", metrics)

    def test_quality_runner_emits_producer_class_for_spatial_rows(self) -> None:
        """A scene name or visual annotation cannot substitute for this field."""
        script = Path(__file__).parents[1] / "benchmarks/video_corpus/run_quality.sh"
        header = next(
            line.strip().strip("'")
            for line in script.read_text(encoding="utf-8").splitlines()
            if "fsr_psnr_db" in line and "clip_id" in line
        )
        self.assertIn("class", header.split(","))

    def test_five_candidates_emit_exactly_one_row_for_each_selected_class(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            scenes = {
                "tos_daylight": ["faces-hair-skin", "fine-fabric-texture"],
                "tos_debris": [],
                "sintel_rooftop": ["high-contrast-architecture"],
                "sintel_cave": ["low-light-shadow-detail"],
            }
            campaign = _campaign(root)
            campaign["corpus"]["selection"] = list(scenes)
            campaign["classes"] = sorted({item for values in scenes.values() for item in values})
            campaign["classSelections"] = scenes
            annotations = []
            for scene, classes in scenes.items():
                for index, quality_class in enumerate(classes):
                    asset = root / f"review/{scene}-{index}.png"
                    asset.write_bytes(b"review-asset")
                    annotations.append({
                        "scene": scene,
                        "frame": 48,
                        "candidateId": "current",
                        "qualityClass": quality_class,
                        "assetPath": str(asset),
                        "staticRegion": {
                            "x": index * 100,
                            "y": 100,
                            "width": 600,
                            "height": 500,
                            "imageWidth": 1920,
                            "imageHeight": 1080,
                        },
                    })
            Path(campaign["qualityClassAnnotationsPath"]).write_text(
                json.dumps({"schemaVersion": 1, "annotations": annotations}),
                encoding="utf-8",
            )
            candidates = []
            for index in range(5):
                candidate = json.loads(json.dumps(campaign["candidates"][0]))
                candidate["id"] = f"candidate-{index}"
                candidate["configSha256"] = str(index) * 64
                candidate["binarySha256"] = chr(ord("f") - index) * 64
                candidate["gitCommit"] = (f"{index + 1:040x}")
                candidate["reviewAssets"] = []
                for scene in scenes:
                    asset = root / f"review/{candidate['id']}-{scene}.png"
                    asset.write_bytes(b"candidate-review-asset")
                    candidate["reviewAssets"].append({
                        "scene": scene,
                        "frame": 48,
                        "path": str(asset),
                        "width": 1920,
                        "height": 1080,
                    })
                candidates.append(candidate)
            campaign["candidates"] = candidates
            campaign["baselineCandidateId"] = candidates[0]["id"]

            result_entries = []
            for candidate in candidates:
                csv_path = root / f"{candidate['id']}.csv"
                fields = ["clip_id", "width", "height", "output_width", "output_height",
                          "quality", "crf", "frame", *PRIMARY_SOURCE_METRICS,
                          "fsr_lowfreq_luma_mae", "fsr_lowfreq_luma_bias", "class"]
                with csv_path.open("w", newline="", encoding="utf-8") as stream:
                    writer = csv.DictWriter(stream, fieldnames=fields)
                    writer.writeheader()
                    for scene, classes in scenes.items():
                        for quality_class in classes:
                            writer.writerow({
                                "clip_id": scene,
                                "width": "426",
                                "height": "240",
                                "output_width": "1920",
                                "output_height": "1080",
                                "quality": "high",
                                "crf": "12",
                                "frame": "48",
                                **PRIMARY_SOURCE_METRICS,
                                "fsr_lowfreq_luma_mae": "0.018085",
                                "fsr_lowfreq_luma_bias": "0.003483",
                                "class": quality_class,
                            })
                result_entries.append({
                    "candidateId": candidate["id"],
                    "csv": str(csv_path),
                    "exitCode": 0,
                    "frame": 48,
                    "dimensions": "426x240",
                    "outputDimensions": "1920x1080",
                    "configSource": candidate["configPath"],
                    "configSha256": candidate["configSha256"],
                    "binarySha256": candidate["binarySha256"],
                    "gitCommit": candidate["gitCommit"],
                })
            result_path = root / "results.json"
            result_path.write_text(json.dumps(result_entries), encoding="utf-8")

            matrix = assemble_spatial_matrix(campaign, result_path, root)

        expected = sum(len(values) for values in scenes.values()) * 5
        self.assertEqual(len(matrix["rows"]), expected)
        self.assertEqual(
            {(row["candidateId"], row["scene"], row["qualityClass"]) for row in matrix["rows"]},
            {
                (candidate_id, scene, quality_class)
                for candidate_id in (candidate["id"] for candidate in candidates)
                for scene, classes in scenes.items()
                for quality_class in classes
            },
        )

    def test_annotation_source_rejects_ambiguous_class_region_mapping(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            campaign = _campaign(root)
            annotations_path = Path(campaign["qualityClassAnnotationsPath"])
            document = json.loads(annotations_path.read_text(encoding="utf-8"))
            document["annotations"].append(dict(document["annotations"][0]))
            annotations_path.write_text(json.dumps(document), encoding="utf-8")
            result_path = _write_class_attributed_result(root, campaign)
            with self.assertRaisesRegex(Exception, "ambiguous|duplicate"):
                assemble_spatial_matrix(campaign, result_path, root)

    def test_annotation_source_rejects_missing_selected_class(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            campaign = _campaign(root)
            annotations_path = Path(campaign["qualityClassAnnotationsPath"])
            document = json.loads(annotations_path.read_text(encoding="utf-8"))
            document["annotations"] = []
            annotations_path.write_text(json.dumps(document), encoding="utf-8")
            result_path = _write_class_attributed_result(root, campaign)
            with self.assertRaisesRegex(Exception, "missing class-region mapping"):
                assemble_spatial_matrix(campaign, result_path, root)

    def test_annotation_source_rejects_synthetic_scene(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            campaign = _campaign(root)
            annotations_path = Path(campaign["qualityClassAnnotationsPath"])
            document = json.loads(annotations_path.read_text(encoding="utf-8"))
            document["annotations"][0]["scene"] = "synthetic_edges_text"
            annotations_path.write_text(json.dumps(document), encoding="utf-8")
            result_path = _write_class_attributed_result(root, campaign)
            with self.assertRaisesRegex(Exception, "synthetic"):
                assemble_spatial_matrix(campaign, result_path, root)

    def test_legacy_whole_scene_row_is_rejected_even_when_a_class_is_selected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            campaign = _campaign(root)
            result_path = _write_class_attributed_result(root, campaign)
            results = json.loads(result_path.read_text(encoding="utf-8"))
            csv_path = Path(results[0]["csv"])
            text = csv_path.read_text(encoding="utf-8").replace(
                "faces-hair-skin\n", "__whole_scene__\n"
            )
            csv_path.write_text(text, encoding="utf-8")
            with self.assertRaisesRegex(Exception, "unexpected class|whole.scene|selected"):
                assemble_spatial_matrix(campaign, result_path, root)


if __name__ == "__main__":
    unittest.main()
