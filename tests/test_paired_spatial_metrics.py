"""M6.2 tests for strict paired spatial-candidate comparisons."""

from __future__ import annotations

import csv
import tempfile
import unittest
from pathlib import Path


FIELDS = [
    "clip_id",
    "width",
    "height",
    "output_width",
    "output_height",
    "quality",
    "crf",
    "frame",
    "fsr_psnr_db",
    "fsr_ssim",
    "fsr_edge_ssim",
    "fsr_lowfreq_luma_mae",
    "fsr_lowfreq_luma_bias",
    "control_source_path",
    "control_source_sha256",
]


def write_rows(path: Path, rows: list[dict[str, object]]) -> None:
    """Write the smallest real-shaped CSV accepted by the pairing contract."""
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=FIELDS)
        writer.writeheader()
        writer.writerows(rows)


def row(clip_id: str, *, ssim: float, mae: float, output_width: int = 1920) -> dict[str, object]:
    """Create one deterministic paired metric row."""
    return {
        "clip_id": clip_id,
        "width": 426,
        "height": 240,
        "output_width": output_width,
        "output_height": 1080,
        "quality": "high",
        "crf": 12,
        "frame": 48,
        "fsr_psnr_db": 25.0,
        "fsr_ssim": ssim,
        "fsr_edge_ssim": ssim + 0.1,
        "fsr_lowfreq_luma_mae": mae,
        "fsr_lowfreq_luma_bias": 0.01,
        "control_source_path": f"/captures/{clip_id}-gpu-raw.png",
        "control_source_sha256": "a" * 64,
    }


class PairedSpatialMetricTests(unittest.TestCase):
    """Prevent M6 rankings from hiding unmatched or class-specific regressions."""

    def test_pairing_reports_per_clip_deltas_and_mean_median_worst(self) -> None:
        """The report must preserve paired rows and robust aggregate summaries."""
        from benchmarks.quality_sweeps.paired_spatial_metrics import pair_spatial_metrics

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            baseline = root / "baseline.csv"
            candidate = root / "candidate.csv"
            write_rows(
                baseline,
                [
                    row("a", ssim=0.50, mae=0.10),
                    row("b", ssim=0.90, mae=0.20),
                    row("c", ssim=0.70, mae=0.30),
                ],
            )
            write_rows(
                candidate,
                [
                    row("a", ssim=0.60, mae=0.08),
                    row("b", ssim=0.80, mae=0.25),
                    row("c", ssim=0.65, mae=0.32),
                ],
            )
            report = pair_spatial_metrics(
                baseline,
                candidate,
                baseline_id="base_only_bilinear",
                candidate_id="current",
            )

        self.assertEqual(report["rowCount"], 3)
        self.assertEqual([item["clipId"] for item in report["rows"]], ["a", "b", "c"])
        self.assertAlmostEqual(report["rows"][0]["delta"]["fsr_ssim"], 0.10)
        self.assertAlmostEqual(report["summary"]["mean"]["fsr_ssim"], -0.0166666667)
        self.assertAlmostEqual(report["summary"]["median"]["fsr_ssim"], -0.05)
        self.assertAlmostEqual(report["summary"]["worst"]["fsr_ssim"], -0.10)
        self.assertAlmostEqual(report["summary"]["worst"]["fsr_lowfreq_luma_mae"], 0.05)
        self.assertEqual(
            report["rows"][0]["baselineControlSource"]["sha256"], "a" * 64
        )
        self.assertEqual(
            report["rows"][0]["candidateControlSource"]["sha256"], "a" * 64
        )

    def test_pairing_rejects_different_control_source_pixels(self) -> None:
        """Matching clip labels cannot hide different decoded source pixels."""
        from benchmarks.quality_sweeps.paired_spatial_metrics import (
            SpatialPairingError,
            pair_spatial_metrics,
        )

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            baseline = root / "baseline.csv"
            candidate = root / "candidate.csv"
            baseline_row = row("a", ssim=0.5, mae=0.1)
            candidate_row = row("a", ssim=0.5, mae=0.1)
            candidate_row["control_source_sha256"] = "b" * 64
            write_rows(baseline, [baseline_row])
            write_rows(candidate, [candidate_row])
            with self.assertRaisesRegex(SpatialPairingError, "control source pixels differ"):
                pair_spatial_metrics(
                    baseline,
                    candidate,
                    baseline_id="base",
                    candidate_id="different-source",
                )

    def test_pairing_rejects_missing_clip_or_dimension_mismatch(self) -> None:
        """A candidate cannot be ranked when its source/output tuple differs."""
        from benchmarks.quality_sweeps.paired_spatial_metrics import (
            SpatialPairingError,
            pair_spatial_metrics,
        )

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            baseline = root / "baseline.csv"
            candidate = root / "candidate.csv"
            write_rows(baseline, [row("a", ssim=0.5, mae=0.1), row("b", ssim=0.5, mae=0.1)])
            write_rows(candidate, [row("a", ssim=0.5, mae=0.1, output_width=1280)])
            with self.assertRaises(SpatialPairingError):
                pair_spatial_metrics(
                    baseline,
                    candidate,
                    baseline_id="base",
                    candidate_id="bad",
                )

    def test_pairing_preserves_class_as_part_of_the_identity_when_present(self) -> None:
        from benchmarks.quality_sweeps.paired_spatial_metrics import pair_spatial_metrics

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            baseline = root / "baseline.csv"
            candidate = root / "candidate.csv"
            class_fields = [*FIELDS, "class"]
            baseline_rows = [row("a", ssim=0.50, mae=0.10), row("a", ssim=0.60, mae=0.11)]
            candidate_rows = [row("a", ssim=0.55, mae=0.09), row("a", ssim=0.65, mae=0.10)]
            for values, scene_class in zip(baseline_rows, ("faces", "fabric")):
                values["class"] = scene_class
            for values, scene_class in zip(candidate_rows, ("faces", "fabric")):
                values["class"] = scene_class
            for path, values in ((baseline, baseline_rows), (candidate, candidate_rows)):
                with path.open("w", newline="", encoding="utf-8") as stream:
                    writer = csv.DictWriter(stream, fieldnames=class_fields)
                    writer.writeheader()
                    writer.writerows(values)
            report = pair_spatial_metrics(
                baseline,
                candidate,
                baseline_id="base",
                candidate_id="candidate",
            )

        self.assertEqual(report["rowCount"], 2)


if __name__ == "__main__":
    unittest.main()
