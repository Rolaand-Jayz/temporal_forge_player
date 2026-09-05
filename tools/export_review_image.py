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

LEGAL_PAIRS = {
    (360, 480), (360, 720), (360, 1080),
    (480, 720), (480, 1080), (480, 1440),
    (720, 1080), (720, 1440), (720, 2160),
    (1080, 1440), (1080, 2160),
}
METHODS = {
    "current_cas20", "base_only_bilinear_cas20", "fsr_direct_cas20",
    "fsr_200x_downsample_resolve_cas20", "fsr_200x_downsample_external_post_cas20", "fsr_200x_downsample_no_cas",
    "fsr_225x_downsample_resolve_cas20", "fsr_225x_downsample_external_post_cas20", "fsr_225x_downsample_no_cas",
    "fsr_250x_downsample_resolve_cas20", "fsr_250x_downsample_external_post_cas20", "fsr_250x_downsample_no_cas",
    "fsr_275x_downsample_resolve_cas20", "fsr_275x_downsample_external_post_cas20", "fsr_275x_downsample_no_cas",
    "fsr_300x_downsample_resolve_cas20", "fsr_300x_downsample_external_post_cas20", "fsr_300x_downsample_no_cas",
    "fsr_nativeaa_downsample_resolve_cas20", "fsr_nativeaa_downsample_external_post_cas20", "fsr_nativeaa_downsample_no_cas",
    "conventional_lanczos", "conventional_bicubic",
}


def canonical_filename(scene: str, frame: str, input_height: int, method: str, output_height: int) -> str:
    scene_slug = re.sub(r"[^a-z0-9]+", "_", scene.lower()).strip("_")
    if not scene_slug or not re.fullmatch(r"\d{4}", frame):
        raise ValueError("scene must be non-empty and frame must be four digits")
    if (input_height, output_height) not in LEGAL_PAIRS or method not in METHODS:
        raise ValueError("selection is outside the review harness contract")
    return f"scene-{scene_slug}__frame-{frame}__in-{input_height}p__method-{method}__out-{output_height}p.png"


def png_dimensions(path: Path) -> tuple[int, int]:
    with path.open("rb") as stream:
        if stream.read(24)[:8] != b"\x89PNG\r\n\x1a\n":
            raise ValueError(f"not a PNG: {path}")
        stream.seek(16)
        return struct.unpack(">II", stream.read(8))


def export_review_image(source: Path, *, scene: str, frame: str,
                        input_height: int, method: str, output_height: int,
                        root: Path) -> Path:
    """Validate and copy one campaign image without spawning another process."""
    if not source.is_file():
        raise ValueError(f"missing source image: {source}")
    width, height = png_dimensions(source)
    expected_width = 854 if output_height == 480 else output_height * 16 // 9
    if (width, height) != (expected_width, output_height):
        raise ValueError(
            f"delivery image has {width}x{height}, expected "
            f"{expected_width}x{output_height}: {source}"
        )
    name = canonical_filename(scene, frame, input_height, method, output_height)
    destination = root / "images" / name
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)
    return destination


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
    destination = export_review_image(
        args.source, scene=args.scene, frame=args.frame,
        input_height=args.input_height, method=args.method,
        output_height=args.output_height, root=args.root,
    )
    width, height = png_dimensions(destination)
    print(f"exported {destination} ({width}x{height})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
