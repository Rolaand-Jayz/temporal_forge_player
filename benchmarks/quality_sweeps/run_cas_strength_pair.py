#!/usr/bin/env python3
"""Capture a paired CAS-strength comparison for the two finalist controls."""
from __future__ import annotations

import argparse
import csv
import hashlib
import os
from pathlib import Path
import re
import subprocess

CANDIDATES = {
    "current": "benchmarks/quality_sweeps/stage_a/current.json",
    "base_only_bilinear": "benchmarks/quality_sweeps/stage_a/base_only_bilinear.json",
}
STRENGTHS = ("0.04", "0.20")
SCENES = ("tos_daylight", "tos_debris", "sintel_rooftop", "sintel_cave")


def metric(label: str, text: str) -> str:
    values = re.findall(rf"{re.escape(label)}:([-0-9.]+)", text)
    if not values:
        raise RuntimeError(f"missing {label} metric")
    return values[-1]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--player", type=Path, default=Path("build/temporal_forge_player"))
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--artifact-root", type=Path, required=True)
    parser.add_argument("--output-csv", type=Path, required=True)
    parser.add_argument("--frame", type=int, default=48)
    args = parser.parse_args()
    root = args.repo.resolve(); artifacts = args.artifact_root.resolve(); artifacts.mkdir(parents=True, exist_ok=True)
    player = args.player.resolve(); binary_hash = hashlib.sha256(player.read_bytes()).hexdigest()
    with (root / "benchmarks/video_corpus/manifest.csv").open(newline="") as stream:
        manifest = list(csv.DictReader(stream))
    selected = {scene: next(row for row in manifest if row["clip_id"] == scene and row["quality"] == "high" and row["width"] == "1280" and row["height"] == "720") for scene in SCENES}
    rows: list[dict[str, str]] = []
    for candidate, config in CANDIDATES.items():
        for strength in STRENGTHS:
            for scene in SCENES:
                scene_root = artifacts / candidate / f"cas_{strength.replace('.', '')}" / scene; scene_root.mkdir(parents=True, exist_ok=True)
                raw_csv = scene_root / "raw.csv"
                env = os.environ.copy()
                env.update({
                    "TFORGE_QUALITY_CLIP": rf"^{re.escape(scene)}$", "TFORGE_QUALITY_QUALITY": "high",
                    "TFORGE_QUALITY_FRAME": str(args.frame), "TFORGE_QUALITY_TAG": f"{candidate}_cas{strength.replace('.', '')}",
                    "TFORGE_QUALITY_ARTIFACT_ROOT": str(scene_root), "TFORGE_QUALITY_OUTPUT_DIMENSIONS": "1920x1080",
                    "TFORGE_FSR4_FORCE_VIEWPORT": "1920x1080", "TFORGE_FSR4_FORCE_SCALE": "1.50",
                    "TFORGE_BENCHMARK_PRESET": "Quality", "TFORGE_DISABLE_HW_DECODE": "1",
                    "TFORGE_FSR4_PROFILE_TIMINGS": "1", "TFORGE_REVIEW_FSR_CAS": strength,
                    "TFORGE_QUALITY_LAB_CONFIG": str(root / config),
                })
                subprocess.run([str(root / "benchmarks/video_corpus/run_quality.sh"), str(player), "1280x720", str(raw_csv)], cwd=root, env=env, check=True)
                raw = next(csv.DictReader(raw_csv.open(newline="")))
                rows.append({
                    "candidate": candidate, "cas_strength": strength, "cas_placement": "resolve",
                    "scene": scene, "input_resolution": "1280x720", "output_resolution": "1920x1080",
                    "frame": str(args.frame), "psnr_db": raw["fsr_psnr_db"], "ssim": raw["fsr_ssim"],
                    "edge_ssim": raw["fsr_edge_ssim"], "gpu_ms": "", "binary_sha256": binary_hash,
                    "source_sha256": raw["control_source_sha256"], "config": config,
                })
    args.output_csv.parent.mkdir(parents=True, exist_ok=True)
    with args.output_csv.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0])); writer.writeheader(); writer.writerows(rows)
    print(args.output_csv)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
