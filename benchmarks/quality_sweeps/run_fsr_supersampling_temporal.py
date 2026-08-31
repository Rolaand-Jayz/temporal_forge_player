#!/usr/bin/env python3
"""Capture temporal FSR scale arms and score them at one final resolution."""
from __future__ import annotations

import argparse
import csv
import os
import re
import statistics
import subprocess
import time
from pathlib import Path

SCALES = (2.00, 2.25, 2.50, 2.75, 3.00)
SCENES = ("tos_daylight", "tos_debris", "sintel_rooftop", "sintel_cave")


def command(args: list[str], *, env: dict[str, str], cwd: Path, output: Path | None = None) -> str:
    if output is None:
        subprocess.run(args, env=env, cwd=cwd, check=True)
        return ""
    with output.open("w") as handle:
        subprocess.run(args, env=env, cwd=cwd, stdout=subprocess.DEVNULL,
                       stderr=handle, check=True)
    return output.read_text(errors="replace")


def values(pattern: str, text: str) -> list[float]:
    return [float(x) for x in re.findall(pattern, text)]


def vram_used() -> int | None:
    for path in sorted(Path("/sys/class/drm").glob("card*/device/mem_info_vram_used")):
        try:
            total = int(path.with_name("mem_info_vram_total").read_text())
            if total > 1_000_000_000:
                return int(path.read_text())
        except (OSError, ValueError):
            pass
    return None


def run_capture(args: list[str], *, env: dict[str, str], cwd: Path) -> tuple[int | None, int | None]:
    before = vram_used()
    process = subprocess.Popen(args, env=env, cwd=cwd)
    peak = before
    while process.poll() is None:
        used = vram_used()
        if used is not None and (peak is None or used > peak):
            peak = used
        time.sleep(0.05)
    used = vram_used()
    if used is not None and (peak is None or used > peak):
        peak = used
    if process.returncode:
        raise subprocess.CalledProcessError(process.returncode, args)
    return before, peak


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--player", type=Path, default=Path("build/temporal_forge_player"))
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--artifact-root", type=Path, required=True)
    parser.add_argument("--output-csv", type=Path, required=True)
    parser.add_argument("--final", default="2560x1440")
    parser.add_argument("--frames", type=int, default=12)
    parser.add_argument("--warmup", type=int, default=12)
    args = parser.parse_args()
    root = args.repo.resolve()
    player = args.player.resolve()
    final_w, final_h = (int(x) for x in args.final.split("x"))
    artifacts = args.artifact_root.resolve()
    artifacts.mkdir(parents=True, exist_ok=True)
    with (root / "benchmarks/video_corpus/manifest.csv").open(newline="") as handle:
        rows = list(csv.DictReader(handle))
    selected = {r["clip_id"]: r for r in rows if r["clip_id"] in SCENES and
                r["quality"] == "high" and r["width"] == "1280" and r["height"] == "720"}
    if set(selected) != set(SCENES):
        raise RuntimeError("required 1280x720 high-quality scenes are missing")

    result_rows: list[dict[str, str]] = []
    for scale in SCALES:
        iw, ih = int(round(final_w * scale / 2)), int(round(final_h * scale / 2))
        arm = f"scale_{scale:.2f}".replace(".", "_")
        for scene in SCENES:
            scene_root = artifacts / arm / scene
            scene_root.mkdir(parents=True, exist_ok=True)
            raw_csv = scene_root / "raw.csv"
            raw_env = os.environ.copy()
            raw_env.update({
                "TFORGE_TEMPORAL_WARMUP_FRAMES": str(args.warmup),
                "TFORGE_TEMPORAL_ARTIFACT_DIR": str(scene_root),
                "TFORGE_FSR4_FORCE_VIEWPORT": f"{iw}x{ih}",
                "TFORGE_FSR4_FORCE_SCALE": f"{scale:.2f}",
                "TFORGE_FSR4_JITTER_MODE": "off",
                "TFORGE_DISABLE_HW_DECODE": "1",
                "TFORGE_FSR4_PROFILE_TIMINGS": "1",
                "TFORGE_TEMPORAL_CAPTURE_TIMEOUT": "60",
                "TFORGE_QUALITY_LAB_CONFIG": str(root / "benchmarks/quality_sweeps/swarm/agent_composition_audit/current_control.json"),
            })
            vram_before, vram_peak = run_capture(
                [str(root / "benchmarks/video_corpus/run_temporal_quality.sh"), str(player),
                 selected[scene]["path"], selected[scene]["reference_path"], str(raw_csv), str(args.frames)],
                env=raw_env, cwd=root)
            frames_dir = scene_root / "fsr_frames"
            if not frames_dir.is_dir():
                raise RuntimeError(f"missing retained temporal frames: {frames_dir}")
            candidate_video = scene_root / "candidate_final.mkv"
            reference_video = scene_root / "reference_final.mkv"
            command(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-framerate", "24",
                     "-i", str(frames_dir / "temporal_forge_fsr4_%04d.ppm"), "-vf",
                     f"scale={final_w}:{final_h}:flags=lanczos,format=rgb24", "-frames:v", str(args.frames),
                     "-c:v", "ffv1", "-level", "3", "-g", "1", str(candidate_video)], env=raw_env, cwd=root)
            command(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", selected[scene]["reference_path"],
                     "-vf", f"select=gte(n\\,{args.warmup}),scale={final_w}:{final_h}:flags=lanczos,format=rgb24,setpts=N/24/TB",
                     "-frames:v", str(args.frames), "-c:v", "ffv1", "-level", "3", "-g", "1", str(reference_video)], env=raw_env, cwd=root)
            ssim_log = scene_root / "final_ssim.log"
            ssim_text = command(["ffmpeg", "-hide_banner", "-i", str(candidate_video), "-i", str(reference_video),
                                 "-lavfi", "[0:v][1:v]ssim=stats_file=/dev/stderr", "-f", "null", "-"], env={**raw_env, "LC_ALL": "C"}, cwd=root, output=ssim_log)
            # FFmpeg prints one aggregate `SSIM ... All:` line after the
            # per-frame `n:` records.  The aggregate must not be counted as a
            # thirteenth frame in a twelve-frame capture.
            scores = values(r"n:\d+.*All:([0-9.]+)", ssim_text)
            if len(scores) != args.frames:
                raise RuntimeError(f"expected {args.frames} SSIM records, got {len(scores)}")
            delta_log = scene_root / "final_delta.log"
            delta_text = command(["ffmpeg", "-hide_banner", "-loglevel", "error", "-i", str(candidate_video),
                                  "-vf", f"tblend=all_mode=difference,signalstats,metadata=print:file={delta_log}", "-f", "null", "-"], env=raw_env, cwd=root, output=scene_root / "delta.stderr")
            deltas = values(r"lavfi\.signalstats\.YAVG=([0-9.]+)", delta_log.read_text(errors="replace"))
            stage = "\n".join(p.read_text(errors="replace") for p in scene_root.glob("**/*.log"))
            gpu = values(r"GPU=([0-9.]+)ms", stage)
            result_rows.append({
                "scene": scene, "scale": f"{scale:.2f}",
                "intermediate_resolution": f"{iw}x{ih}", "final_resolution": args.final,
                "downsample": "lanczos", "frames": str(args.frames), "warmup": str(args.warmup),
                "ssim_mean": f"{statistics.mean(scores):.6f}", "ssim_min": f"{min(scores):.6f}",
                "temporal_delta_mean": f"{statistics.mean(deltas):.6f}" if deltas else "",
                "gpu_ms_mean": f"{statistics.mean(gpu):.6f}" if gpu else "",
                "gpu_ms_max": f"{max(gpu):.6f}" if gpu else "",
                "vram_before_bytes": str(vram_before or ""), "vram_peak_bytes": str(vram_peak or ""),
            })
    args.output_csv.parent.mkdir(parents=True, exist_ok=True)
    with args.output_csv.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(result_rows[0]))
        writer.writeheader()
        writer.writerows(result_rows)
    print(args.output_csv)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
