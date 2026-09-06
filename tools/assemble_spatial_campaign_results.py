#!/usr/bin/env python3
"""Assemble spatial campaign results from an existing run without recapture.

The assembler reads the exact per-candidate result identities already written
under a run root.  It never uses the current checkout HEAD as a substitute for
missing execution provenance.  Old runs therefore produce an explicitly
unrecorded result and a non-zero status until a proven commit is present in
their captured identity.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from benchmarks.quality_sweeps.campaign_provenance import (  # noqa: E402
    ProvenanceError,
    stamp_result_git_commit,
)


_COMMIT = re.compile(r"^[0-9a-fA-F]{7,64}$")


def _result_for_candidate(run_root: Path, candidate_id: str) -> dict[str, Any]:
    matches: list[dict[str, Any]] = []
    for path in run_root.rglob("result.json"):
        try:
            result = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            raise ProvenanceError(f"cannot read captured result {path}: {error}") from error
        if isinstance(result, dict) and result.get("candidateId") == candidate_id:
            matches.append(result)
    if len(matches) != 1:
        raise ProvenanceError(
            f"captured run must contain exactly one result for {candidate_id}; found {len(matches)}"
        )
    result = matches[0]
    run_id = result.get("runId")
    if not isinstance(run_id, str) or not run_id:
        raise ProvenanceError(f"captured result for {candidate_id} has no exact runId")
    return result


def assemble_results(run_root: Path, candidate_ids: list[str]) -> tuple[list[dict[str, Any]], str | None]:
    """Return result rows and a commit only when every row proves the same one."""
    if not candidate_ids:
        raise ProvenanceError("at least one candidate is required")
    results = [_result_for_candidate(run_root, candidate_id) for candidate_id in candidate_ids]
    commits: set[str] = set()
    unavailable = False
    for result in results:
        value = result.get("gitCommit")
        if value is None:
            unavailable = True
            continue
        if not isinstance(value, str) or not _COMMIT.fullmatch(value):
            raise ProvenanceError(f"captured result has invalid gitCommit: {value!r}")
        commits.add(value.lower())
    if len(commits) > 1:
        raise ProvenanceError("captured results contain mixed git commits")
    if commits and unavailable:
        raise ProvenanceError("captured results mix recorded and unavailable git commits")
    commit = next(iter(commits), None)
    return [stamp_result_git_commit(result, commit) for result in results], commit


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run_root", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--candidate", action="append", required=True)
    args = parser.parse_args()
    if args.output.exists():
        parser.error(f"refusing to overwrite existing output: {args.output}")
    try:
        results, commit = assemble_results(args.run_root, args.candidate)
        args.output.write_text(json.dumps(results, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    except (OSError, ProvenanceError) as error:
        parser.error(str(error))
    if commit is None:
        print(f"spatial campaign results written without proven gitCommit: {args.output}", file=sys.stderr)
        return 2
    print(f"spatial campaign results written: {args.output} (gitCommit={commit})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
