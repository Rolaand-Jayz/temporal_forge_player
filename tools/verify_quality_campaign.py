#!/usr/bin/env python3
"""Verify an M6 campaign manifest and its class-attributed matrix.

This command is intentionally a verifier, not a quality experiment launcher.
It makes the milestone gate runnable without changing reconstruction behavior
and without silently substituting legacy per-candidate CSVs for the schema-v2
spatial/temporal matrix.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

# The repository is intentionally not installed as a Python package. Add its
# root so this standalone verifier works when opened directly from any cwd.
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from benchmarks.quality_sweeps.campaign_matrix import MatrixError, validate_complete_matrix


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("campaign", type=Path)
    parser.add_argument(
        "matrix",
        nargs="?",
        type=Path,
        help="schema-v2 JSON object containing spatial and temporal row arrays",
    )
    parser.add_argument(
        "--metrics",
        action="append",
        default=[],
        metavar="CANDIDATE=CSV",
        help="legacy per-candidate CSV input; rejected by the strict verifier",
    )
    parser.add_argument(
        "--matrix",
        dest="matrix_option",
        type=Path,
        help="schema-v2 JSON object containing spatial and temporal row arrays",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.metrics:
        raise MatrixError(
            "strict campaign verification requires a schema-v2 matrix; "
            "legacy --metrics CSVs are not matrix input"
        )
    if args.matrix is not None and args.matrix_option is not None:
        raise MatrixError("supply the matrix as a positional argument or --matrix, not both")
    matrix_path = args.matrix_option or args.matrix
    if matrix_path is None:
        raise MatrixError(
            "strict campaign verification requires a schema-v2 matrix; "
            "pass MATRIX or --matrix MATRIX"
        )
    with args.campaign.open("r", encoding="utf-8") as stream:
        campaign = json.load(stream)
    with matrix_path.open("r", encoding="utf-8") as stream:
        matrix = json.load(stream)
    if not isinstance(matrix, dict):
        raise MatrixError("matrix artifact must be an object")
    validate_complete_matrix(campaign, matrix.get("spatial"), matrix.get("temporal"))
    print(f"quality campaign matrix verified: {len(campaign['candidates'])} candidates")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (MatrixError, OSError, json.JSONDecodeError, KeyError, TypeError) as error:
        print(f"quality campaign verification error: {error}", file=sys.stderr)
        raise SystemExit(2)
