#!/usr/bin/env python3
"""Assemble complete per-frame codec-motion exports into one M6 sidecar."""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections.abc import Mapping
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from benchmarks.quality_sweeps.motion_sidecar import (  # noqa: E402
    assemble_motion_sidecar,
)


def parse_args() -> argparse.Namespace:
    """Define the capture-only assembly interface."""

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--records-dir", required=True, type=Path)
    parser.add_argument("--expected-frames", required=True, type=int)
    parser.add_argument("--target-width", required=True, type=int)
    parser.add_argument("--target-height", required=True, type=int)
    parser.add_argument("--output", required=True, type=Path)
    return parser.parse_args()


def _natural_key(path: Path) -> list[int | str]:
    """Sort exported frame records by numeric frame suffix."""

    return [
        int(part) if part.isdigit() else part.lower()
        for part in re.split(r"(\d+)", path.name)
    ]


def _load_record(path: Path) -> Mapping[str, Any]:
    """Read one complete JSON frame record and reject non-object data."""

    with path.open("r", encoding="utf-8") as stream:
        value = json.load(stream)
    if not isinstance(value, Mapping):
        raise ValueError(f"motion frame record must be an object: {path}")
    return value


def main() -> int:
    """Load exactly the requested records and publish one validated sidecar."""

    args = parse_args()
    if args.expected_frames < 2:
        raise ValueError("expected frame count must be at least two")
    if not args.records_dir.is_dir():
        raise ValueError(f"motion records directory does not exist: {args.records_dir}")
    paths = sorted(args.records_dir.glob("codec_motion_*.json"), key=_natural_key)
    if len(paths) != args.expected_frames:
        raise ValueError(
            f"expected {args.expected_frames} motion frame records, found {len(paths)}"
        )
    records = [_load_record(path) for path in paths]
    sidecar = assemble_motion_sidecar(
        records,
        target_width=args.target_width,
        target_height=args.target_height,
    )
    if args.output.exists():
        raise ValueError(f"refusing to overwrite existing motion sidecar: {args.output}")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="utf-8") as stream:
        json.dump(sidecar, stream, indent=2, sort_keys=True)
        stream.write("\n")
    print(f"motion sidecar written: {args.output}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"motion sidecar assembly error: {error}", file=sys.stderr)
        raise SystemExit(2)
