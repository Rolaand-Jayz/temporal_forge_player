#!/usr/bin/env python3
"""Export and validate one completed resolution pair for the review harness."""

from __future__ import annotations

import argparse
import json
import re
import struct
import subprocess
from pathlib import Path


SCENES = ("tos_daylight", "sintel_rooftop", "sintel_cave")
SCALES = (2.00, 2.25, 2.50, 2.75, 3.00)


def png_size(path: Path) -> tuple[int, int]:
    with path.open("rb") as stream:
        if stream.read(8) != b"\x89PNG\r\n\x1a\n":
            raise ValueError(f"not a PNG: {path}")
        stream.seek(16)
        return struct.unpack(">II", stream.read(8))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--artifact-root", type=Path, required=True)
    parser.add_argument("--input", type=int, required=True, dest="input_height")
    parser.add_argument("--output", type=int, required=True, dest="output_height")
    parser.add_argument("--frame", default="0048")
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--harness-root", type=Path, default=Path("review_harness"))
    args = parser.parse_args()
    root = args.repo.resolve()
    artifact_root = args.artifact_root.resolve()
    expected = (round(args.output_height * 16 / 9), args.output_height)
    exported = []
    for scale in SCALES:
        scale_dir = artifact_root / f"scale_{scale:.2f}".replace(".", "_")
        method = f"fsr_{int(scale * 100):03d}x_downsample_cas20_pre"
        for scene in SCENES:
            source = scale_dir / scene / "candidate_final.png"
            if not source.is_file():
                raise SystemExit(f"pair is incomplete, missing {source}")
            if png_size(source) != expected:
                raise SystemExit(f"wrong final dimensions for {source}: {png_size(source)} != {expected}")
            subprocess.run([
                "python3", str(root / "tools/export_review_image.py"), str(source),
                "--scene", scene, "--frame", args.frame, "--input", str(args.input_height),
                "--method", method, "--output", str(args.output_height),
                "--root", str(args.harness_root.resolve()),
            ], cwd=root, check=True)
            exported.append({"scene": scene, "scale": f"{scale:.2f}", "method": method,
                             "source": str(source), "dimensions": list(expected)})
    report = {"input_height": args.input_height, "output_height": args.output_height,
              "frame": args.frame, "rows": len(exported), "all_assets_valid": True,
              "assets": exported}
    report_path = artifact_root / "review_pair_validation.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"rows": len(exported), "all_assets_valid": True}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
