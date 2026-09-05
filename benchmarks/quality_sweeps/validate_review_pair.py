#!/usr/bin/env python3
"""Export and validate one completed resolution pair for the review harness."""

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path


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
    artifact_root = args.artifact_root.resolve()
    marker_path = artifact_root / "harness_pair_complete.json"
    if not marker_path.is_file():
        raise SystemExit(f"pair is incomplete, missing {marker_path}")
    marker = json.loads(marker_path.read_text())
    if (marker.get("input"), marker.get("output")) != (args.input_height, args.output_height):
        raise SystemExit("completion marker resolution does not match requested pair")
    assets = marker.get("assets_detail", [])
    if marker.get("assets") != len(assets) or not assets:
        raise SystemExit("completion marker has no complete asset detail")
    expected = (round(args.output_height * 16 / 9), args.output_height)
    validated = []
    for asset in assets:
        path = Path(asset["path"])
        if not path.is_file():
            raise SystemExit(f"pair is incomplete, missing {path}")
        dimensions = png_size(path)
        if dimensions != expected:
            raise SystemExit(f"wrong final dimensions for {path}: {dimensions} != {expected}")
        if (asset.get("width"), asset.get("height")) != dimensions:
            raise SystemExit(f"marker dimensions do not match {path}")
        if asset.get("input") != args.input_height or asset.get("output") != args.output_height:
            raise SystemExit(f"marker identity does not match {path}")
        validated.append({"path": str(path), "scene": asset.get("scene"),
                          "method": asset.get("method"), "dimensions": list(dimensions)})
    if len({item["scene"] for item in validated}) != 4:
        raise SystemExit("completion marker does not contain all four scenes")
    if len({item["method"] for item in validated}) != 23:
        raise SystemExit("completion marker does not contain all required methods")
    report = {"input_height": args.input_height, "output_height": args.output_height,
              "frame": args.frame, "rows": len(validated), "all_assets_valid": True,
              "assets": validated}
    report_path = artifact_root / "review_pair_validation.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"rows": len(validated), "all_assets_valid": True}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
