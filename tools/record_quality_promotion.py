#!/usr/bin/env python3
"""Write an auditable M7.5 promotion or non-promotion record.

This tool never edits runtime defaults or copies binaries. It turns the already
verified evidence into a release decision, preserving the known-good rollback
identity and the reasons a candidate was not promoted. A candidate may only be
marked promoted when every declared gate passes and rollback metadata exists.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import sys
from pathlib import Path


REQUIRED = (
    "schema", "campaignId", "candidateId", "referenceCandidateId",
    "quality", "equivalence", "performance", "diagnostics", "rollback",
    "binary", "artifacts", "limitations",
)


def fail(message: str) -> int:
    print(message, file=sys.stderr)
    return 2


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evidence", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        evidence = json.loads(args.evidence.read_text())
    except (OSError, json.JSONDecodeError) as error:
        return fail(str(error))
    if not isinstance(evidence, dict):
        return fail("promotion evidence must be a JSON object")
    missing = [key for key in REQUIRED if key not in evidence]
    if missing:
        return fail("promotion evidence missing: " + ", ".join(missing))

    rollback = evidence["rollback"]
    if not isinstance(rollback, dict) or not rollback.get("configPath") or not rollback.get("candidateId"):
        return fail("rollback configPath and candidateId are required")
    binary = evidence["binary"]
    if not isinstance(binary, dict) or not binary.get("path") or not binary.get("sha256"):
        return fail("binary path and sha256 are required")

    gates = {
        "quality": bool(evidence["quality"].get("passed")),
        "equivalence": bool(evidence["equivalence"].get("passed")),
        "performance": bool(evidence["performance"].get("passed")),
        "diagnostics": bool(evidence["diagnostics"].get("disabledByDefault")),
    }
    reasons = []
    for name, passed in gates.items():
        if not passed:
            detail = evidence[name].get("reason") if isinstance(evidence[name], dict) else None
            reasons.append(detail or f"{name} gate did not pass")

    promoted = not reasons
    record = {
        "schema": "temporal_forge.m7_promotion_record.v1",
        "recordedAtUtc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "campaignId": evidence["campaignId"],
        "candidateId": evidence["candidateId"],
        "referenceCandidateId": evidence["referenceCandidateId"],
        "status": "promoted" if promoted else "not_promoted",
        "gates": gates,
        "reasons": reasons,
        "rollback": rollback,
        "binary": binary,
        "artifacts": evidence["artifacts"],
        "limitations": evidence["limitations"],
        "defaultMutation": "none",
    }
    try:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")
    except OSError as error:
        return fail(str(error))
    print(json.dumps(record, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
