#!/usr/bin/env python3
"""Capture matched temporal CAS-strength pairs for the finalist controls."""
from __future__ import annotations

import argparse
import csv
import hashlib
import os
from pathlib import Path
import re
import statistics
import subprocess

CANDIDATES = {
    "current": "benchmarks/quality_sweeps/stage_a/current.json",
    "base_only_bilinear": "benchmarks/quality_sweeps/stage_a/base_only_bilinear.json",
}
STRENGTHS = ("0.04", "0.20")
SCENES = ("tos_daylight", "tos_debris", "sintel_rooftop", "sintel_cave")


def ffmpeg(command: list[str], env: dict[str, str], cwd: Path, stderr: Path | None = None) -> str:
    if stderr is None:
        result = subprocess.run(command, env=env, cwd=cwd, check=True, capture_output=True, text=True)
        return result.stderr
    with stderr.open("w") as stream:
        subprocess.run(command, env=env, cwd=cwd, check=True, stdout=subprocess.DEVNULL, stderr=stream)
    return stderr.read_text(errors="replace")


def numbers(pattern: str, text: str) -> list[float]:
    return [float(x) for x in re.findall(pattern, text)]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--player", type=Path, default=Path("build/temporal_forge_player"))
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--artifact-root", type=Path, required=True)
    parser.add_argument("--output-csv", type=Path, required=True)
    parser.add_argument("--frames", type=int, default=12)
    parser.add_argument("--warmup", type=int, default=12)
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
                sequence = scene_root / "fsr_frames"; sequence.mkdir(exist_ok=True)
                raw_csv = scene_root / "raw.csv"
                env = os.environ.copy()
                env.update({
                    "TFORGE_TEMPORAL_WARMUP_FRAMES": str(args.warmup), "TFORGE_TEMPORAL_ARTIFACT_DIR": str(scene_root),
                    "TFORGE_FSR4_FORCE_VIEWPORT": "1920x1080", "TFORGE_FSR4_FORCE_SCALE": "1.50",
                    "TFORGE_FSR4_JITTER_MODE": "off", "TFORGE_DISABLE_HW_DECODE": "1",
                    "TFORGE_FSR4_PROFILE_TIMINGS": "1", "TFORGE_TEMPORAL_CAPTURE_TIMEOUT": "90",
                    "TFORGE_QUALITY_LAB_CONFIG": str(root / config), "TFORGE_FSR4_CAS_STRENGTH": strength,
                    "TFORGE_ALLOW_SPATIAL_TEMPORAL_CONTROL": "1",
                })
                subprocess.run([str(root / "benchmarks/video_corpus/run_temporal_quality.sh"), str(player), selected[scene]["path"], selected[scene]["reference_path"], str(raw_csv), str(args.frames)], cwd=root, env=env, check=True)
                reference = scene_root / "reference.mkv"
                ffmpeg(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", selected[scene]["reference_path"], "-vf", f"select=gte(n\\,{args.warmup}),scale=1920:1080:flags=lanczos,format=rgb24,setpts=N/24/TB", "-frames:v", str(args.frames), "-c:v", "ffv1", "-level", "3", "-g", "1", str(reference)], env, root)
                for filter_name, flags in {"lanczos": "lanczos", "bicubic": "bicubic"}.items():
                    candidate_video = scene_root / f"candidate_{filter_name}.mkv"
                    ffmpeg(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-framerate", "24", "-i", str(sequence / "temporal_forge_fsr4_%04d.ppm"), "-vf", f"scale=1920:1080:flags={flags},format=rgb24", "-frames:v", str(args.frames), "-c:v", "ffv1", "-level", "3", "-g", "1", str(candidate_video)], env, root)
                    log = scene_root / f"{filter_name}_ssim.log"
                    text = ffmpeg(["ffmpeg", "-hide_banner", "-i", str(candidate_video), "-i", str(reference), "-lavfi", "[0:v][1:v]ssim=stats_file=/dev/stderr", "-f", "null", "-"], {**env, "LC_ALL": "C"}, root, log)
                    scores = numbers(r"n:\d+.*All:([0-9.]+)", text)
                    if len(scores) != args.frames:
                        raise RuntimeError(f"expected {args.frames} SSIM records, got {len(scores)}")
                    delta_log = scene_root / f"{filter_name}_delta.log"
                    ffmpeg(["ffmpeg", "-hide_banner", "-loglevel", "error", "-i", str(candidate_video), "-vf", f"tblend=all_mode=difference,signalstats,metadata=print:file={delta_log}", "-f", "null", "-"], env, root)
                    deltas = numbers(r"lavfi\.signalstats\.YAVG=([0-9.]+)", delta_log.read_text(errors="replace"))
                    rows.append({"candidate": candidate, "cas_strength": strength, "scene": scene, "input_resolution": "1280x720", "output_resolution": "1920x1080", "downsample": filter_name, "frames": str(args.frames), "warmup": str(args.warmup), "ssim_mean": f"{statistics.mean(scores):.6f}", "ssim_min": f"{min(scores):.6f}", "temporal_delta_mean": f"{statistics.mean(deltas):.6f}" if deltas else "", "binary_sha256": binary_hash, "source_sha256": hashlib.sha256(Path(selected[scene]["path"]).read_bytes()).hexdigest(), "config": config})
                for frame in sequence.glob("temporal_forge_fsr4_*.ppm"):
                    frame.unlink()
    args.output_csv.parent.mkdir(parents=True, exist_ok=True)
    with args.output_csv.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0])); writer.writeheader(); writer.writerows(rows)
    print(args.output_csv)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
