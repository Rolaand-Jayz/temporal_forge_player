#!/usr/bin/env python3
"""Run the meaningful harness resolution matrix serially.

This is a data-only expansion of the strongest supersampling probe. It keeps
the harness's full selector matrix available while producing real rows for
every meaningful upscale pair in the campaign brief.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from benchmarks.quality_sweeps.trackmania_guard import trackmania_is_running


PAIRS = (
    (360, "640x360", 1080), (360, "640x360", 1440), (360, "640x360", 2160),
    (480, "854x480", 1080), (480, "854x480", 1440), (480, "854x480", 2160),
    (540, "960x540", 720), (540, "960x540", 1080), (540, "960x540", 1440), (540, "960x540", 2160),
    (1080, "1920x1080", 1440), (1080, "1920x1080", 2160),
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--player", type=Path, default=Path("build/temporal_forge_player"))
    parser.add_argument("--artifact-root", type=Path, required=True)
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--fixture-manifest", type=Path, required=True)
    parser.add_argument("--resume", action="store_true")
    args = parser.parse_args()
    root = args.repo.resolve()
    player = args.player.resolve()
    artifacts = args.artifact_root.resolve()
    outputs = args.output_root.resolve()
    for input_height, source, output_height in PAIRS:
        if trackmania_is_running():
            print(f"Trackmania detected; keeping {input_height}->{output_height} serial.", file=sys.stderr)
        final = f"{round(output_height * 16 / 9)}x{output_height}"
        stem = f"resolution_{input_height}_to_{output_height}_20260901"
        output_csv = outputs / f"{stem}.csv"
        if args.resume and output_csv.is_file():
            import csv
            with output_csv.open(newline="") as handle:
                if sum(1 for _ in csv.DictReader(handle)) == 20:
                    print(f"Skipping capture for complete {stem}; validating/exporting pair.")
                    subprocess.run([
                        sys.executable, str(root / "benchmarks/quality_sweeps/validate_review_pair.py"),
                        "--artifact-root", str(artifacts / stem), "--input", str(input_height),
                        "--output", str(output_height), "--repo", str(root),
                    ], cwd=root, check=True)
                    continue
        command = [sys.executable, str(root / "benchmarks/quality_sweeps/run_fsr_supersampling.py"),
                   "--player", str(player), "--repo", str(root), "--artifact-root", str(artifacts / stem),
                   "--output-csv", str(output_csv), "--final", final,
                   "--source", source, "--cas-strength", "0.20"]
        if input_height == 540:
            command.extend(["--manifest", str(args.fixture_manifest.resolve())])
        subprocess.run(command, cwd=root, check=True)
        subprocess.run([
            sys.executable, str(root / "benchmarks/quality_sweeps/validate_review_pair.py"),
            "--artifact-root", str(artifacts / stem), "--input", str(input_height),
            "--output", str(output_height), "--repo", str(root),
        ], cwd=root, check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
