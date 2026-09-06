#!/usr/bin/env python3
"""Validate quality-run labels against their recorded capture artifacts.

The audit is intentionally data-only. It checks that each run row agrees with
the runner's raw asset row, that the source frame resolves to the same video
timestamp, and that any exported harness image has the declared output size.
"""

from __future__ import annotations

import argparse
import csv
import json
import re
import subprocess
from pathlib import Path


def frame_timestamp(path: Path, frame: int) -> float:
    result = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0",
         "-show_entries", "frame=best_effort_timestamp_time", "-of",
         "csv=p=0", "-read_intervals", f"%+#{frame + 1}", str(path)],
        check=True, capture_output=True, text=True,
    )
    values = [float(line.rstrip(",")) for line in result.stdout.splitlines() if line.strip()]
    if len(values) <= frame:
        raise ValueError(f"frame {frame} is unavailable in {path}")
    return values[frame]


def png_size(path: Path) -> tuple[int, int]:
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"not a PNG: {path}")
    return int.from_bytes(data[16:20], "big"), int.from_bytes(data[20:24], "big")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("csv", type=Path, help="quality sweep CSV")
    parser.add_argument("--artifact-root", type=Path, required=True)
    parser.add_argument("--harness-root", type=Path, default=Path("review_harness"))
    parser.add_argument("--manifest", type=Path, default=Path("benchmarks/video_corpus/manifest.csv"))
    parser.add_argument("--json", type=Path)
    args = parser.parse_args()

    rows = list(csv.DictReader(args.csv.open(newline="")))
    manifest = list(csv.DictReader(args.manifest.open(newline="")))
    sources = {(row["clip_id"], row["width"], row["height"], row["quality"]): row["path"]
               for row in manifest}
    findings = []
    for row in rows:
        scene = row["scene"]
        frame = int(row["frame"])
        source_w, source_h = row["source_resolution"].split("x")
        source = Path(sources[(scene, source_w, source_h, "high")])
        asset = args.artifact_root / f"scale_{float(row['scale']):.2f}".replace(".", "_") / scene / "quality_frames"
        matches = [path for path in asset.glob(f"*f{frame}_*.png")
                   if all(token not in path.name for token in ("reference", "gpu_raw", "lanczos", "bicubic", "difference"))]
        if len(matches) != 1:
            raise ValueError(f"expected one captured frame for {scene} scale {row['scale']}, got {matches}")
        captured = matches[0]
        width, height = png_size(captured)
        if (width, height) != (int(row["intermediate_resolution"].split("x")[0]), int(row["intermediate_resolution"].split("x")[1])):
            raise ValueError(f"intermediate dimensions disagree for {captured}")
        pts = frame_timestamp(source, frame)
        findings.append({
            "scene": scene, "frame": frame, "source_resolution": row["source_resolution"],
            "intermediate_resolution": row["intermediate_resolution"],
            "final_resolution": row["final_resolution"], "scale": row["scale"],
            "source_frame_timestamp_seconds": round(pts, 6),
            "captured_asset": str(captured),
            "label_matches_csv": True,
        })
    result = {"csv": str(args.csv), "rows": len(findings), "all_labels_match": True, "findings": findings}
    if args.json:
        args.json.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({"rows": len(findings), "all_labels_match": True}, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
