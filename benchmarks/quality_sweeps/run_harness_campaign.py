#!/usr/bin/env python3
"""Populate the portable review harness from one resumable serial command.

Each pair is a transaction: renderer arms are captured serially, source-based
controls are created from the matched decoded input frame, then every method
for the pair is exported and dimension-checked.  A pair is never marked done
until all 23 method IDs have files for all three scenes.
"""
from __future__ import annotations

import argparse
import csv
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from benchmarks.quality_sweeps.trackmania_guard import trackmania_is_running
SCENES = ("tos_daylight", "sintel_rooftop", "sintel_cave")
SCALES = (2.00, 2.25, 2.50, 2.75, 3.00)
PAIRS = (
    (360, "640x360", 720), (360, "640x360", 1080), (360, "640x360", 1440), (360, "640x360", 2160),
    (480, "854x480", 720), (480, "854x480", 1080), (480, "854x480", 1440), (480, "854x480", 2160),
    (540, "960x540", 720), (540, "960x540", 1080), (540, "960x540", 1440), (540, "960x540", 2160),
    (720, "1280x720", 1080), (720, "1280x720", 1440), (720, "1280x720", 2160),
    (1080, "1920x1080", 1440), (1080, "1920x1080", 2160),
)
METHODS = (
    "current_cas20", "base_only_bilinear_cas20", "fsr_direct_cas20",
    *(f"fsr_{int(scale * 100):03d}x_downsample_{placement}"
      for scale in SCALES for placement in ("cas20_pre", "cas20_post", "no_cas")),
    "fsr_nativeaa_downsample_cas20_pre", "fsr_nativeaa_downsample_cas20_post",
    "fsr_nativeaa_downsample_no_cas", "conventional_lanczos", "conventional_bicubic",
)


def run(command: list[str], cwd: Path = ROOT) -> None:
    subprocess.run(command, cwd=cwd, check=True)


def filename(scene: str, input_height: int, method: str, output_height: int) -> str:
    return (f"scene-{scene}__frame-0048__in-{input_height}p__method-{method}"
            f"__out-{output_height}p.png")


def export(source: Path, scene: str, input_height: int, method: str, output_height: int, harness: Path) -> None:
    run([sys.executable, str(ROOT / "tools/export_review_image.py"), str(source),
         "--scene", scene, "--frame", "0048", "--input", str(input_height),
         "--method", method, "--output", str(output_height), "--root", str(harness)])


def source_raw(scene_root: Path) -> Path:
    candidates = sorted((scene_root / "quality_frames").glob("*_gpu_raw.png"))
    if len(candidates) != 1:
        raise SystemExit(f"expected one matched decoded source frame in {scene_root}, got {candidates}")
    return candidates[0]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--player", type=Path, default=Path("build/temporal_forge_player"))
    parser.add_argument("--artifact-root", type=Path, default=Path("benchmarks/quality_sweeps/harness_campaign_20260901"))
    parser.add_argument("--harness-root", type=Path, default=Path("review_harness"))
    parser.add_argument("--fixture-manifest", type=Path,
                        default=Path("benchmarks/quality_sweeps/resolution_540p_fixture_20260901.csv"))
    parser.add_argument("--only", action="append", metavar="INPUTxOUTPUT",
                        help="limit this invocation to selected pair(s); repeatable")
    parser.add_argument("--resume", action="store_true")
    args = parser.parse_args()
    artifacts = args.artifact_root.resolve(); harness = args.harness_root.resolve()
    run_capture = ROOT / "benchmarks/quality_sweeps/run_fsr_supersampling.py"
    for input_height, source, output_height in PAIRS:
        if args.only and f"{input_height}x{output_height}" not in args.only:
            continue
        stem = f"resolution_{input_height}_to_{output_height}"
        pair_root = artifacts / stem
        marker = pair_root / "harness_pair_complete.json"
        if args.resume and marker.is_file():
            print(f"Skipping complete {stem}")
            continue
        if trackmania_is_running():
            raise SystemExit(
                f"Trackmania is running; capture paused before {stem}. "
                "Stop the game and rerun with --resume."
            )
        final = f"{round(output_height * 16 / 9)}x{output_height}"
        manifest = args.fixture_manifest if input_height == 540 else None
        roots = {}
        for placement in ("pre", "post", "none"):
            if trackmania_is_running():
                raise SystemExit(f"Trackmania started; capture paused before {stem}/{placement}")
            root = pair_root / f"fsr_{placement}"
            csv_path = pair_root / f"fsr_{placement}.csv"
            command = [sys.executable, str(run_capture), "--player", str(args.player.resolve()),
                       "--repo", str(ROOT), "--artifact-root", str(root), "--output-csv", str(csv_path),
                       "--final", final, "--source", source, "--cas-strength", "0.20",
                       "--cas-placement", placement]
            if manifest:
                command.extend(["--manifest", str(manifest.resolve())])
            run(command)
            roots[placement] = root
        native_root = pair_root / "nativeaa"
        native_csv = pair_root / "nativeaa.csv"
        if trackmania_is_running():
            raise SystemExit(f"Trackmania started; capture paused before {stem}/nativeaa")
        command = [sys.executable, str(run_capture), "--player", str(args.player.resolve()),
                   "--repo", str(ROOT), "--artifact-root", str(native_root), "--output-csv", str(native_csv),
                   "--final", final, "--source", source, "--cas-strength", "0.20",
                   "--preset", "NativeAA"]
        if manifest:
            command.extend(["--manifest", str(manifest.resolve())])
        run(command)
        for scene in SCENES:
            pre_scene = roots["pre"] / "scale_2_00" / scene
            raw = source_raw(pre_scene)
            # Conventional controls and bilinear+CAS are actual transforms of
            # the matched decoded input frame, never copies of FSR output.
            for method, flags in (("conventional_lanczos", "lanczos"), ("conventional_bicubic", "bicubic"),
                                  ("base_only_bilinear_cas20", "bilinear")):
                output = harness / "images" / filename(scene, input_height, method, output_height)
                vf = f"scale={round(output_height * 16 / 9)}:{output_height}:flags={flags}"
                if method == "base_only_bilinear_cas20":
                    vf += ",cas=strength=0.20"
                run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", str(raw),
                     "-vf", vf, "-frames:v", "1", str(output)])
            # At 2.00x the delivery grid equals the final image. These are the
            # direct/current controls, recorded as an explicit same-pipeline
            # provenance rather than silently pretending they were separate.
            direct = roots["pre"] / "scale_2_00" / scene / "candidate_final.png"
            export(direct, scene, input_height, "fsr_direct_cas20", output_height, harness)
            export(direct, scene, input_height, "current_cas20", output_height, harness)
            for placement, suffix in (("pre", "cas20_pre"), ("post", "cas20_post"), ("none", "no_cas")):
                root = roots[placement]
                for scale in SCALES:
                    export(root / f"scale_{scale:.2f}".replace(".", "_") / scene / "candidate_final.png",
                           scene, input_height, f"fsr_{int(scale * 100):03d}x_downsample_{suffix}",
                           output_height, harness)
            for placement, suffix in (("pre", "cas20_pre"), ("post", "cas20_post"), ("none", "no_cas")):
                export(native_root / "scale_2_00" / scene / "candidate_final.png",
                       scene, input_height, f"fsr_nativeaa_downsample_{suffix}", output_height, harness)
        missing = []
        for scene in SCENES:
            for method in METHODS:
                path = harness / "images" / filename(scene, input_height, method, output_height)
                if not path.is_file():
                    missing.append(str(path))
        if missing:
            raise SystemExit(f"{stem} incomplete: {len(missing)} harness assets missing")
        marker.parent.mkdir(parents=True, exist_ok=True)
        marker.write_text(json.dumps({"input": input_height, "output": output_height,
                                      "scenes": list(SCENES), "methods": list(METHODS),
                                      "assets": len(SCENES) * len(METHODS),
                                      "provenance": {
                                          "current_cas20": "same 2.00x pre-CAS render as fsr_direct_cas20 at the delivery grid",
                                          "fsr_direct_cas20": "same 2.00x pre-CAS render as current_cas20 at the delivery grid",
                                          "base_only_bilinear_cas20": "matched decoded input frame scaled with bilinear then CAS 0.20",
                                          "conventional_lanczos": "matched decoded input frame scaled with Lanczos",
                                          "conventional_bicubic": "matched decoded input frame scaled with bicubic",
                                      }}) + "\n")
        print(f"completed {stem}: {len(SCENES) * len(METHODS)} assets")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
