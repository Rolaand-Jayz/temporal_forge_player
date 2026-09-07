#!/usr/bin/env python3
"""Measure CAS placement around an above-source FSR result and reduction."""

from __future__ import annotations

import argparse
import csv
import hashlib
import os
from pathlib import Path
import re
import subprocess

MODES = {
    "cas20_pre": {"TFORGE_FSR4_PRE_CAS": "1", "TFORGE_BENCHMARK_SHARPNESS": "0.20", "TFORGE_FSR4_DISABLE_CAS": "1"},
    "cas20_resolve": {"TFORGE_REVIEW_FSR_CAS": "0.20"},
    "cas20_post": {"TFORGE_FSR4_DISABLE_CAS": "1"},
    "no_cas": {"TFORGE_FSR4_DISABLE_CAS": "1"},
}
FILTERS = {"lanczos": "lanczos", "bicubic": "bicubic"}
SCENES = ("tos_daylight", "tos_debris", "sintel_rooftop", "sintel_cave")


def run(args: list[str], env: dict[str, str], cwd: Path) -> None:
    subprocess.run(args, env=env, cwd=cwd, check=True, stdout=subprocess.DEVNULL)


def score(candidate: Path, reference: Path, cwd: Path) -> tuple[str, str]:
    result = subprocess.run(
        ["ffmpeg", "-hide_banner", "-i", str(candidate), "-i", str(reference),
         "-lavfi", "[0:v][1:v]psnr;[0:v][1:v]ssim", "-f", "null", "-"],
        cwd=cwd, capture_output=True, text=True, check=True,
        env={**os.environ, "LC_ALL": "C"},
    )
    psnr = re.findall(r"average:([0-9.]+)", result.stderr)[-1]
    ssim = re.findall(r"All:([0-9.]+)", result.stderr)[-1]
    return psnr, ssim


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--player", type=Path, default=Path("build/temporal_forge_player"))
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--artifact-root", type=Path, required=True)
    parser.add_argument("--output-csv", type=Path, required=True)
    parser.add_argument("--frame", type=int, default=48)
    parser.add_argument("--mode", choices=tuple(MODES), action="append")
    parser.add_argument("--scene", choices=SCENES, action="append")
    args = parser.parse_args()
    root = args.repo.resolve(); artifact = args.artifact_root.resolve(); artifact.mkdir(parents=True, exist_ok=True)
    with (root / "benchmarks/video_corpus/manifest.csv").open(newline="") as stream:
        manifest = list(csv.DictReader(stream))
    selected = {scene: next(row for row in manifest if row["clip_id"] == scene and row["quality"] == "high" and row["width"] == "1920" and row["height"] == "1080") for scene in SCENES}
    binary_hash = hashlib.sha256(args.player.resolve().read_bytes()).hexdigest()
    rows: list[dict[str, str]] = []
    modes = args.mode or list(MODES)
    scenes = args.scene or list(SCENES)
    for mode in modes:
        overrides = MODES[mode]
        for scene in scenes:
            source_row = selected[scene]
            scene_root = artifact / mode / scene; scene_root.mkdir(parents=True, exist_ok=True)
            raw_csv = scene_root / "raw.csv"
            env = os.environ.copy()
            env.update({
                "TFORGE_QUALITY_CLIP": rf"^{re.escape(scene)}$", "TFORGE_QUALITY_QUALITY": "high",
                "TFORGE_QUALITY_FRAME": str(args.frame), "TFORGE_QUALITY_ARTIFACT_ROOT": str(scene_root),
                "TFORGE_QUALITY_OUTPUT_DIMENSIONS": "3840x2160", "TFORGE_FSR4_FORCE_VIEWPORT": "3840x2160",
                "TFORGE_FSR4_FORCE_SCALE": "2.00", "TFORGE_BENCHMARK_PRESET": "NativeAA",
                "TFORGE_FSR4_PROFILE_TIMINGS": "1", "TFORGE_DISABLE_HW_DECODE": "1",
                "TFORGE_QUALITY_LAB_CONFIG": str(root / "benchmarks/quality_sweeps/swarm/agent_composition_audit/current_control.json"),
            })
            for name in ("TFORGE_FSR4_PRE_CAS", "TFORGE_BENCHMARK_SHARPNESS", "TFORGE_FSR4_DISABLE_CAS", "TFORGE_REVIEW_FSR_CAS"):
                env.pop(name, None)
            env.update(overrides)
            run([str(root / "benchmarks/video_corpus/run_quality.sh"), str(args.player.resolve()), "1920x1080", str(raw_csv)], env, root)
            raw = next(csv.DictReader(raw_csv.open(newline="")))
            high = Path(raw["output_path"])
            reference = scene_root / "reference_1280x720.png"
            run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", source_row["reference_path"], "-vf", f"select=eq(n\\,{args.frame}),scale=1280:720:flags=lanczos", "-frames:v", "1", str(reference)], env, root)
            for filter_name, flags in FILTERS.items():
                reduced = scene_root / f"candidate_{filter_name}.png"
                run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", str(high), "-vf", f"scale=1280:720:flags={flags}", "-frames:v", "1", str(reduced)], env, root)
                candidate = reduced
                if mode == "cas20_post":
                    candidate = scene_root / f"candidate_{filter_name}_cas20_post.png"
                    run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", str(reduced), "-vf", "cas=strength=0.20", "-frames:v", "1", str(candidate)], env, root)
                psnr, ssim = score(candidate, reference, root)
                logs = "\n".join(path.read_text(errors="replace") for path in scene_root.glob("quality_logs/*.log"))
                gpu = re.findall(r"GPU=([0-9.]+)ms", logs)
                rows.append({"scene": scene, "cas_placement": mode, "cas_strength": "0.20" if mode != "no_cas" else "0.00", "reconstruction_resolution": "3840x2160", "source_resolution": "1920x1080", "final_resolution": "1280x720", "downsample": filter_name, "frame": str(args.frame), "psnr_db": psnr, "ssim": ssim, "gpu_ms": gpu[-1] if gpu else "", "binary_sha256": binary_hash, "source_sha256": raw["control_source_sha256"]})
    args.output_csv.parent.mkdir(parents=True, exist_ok=True)
    with args.output_csv.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0])); writer.writeheader(); writer.writerows(rows)
    print(args.output_csv)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
