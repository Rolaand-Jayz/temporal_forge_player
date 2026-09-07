#!/usr/bin/env python3
"""Assemble an auditable M6 schema-v2 matrix from existing temporal CSVs."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from benchmarks.quality_sweeps.temporal_matrix import (  # noqa: E402
    TemporalMatrixError,
    assemble_temporal_matrix,
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("campaign", type=Path)
    parser.add_argument("spatial_matrix", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument(
        "--temporal-csv",
        action="append",
        required=True,
        type=Path,
        help="one existing one-row temporal_metrics.csv; repeat for each campaign row",
    )
    parser.add_argument(
        "--declared-candidate",
        action="append",
        default=[],
        help="candidate id override for the corresponding --temporal-csv (preserves raw id and reports drift)",
    )
    parser.add_argument("--repo-root", type=Path, default=Path.cwd())
    parser.add_argument(
        "--allow-incomplete",
        action="store_true",
        help="write the evidence assembly despite blocking issues; pending gaps are always recorded",
    )
    args = parser.parse_args()
    if args.output.exists():
        parser.error(f"refusing to overwrite existing output: {args.output}")
    if args.declared_candidate and len(args.declared_candidate) != len(args.temporal_csv):
        parser.error("--declared-candidate must be supplied once for every --temporal-csv")
    try:
        campaign = json.loads(args.campaign.read_text(encoding="utf-8"))
        temporal_csvs = [
            (declared, path)
            for declared, path in zip(args.declared_candidate, args.temporal_csv)
        ] if args.declared_candidate else args.temporal_csv
        matrix = assemble_temporal_matrix(
            campaign,
            args.spatial_matrix,
            temporal_csvs,
            args.repo_root,
        )
        args.output.write_text(json.dumps(matrix, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    except (OSError, json.JSONDecodeError, TemporalMatrixError) as error:
        parser.error(str(error))
    issue_count = len(matrix["issues"])
    evidence_gap_count = len(matrix["evidenceGaps"])
    print(
        f"temporal matrix assembled: {args.output} "
        f"({matrix['temporalCsvCount']} rows, {issue_count} blocking issues, "
        f"{evidence_gap_count} evidence gaps, complete={matrix['complete']})"
    )
    if issue_count and not args.allow_incomplete:
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
