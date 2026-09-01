#!/usr/bin/env python3
"""Run the FSR reconstruction-scale experiment at one common final size.

The player is asked for the candidate's intermediate viewport.  Every
supersampled output is then reduced with the fixed first-pass filter before
metrics are computed, so intermediate pixels are never compared directly to
the final-resolution reference.
"""
from __future__ import annotations

import argparse
import csv
import os
import re
import subprocess
import time
from pathlib import Path


SCALES = (2.00, 2.25, 2.50, 2.75, 3.00)
SCENES = ("tos_daylight", "tos_debris", "sintel_rooftop", "sintel_cave")


def run(command: list[str], *, env: dict[str, str], cwd: Path) -> None:
    subprocess.run(command, env=env, cwd=cwd, check=True)


def vram_used() -> int | None:
    for path in sorted(Path("/sys/class/drm").glob("card*/device/mem_info_vram_used")):
        try:
            total = int(path.with_name("mem_info_vram_total").read_text())
            if total > 1_000_000_000:
                return int(path.read_text())
        except (OSError, ValueError):
            pass
    return None


def run_player(command: list[str], *, env: dict[str, str], cwd: Path) -> tuple[int | None, int | None]:
    before = vram_used()
    process = subprocess.Popen(command, env=env, cwd=cwd)
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
        raise subprocess.CalledProcessError(process.returncode, command)
    return before, peak


def metric(label: str, path: Path) -> str:
    values = re.findall(rf"{re.escape(label)}:([-0-9.]+)", path.read_text(errors="replace"))
    if not values:
        raise RuntimeError(f"missing {label} in {path}")
    return values[-1]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--player", type=Path, default=Path("build/temporal_forge_player"))
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--artifact-root", type=Path, required=True)
    parser.add_argument("--output-csv", type=Path, required=True)
    parser.add_argument("--final", default="2560x1440")
    parser.add_argument("--scene", action="append", choices=SCENES + ("synthetic_edges_text", "bbb_branches"))
    parser.add_argument("--source", default="1280x720")
    parser.add_argument("--frame", type=int, default=48)
    parser.add_argument("--scale", type=float, action="append", choices=SCALES,
                        help="limit the run to selected reconstruction scales")
    args = parser.parse_args()
    final_w, final_h = (int(x) for x in args.final.split("x"))
    root = args.repo.resolve()
    player = args.player.resolve()
    artifact_root = args.artifact_root.resolve()
    artifact_root.mkdir(parents=True, exist_ok=True)
    output_rows: list[dict[str, str]] = []
    requested_scales = tuple(args.scale) if args.scale else SCALES

    manifest = root / "benchmarks/video_corpus/manifest.csv"
    with manifest.open(newline="") as handle:
        rows = list(csv.DictReader(handle))
    source_w, source_h = (int(x) for x in args.source.split("x"))
    requested_scenes = tuple(args.scene) if args.scene else SCENES
    selected = {
        row["clip_id"]: row
        for row in rows
        if row["clip_id"] in requested_scenes and row["quality"] == "high"
        and row["width"] == str(source_w) and row["height"] == str(source_h)
    }
    if set(selected) != set(requested_scenes):
        raise RuntimeError(f"missing required scenes: {sorted(set(requested_scenes)-set(selected))}")

    for scale in requested_scales:
        intermediate_w = int(round(final_w * scale / 2.0))
        intermediate_h = int(round(final_h * scale / 2.0))
        candidate = f"scale_{scale:.2f}".replace(".", "_")
        candidate_root = artifact_root / candidate
        candidate_root.mkdir(parents=True, exist_ok=True)
        for scene in requested_scenes:
            row = selected[scene]
            scene_root = candidate_root / scene
            scene_root.mkdir(parents=True, exist_ok=True)
            raw_csv = scene_root / "raw.csv"
            env = os.environ.copy()
            env.update({
                "TFORGE_QUALITY_MANIFEST": str(manifest),
                "TFORGE_QUALITY_CLIP": rf"^{re.escape(scene)}$",
                "TFORGE_QUALITY_QUALITY": "high",
                "TFORGE_QUALITY_FRAME": str(args.frame),
                "TFORGE_QUALITY_TAG": candidate,
                "TFORGE_QUALITY_ARTIFACT_ROOT": str(scene_root),
                "TFORGE_QUALITY_OUTPUT_DIMENSIONS": f"{intermediate_w}x{intermediate_h}",
                "TFORGE_FSR4_FORCE_VIEWPORT": f"{intermediate_w}x{intermediate_h}",
                "TFORGE_FSR4_FORCE_SCALE": f"{scale:.2f}",
                "TFORGE_FSR4_JITTER_MODE": "off",
                "TFORGE_DISABLE_HW_DECODE": "1",
                "TFORGE_FSR4_PROFILE_TIMINGS": "1",
            })
            vram_before, vram_peak = run_player(
                [str(root / "benchmarks/video_corpus/run_quality.sh"), str(player),
                args.source, str(raw_csv)], env=env, cwd=root)
            with raw_csv.open(newline="") as handle:
                raw = next(csv.DictReader(handle))
            source = Path(raw["output_path"])
            final_candidate = scene_root / "candidate_final.png"
            reference_final = scene_root / "reference_final.png"
            reference = Path(raw["control_source_path"]).parent / (
                f"{scene}_reference_{intermediate_w}x{intermediate_h}_f{args.frame}_{candidate}.png"
            )
            # The quality runner's reference is a scaled version of the
            # lossless 2160p master. Regenerate the common final reference
            # directly from that master for an identical reference across arms.
            run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", row["reference_path"],
                 "-vf", f"select=eq(n\\,{args.frame}),scale={final_w}:{final_h}:flags=lanczos",
                 "-frames:v", "1", str(reference_final)], env=env, cwd=root)
            reduction = "lanczos" if scale > 2.0 else "none"
            if scale > 2.0:
                run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", str(source),
                     "-vf", f"scale={final_w}:{final_h}:flags=lanczos", "-frames:v", "1",
                     str(final_candidate)], env=env, cwd=root)
            else:
                run(["cp", str(source), str(final_candidate)], env=env, cwd=root)
            metric_log = scene_root / "final_metrics.log"
            edge_log = scene_root / "final_edge_metrics.log"
            with metric_log.open("w") as out:
                subprocess.run(["ffmpeg", "-hide_banner", "-i", str(final_candidate), "-i", str(reference_final),
                                "-lavfi", "[0:v][1:v]psnr;[0:v][1:v]ssim", "-f", "null", "-"],
                               env={**env, "LC_ALL": "C"}, cwd=root, stdout=subprocess.DEVNULL,
                               stderr=out, check=True)
            with edge_log.open("w") as out:
                subprocess.run(["ffmpeg", "-hide_banner", "-i", str(final_candidate), "-i", str(reference_final),
                                "-filter_complex", "[0:v]format=gray,edgedetect=low=0.05:high=0.15[a];[1:v]format=gray,edgedetect=low=0.05:high=0.15[b];[a][b]ssim", "-f", "null", "-"],
                               env={**env, "LC_ALL": "C"}, cwd=root, stdout=subprocess.DEVNULL,
                               stderr=out, check=True)
            timings = list((scene_root / "quality_logs").glob("*.log"))
            stage = next((p for p in timings if "stage-timing" in p.read_text(errors="replace")), None)
            stage_text = stage.read_text(errors="replace") if stage else ""
            gpu = re.findall(r"GPU=([0-9.]+)ms", stage_text)
            output_rows.append({
                "scene": scene, "scale": f"{scale:.2f}",
                "source_resolution": f"{row['width']}x{row['height']}",
                "intermediate_resolution": f"{intermediate_w}x{intermediate_h}",
                "final_resolution": args.final, "downsample": reduction,
                "psnr_db": metric("average", metric_log), "ssim": metric("All", metric_log),
                "edge_ssim": metric("All", edge_log), "gpu_ms_mean": gpu[-1] if gpu else "",
                "source_sha256": raw["control_source_sha256"],
                "binary": str(player), "frame": str(args.frame),
                "vram_before_bytes": str(vram_before or ""),
                "vram_peak_bytes": str(vram_peak or ""),
            })

    args.output_csv.parent.mkdir(parents=True, exist_ok=True)
    fields = list(output_rows[0])
    with args.output_csv.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(output_rows)
    print(args.output_csv)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
