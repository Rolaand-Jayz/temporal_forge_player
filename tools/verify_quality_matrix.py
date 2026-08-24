#!/usr/bin/env python3
"""Verify the complete spatial/temporal matrix for one M6 campaign.

Upstream: a schema-validated campaign JSON and a matrix JSON containing
``spatial`` and ``temporal`` row lists. Downstream: a nonzero exit status for
missing joins, duplicate identities, dimension drift, missing metrics, or
provenance drift. This command is capture-free and does not alter the player.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

# The repository is intentionally used directly rather than installed as a
# package, so this standalone verifier can be run from any working directory.
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from benchmarks.quality_sweeps.campaign_matrix import MatrixError, validate_complete_matrix


def parse_args() -> argparse.Namespace:
    """Parse the campaign and matrix artifact paths supplied by the gate."""

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("campaign", type=Path, help="schemaVersion 2 campaign JSON")
    parser.add_argument(
        "matrix",
        type=Path,
        help="JSON object with spatial and temporal row arrays",
    )
    return parser.parse_args()


def main() -> int:
    """Load both artifacts and return zero only for exact complete coverage."""

    args = parse_args()
    with args.campaign.open("r", encoding="utf-8") as stream:
        campaign = json.load(stream)
    with args.matrix.open("r", encoding="utf-8") as stream:
        matrix = json.load(stream)
    if not isinstance(matrix, dict):
        raise MatrixError("matrix artifact must be an object")
    validate_complete_matrix(campaign, matrix.get("spatial"), matrix.get("temporal"))
    candidate_count = len(campaign["candidates"])
    scene_count = len(campaign["corpus"]["selection"])
    class_count = len(campaign["classes"])
    print(
        "quality matrix verified: "
        f"{candidate_count} candidates × {scene_count} scenes × {class_count} classes"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (MatrixError, OSError, json.JSONDecodeError, KeyError, TypeError) as error:
        print(f"quality matrix verification error: {error}", file=sys.stderr)
        raise SystemExit(2)
