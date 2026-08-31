#!/usr/bin/env python3
"""Copy one verified campaign PNG into the portable review harness.

The filename is constructed in one place so exported evidence cannot drift
from the review harness naming contract.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import shutil
import struct

INPUTS = {360, 480, 540, 720, 1080}
OUTPUTS = {720, 1080, 1440, 2160}
METHODS = {
    "current_cas20", "base_only_bilinear_cas20", "fsr_direct_cas20",
    "fsr_225x_downsample_cas20_pre", "fsr_225x_downsample_cas20_post", "fsr_225x_downsample_no_cas",
    "fsr_250x_downsample_cas20_pre", "fsr_250x_downsample_cas20_post", "fsr_250x_downsample_no_cas",
    "fsr_275x_downsample_cas20_pre", "fsr_275x_downsample_cas20_post", "fsr_275x_downsample_no_cas",
    "fsr_300x_downsample_cas20_pre", "fsr_300x_downsample_cas20_post", "fsr_300x_downsample_no_cas",
    "fsr_nativeaa_downsample_cas20_pre", "fsr_nativeaa_downsample_cas20_post", "fsr_nativeaa_downsample_no_cas",
    "conventional_lanczos", "conventional_bicubic",
}


def canonical_filename(scene: str, frame: str, input_height: int, method: str, output_height: int) -> str:
    scene_slug = re.sub(r"[^a-z0-9]+", "_", scene.lower()).strip("_")
    if not scene_slug or not re.fullmatch(r"\d{4}", frame):
        raise ValueError("scene must be non-empty and frame must be four digits")
    if input_height not in INPUTS or output_height not in OUTPUTS or method not in METHODS:
        raise ValueError("selection is outside the review harness contract")
    return f"scene-{scene_slug}__frame-{frame}__in-{input_height}p__method-{method}__out-{output_height}p.png"


def png_dimensions(path: Path) -> tuple[int, int]:
    with path.open("rb") as stream:
        if stream.read(24)[:8] != b"\x89PNG\r\n\x1a\n":
            raise ValueError(f"not a PNG: {path}")
        stream.seek(16)
        return struct.unpack(">II", stream.read(8))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("--scene", required=True)
    parser.add_argument("--frame", default="0048")
    parser.add_argument("--input", type=int, required=True, dest="input_height")
    parser.add_argument("--method", required=True)
    parser.add_argument("--output", type=int, required=True, dest="output_height")
    parser.add_argument("--root", type=Path, default=Path("review_harness"))
    args = parser.parse_args()
    if not args.source.is_file():
        raise SystemExit(f"missing source image: {args.source}")
    width, height = png_dimensions(args.source)
    if width <= 0 or height <= 0:
        raise SystemExit(f"invalid PNG dimensions: {args.source}")
    name = canonical_filename(args.scene, args.frame, args.input_height, args.method, args.output_height)
    destination = args.root / "images" / name
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(args.source, destination)
    print(f"exported {destination} ({width}x{height})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
