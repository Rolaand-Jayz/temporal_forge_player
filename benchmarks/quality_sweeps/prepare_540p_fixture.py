#!/usr/bin/env python3
"""Create an explicitly derived 960x540 fixture manifest from 1280x720 clips."""

from __future__ import annotations

import argparse
import csv
import subprocess
from pathlib import Path


SCENES = ("tos_daylight", "tos_debris", "sintel_rooftop", "sintel_cave")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    args = parser.parse_args()
    root = args.repo.resolve()
    output = args.output_root.resolve()
    output.mkdir(parents=True, exist_ok=True)
    source_manifest = root / "benchmarks/video_corpus/manifest.csv"
    rows = list(csv.DictReader(source_manifest.open(newline="")))
    selected = {row["clip_id"]: row for row in rows
                if row["clip_id"] in SCENES and row["quality"] == "high"
                and row["width"] == "1280" and row["height"] == "720"}
    if set(selected) != set(SCENES):
        raise SystemExit(f"missing 1280x720 source rows: {sorted(set(SCENES) - set(selected))}")
    fixture_rows = []
    for scene in SCENES:
        source = selected[scene]
        clip = output / f"{scene}_960x540_derived_from_1280x720.mp4"
        subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", source["path"],
                        "-vf", "scale=960:540:flags=lanczos", "-c:v", "libx264", "-crf", "12",
                        "-pix_fmt", "yuv420p", str(clip)], check=True)
        row = dict(source)
        row.update({"width": "960", "height": "540", "path": str(clip),
                    "title": source["title"] + " (derived 960x540 fixture)",
                    "source_url": "derived: " + source["path"],
                    "license": "inherited from source clip"})
        fixture_rows.append(row)
    args.manifest.parent.mkdir(parents=True, exist_ok=True)
    with args.manifest.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0]), lineterminator="\n")
        writer.writeheader()
        writer.writerows(fixture_rows)
    print(args.manifest)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
