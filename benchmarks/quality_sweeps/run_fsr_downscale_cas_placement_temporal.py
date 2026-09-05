#!/usr/bin/env python3
"""Measure CAS placement over a temporal above-source/downscale pipeline."""
from __future__ import annotations

import argparse
import csv
import hashlib
import os
from pathlib import Path
import re
import statistics
import subprocess
import time

MODES = {
    "cas20_pre": {"TFORGE_FSR4_PRE_CAS": "1", "TFORGE_BENCHMARK_SHARPNESS": "0.20", "TFORGE_FSR4_DISABLE_CAS": "1"},
    "cas20_resolve": {"TFORGE_REVIEW_FSR_CAS": "0.20"},
    "cas20_post": {"TFORGE_FSR4_DISABLE_CAS": "1"},
    "no_cas": {"TFORGE_FSR4_DISABLE_CAS": "1"},
}
FILTERS = {"lanczos": "lanczos", "bicubic": "bicubic"}
SCENES = ("tos_daylight", "tos_debris", "sintel_rooftop", "sintel_cave")


def run_capture(command: list[str], env: dict[str, str], cwd: Path) -> tuple[int | None, int | None]:
    def vram() -> int | None:
        for path in sorted(Path("/sys/class/drm").glob("card*/device/mem_info_vram_used")):
            try:
                total = int(path.with_name("mem_info_vram_total").read_text())
                if total > 1_000_000_000:
                    return int(path.read_text())
            except (OSError, ValueError):
                pass
        return None
    before = vram(); peak = before
    process = subprocess.Popen(command, env=env, cwd=cwd)
    while process.poll() is None:
        used = vram()
        if used is not None and (peak is None or used > peak):
            peak = used
        time.sleep(0.05)
    if process.returncode:
        raise subprocess.CalledProcessError(process.returncode, command)
    return before, peak


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
    parser.add_argument("--mode", choices=tuple(MODES), action="append")
    parser.add_argument("--scene", choices=SCENES, action="append")
    args = parser.parse_args()
    root = args.repo.resolve(); artifacts = args.artifact_root.resolve(); artifacts.mkdir(parents=True, exist_ok=True)
    player = args.player.resolve()
    with (root / "benchmarks/video_corpus/manifest.csv").open(newline="") as stream:
        manifest = list(csv.DictReader(stream))
    selected = {scene: next(row for row in manifest if row["clip_id"] == scene and row["quality"] == "high" and row["width"] == "1920" and row["height"] == "1080") for scene in SCENES}
    binary_hash = hashlib.sha256(player.read_bytes()).hexdigest()
    rows: list[dict[str, str]] = []
    for mode in args.mode or list(MODES):
        for scene in args.scene or list(SCENES):
            scene_root = artifacts / mode / scene; scene_root.mkdir(parents=True, exist_ok=True)
            raw_csv = scene_root / "raw.csv"
            env = os.environ.copy()
            env.update({
                "TFORGE_TEMPORAL_WARMUP_FRAMES": str(args.warmup),
                "TFORGE_TEMPORAL_ARTIFACT_DIR": str(scene_root),
                "TFORGE_FSR4_FORCE_VIEWPORT": "3840x2160", "TFORGE_FSR4_FORCE_SCALE": "2.00",
                "TFORGE_FSR4_JITTER_MODE": "off", "TFORGE_DISABLE_HW_DECODE": "1",
                "TFORGE_FSR4_PROFILE_TIMINGS": "1", "TFORGE_TEMPORAL_CAPTURE_TIMEOUT": "90",
                "TFORGE_QUALITY_LAB_CONFIG": str(root / "benchmarks/quality_sweeps/swarm/agent_composition_audit/current_control.json"),
            })
            for name in ("TFORGE_FSR4_PRE_CAS", "TFORGE_BENCHMARK_SHARPNESS", "TFORGE_FSR4_DISABLE_CAS", "TFORGE_REVIEW_FSR_CAS"):
                env.pop(name, None)
            env.update(MODES[mode])
            run_capture([str(root / "benchmarks/video_corpus/run_temporal_quality.sh"), str(player), selected[scene]["path"], selected[scene]["reference_path"], str(raw_csv), str(args.frames)], env, root)
            frames = scene_root / "fsr_frames"
            if not frames.is_dir():
                raise RuntimeError(f"missing temporal frames: {frames}")
            reference = scene_root / "reference.mkv"
            ffmpeg(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", selected[scene]["reference_path"], "-vf", f"select=gte(n\\,{args.warmup}),scale=1280:720:flags=lanczos,format=rgb24,setpts=N/24/TB", "-frames:v", str(args.frames), "-c:v", "ffv1", "-level", "3", "-g", "1", str(reference)], env, root)
            for filter_name, filter_flags in FILTERS.items():
                reduced = scene_root / f"reduced_{filter_name}.mkv"
                ffmpeg(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-framerate", "24", "-i", str(frames / "temporal_forge_fsr4_%04d.ppm"), "-vf", f"scale=1280:720:flags={filter_flags},format=rgb24", "-frames:v", str(args.frames), "-c:v", "ffv1", "-level", "3", "-g", "1", str(reduced)], env, root)
                candidate = reduced
                if mode == "cas20_post":
                    candidate = scene_root / f"candidate_{filter_name}_cas20_post.mkv"
                    ffmpeg(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", str(reduced), "-vf", "cas=strength=0.20", "-c:v", "ffv1", "-level", "3", "-g", "1", str(candidate)], env, root)
                ssim_log = scene_root / f"{filter_name}_ssim.log"
                ssim_text = ffmpeg(["ffmpeg", "-hide_banner", "-i", str(candidate), "-i", str(reference), "-lavfi", "[0:v][1:v]ssim=stats_file=/dev/stderr", "-f", "null", "-"], {**env, "LC_ALL": "C"}, root, ssim_log)
                scores = numbers(r"n:\d+.*All:([0-9.]+)", ssim_text)
                if len(scores) != args.frames:
                    raise RuntimeError(f"expected {args.frames} SSIM records, got {len(scores)}")
                delta_log = scene_root / f"{filter_name}_delta.log"
                ffmpeg(["ffmpeg", "-hide_banner", "-loglevel", "error", "-i", str(candidate), "-vf", f"tblend=all_mode=difference,signalstats,metadata=print:file={delta_log}", "-f", "null", "-"], env, root)
                deltas = numbers(r"lavfi\.signalstats\.YAVG=([0-9.]+)", delta_log.read_text(errors="replace"))
                rows.append({"scene": scene, "cas_placement": mode, "cas_strength": "0.20" if mode != "no_cas" else "0.00", "reconstruction_resolution": "3840x2160", "source_resolution": "1920x1080", "final_resolution": "1280x720", "downsample": filter_name, "frames": str(args.frames), "warmup": str(args.warmup), "ssim_mean": f"{statistics.mean(scores):.6f}", "ssim_min": f"{min(scores):.6f}", "temporal_delta_mean": f"{statistics.mean(deltas):.6f}" if deltas else "", "gpu_ms_mean": "", "gpu_ms_max": "", "vram_before_bytes": "", "vram_peak_bytes": "", "binary_sha256": binary_hash, "source_sha256": hashlib.sha256(Path(selected[scene]["path"]).read_bytes()).hexdigest()})
            # The retained PPM sequence is only an intermediate for this
            # measurement. Delete it before the next 3840x2160 capture so a
            # complete matrix does not exhaust the temporary filesystem.
            for frame in frames.glob("temporal_forge_fsr4_*.ppm"):
                frame.unlink()
    args.output_csv.parent.mkdir(parents=True, exist_ok=True)
    with args.output_csv.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0])); writer.writeheader(); writer.writerows(rows)
    print(args.output_csv)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
