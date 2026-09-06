#!/usr/bin/env python3
"""Measure one captured candidate/reference PPM sequence pair.

The command is intentionally standalone: it reads existing artifacts, writes a
small CSV, and never launches the player or changes reconstruction behavior.
Motion JSON is required because identity motion would make the result look
more complete than the evidence supports.  Event JSON is optional; omitted
ghost/reset fields stay blank rather than becoming invented zeros.
"""

from __future__ import annotations

import argparse
import csv
import json
import sys
from collections.abc import Mapping
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from benchmarks.quality_sweeps.temporal_sequence import (  # noqa: E402
    load_p6_sequence,
    load_static_mask,
    measure_temporal_sequence,
)
from benchmarks.quality_sweeps.motion_sidecar import read_motion_fields  # noqa: E402


_FIELDS = [
    "candidateId",
    "scene",
    "configId",
    "startFrame",
    "endFrame",
    "class",
    "frames",
    "width",
    "height",
    "static_flicker",
    "edge_variance",
    "motion_compensated_error",
    "ghost_duration_frames",
    "reset_recovery_frames",
]


def parse_args() -> argparse.Namespace:
    """Define the file-oriented interface used by campaign tooling and reviewers."""

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--class",
        dest="scene_class",
        required=True,
        help="declared corpus class for the measured sequence",
    )
    parser.add_argument("--candidate-id", required=True, help="stable candidate/config owner identity")
    parser.add_argument("--scene", required=True, help="real corpus scene identity")
    parser.add_argument("--config-id", required=True, help="exact runtime configuration identity")
    parser.add_argument("--start-frame", required=True, type=int, help="source benchmark frame number")
    parser.add_argument("--candidate-dir", required=True, type=Path)
    parser.add_argument("--reference-dir", required=True, type=Path)
    parser.add_argument(
        "--motion-json",
        required=True,
        type=Path,
        help="temporal_forge.codec_motion.v1 causal codec-motion sidecar",
    )
    parser.add_argument("--events-json", type=Path)
    parser.add_argument(
        "--static-mask-json",
        required=True,
        type=Path,
        help="explicit static-region sidecar (2D boolean mask or rectangle schema)",
    )
    parser.add_argument("--output", required=True, type=Path)
    return parser.parse_args()


def _load_json(path: Path) -> Any:
    """Load one JSON sidecar and keep malformed evidence as a hard error."""

    with path.open("r", encoding="utf-8") as stream:
        return json.load(stream)


def _optional_object(path: Path | None, name: str) -> Mapping[str, Any] | None:
    """Read an optional object sidecar without accepting arrays or scalars."""

    if path is None:
        return None
    value = _load_json(path)
    if not isinstance(value, Mapping):
        raise ValueError(f"{name} JSON must contain an object")
    return value


def _static_mask(path: Path, width: int, height: int) -> Any:
    """Read and expand the required static-region annotation."""

    value = _load_json(path)
    return load_static_mask(value, width=width, height=height)


def _csv_value(value: object) -> str | int | float:
    """Format numbers consistently while preserving unavailable values as blank."""

    if value is None:
        return ""
    if isinstance(value, float):
        return f"{value:.9f}"
    return value  # type: ignore[return-value]


def main() -> int:
    """Load, measure, and write one non-destructive metric row."""

    args = parse_args()
    if args.start_frame < 0:
        raise ValueError("--start-frame must be non-negative")
    candidate = load_p6_sequence(args.candidate_dir)
    reference = load_p6_sequence(args.reference_dir)
    height = len(candidate[0])
    width = len(candidate[0][0])
    temporal_motion = read_motion_fields(
        args.motion_json,
        expected_frames=len(candidate),
        target_width=width,
        target_height=height,
    )
    metrics = measure_temporal_sequence(
        candidate,
        reference,
        temporal_motion_fields=temporal_motion,
        static_mask=_static_mask(args.static_mask_json, width, height),
        events=_optional_object(args.events_json, "events"),
    )
    if not args.output.parent.exists():
        args.output.parent.mkdir(parents=True, exist_ok=True)
    if args.output.exists():
        raise ValueError(f"refusing to overwrite existing metrics file: {args.output}")

    row = {
        "candidateId": args.candidate_id,
        "scene": args.scene,
        "configId": args.config_id,
        "startFrame": args.start_frame,
        "endFrame": args.start_frame + len(candidate) - 1,
        "class": args.scene_class,
        "frames": len(candidate),
        "width": width,
        "height": height,
        **{name: _csv_value(metrics[name]) for name in metrics},
    }
    with args.output.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=_FIELDS)
        writer.writeheader()
        writer.writerow(row)
    print(f"temporal metrics written: {args.output}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"temporal measurement error: {error}", file=sys.stderr)
        raise SystemExit(2)
