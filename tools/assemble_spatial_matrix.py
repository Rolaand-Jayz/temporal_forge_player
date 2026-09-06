#!/usr/bin/env python3
"""CLI for assembling a grounded M6.2 spatial matrix without recapture."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

# Running ``python tools/...`` puts ``tools`` on sys.path, not the repository
# root.  Add the root explicitly so this standalone benchmark tool works from
# the documented command line as well as from the Python test suite.
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from benchmarks.quality_sweeps.spatial_matrix import SpatialMatrixError, assemble_spatial_matrix


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("campaign", type=Path)
    parser.add_argument("results", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--repo-root", type=Path, default=Path.cwd())
    args = parser.parse_args()
    if args.output.exists():
        parser.error(f"refusing to overwrite existing output: {args.output}")
    try:
        campaign = json.loads(args.campaign.read_text(encoding="utf-8"))
        matrix = assemble_spatial_matrix(campaign, args.results, args.repo_root)
        args.output.write_text(json.dumps(matrix, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    except (OSError, json.JSONDecodeError, SpatialMatrixError) as error:
        parser.error(str(error))
    print(f"spatial matrix written: {args.output} ({len(matrix['rows'])} rows)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
