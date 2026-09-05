#!/usr/bin/env python3
"""Add identity-validated event rows to an existing M6 combined matrix."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from benchmarks.quality_sweeps.event_matrix import assemble_event_matrix  # noqa: E402
from benchmarks.quality_sweeps.temporal_matrix import TemporalMatrixError  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("campaign", type=Path)
    parser.add_argument("base_matrix", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--event-csv", action="append", required=True, type=Path)
    parser.add_argument("--event-trace", action="append", required=True, type=Path)
    parser.add_argument("--motion-json", action="append", required=True, help="use - when the supplied event capture retained no motion sidecar")
    parser.add_argument("--static-mask-json", action="append", required=True, help="use - when the supplied event capture retained no static-mask sidecar")
    parser.add_argument("--event-class", action="append", required=True)
    args = parser.parse_args()
    counts = {len(args.event_csv), len(args.event_trace), len(args.motion_json), len(args.static_mask_json), len(args.event_class)}
    if counts != {5}:
        parser.error("each of --event-csv, --event-trace, --motion-json, --static-mask-json, and --event-class must be supplied exactly five times")
    if args.output.exists():
        parser.error(f"refusing to overwrite existing output: {args.output}")
    try:
        campaign = json.loads(args.campaign.read_text(encoding="utf-8"))
        base_matrix = json.loads(args.base_matrix.read_text(encoding="utf-8"))
        inputs = [
            {
                "temporalCsv": csv_path,
                "eventTrace": trace_path,
                "motionJson": None if motion_path == "-" else Path(motion_path),
                "staticMaskJson": None if mask_path == "-" else Path(mask_path),
                "declaredClass": quality_class,
            }
            for csv_path, trace_path, motion_path, mask_path, quality_class in zip(
                args.event_csv, args.event_trace, args.motion_json, args.static_mask_json, args.event_class
            )
        ]
        result = assemble_event_matrix(campaign, base_matrix, inputs)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    except (OSError, json.JSONDecodeError, TemporalMatrixError) as error:
        parser.error(str(error))
    print(
        f"M6 event evidence assembled: {args.output} "
        f"(preserved {len(result['temporal'])} eight-frame rows, "
        f"kept {len(result['eventEvidence']['rows'])} rows in separate non-strict evidence)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
