#!/usr/bin/env python3
"""Compare two serialized numeric stages without making a visual-quality claim.

The M7.2 gate is about semantic equivalence between a clear reference path and
an optimized/fused path. Inputs are JSON arrays (or objects with a ``values``
array). The declared storage format selects a documented numeric tolerance;
the tool reports the errors and never ranks one image as visually preferable.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path


# Tolerances cover representation/rounding noise for the declared formats.
# They are intentionally explicit and conservative; quality preference belongs
# to the paired quality campaign, not this correctness gate.
FORMAT_TOLERANCES = {
    "rgba8": 1.0 / 255.0 + 1.0e-6,
    "rgba16f": 2.0e-3,
    "tensor_fp16": 2.0e-3,
    "tensor_int8": 1.0 / 127.0 + 1.0e-6,
}


def load_values(path: Path) -> list[float]:
    payload = json.loads(path.read_text())
    if isinstance(payload, dict):
        payload = payload.get("values")
    if not isinstance(payload, list):
        raise ValueError(f"{path}: expected a JSON array or object with values")
    values = [float(value) for value in payload]
    if not all(math.isfinite(value) for value in values):
        raise ValueError(f"{path}: non-finite value present")
    return values


def compare(reference: list[float], candidate: list[float], fmt: str) -> dict:
    if len(reference) != len(candidate):
        raise ValueError(f"shape mismatch: reference has {len(reference)} values, "
                         f"candidate has {len(candidate)}")
    tolerance = FORMAT_TOLERANCES[fmt]
    errors = [abs(actual - expected) for expected, actual in zip(reference, candidate)]
    max_error = max(errors, default=0.0)
    rmse = math.sqrt(sum(error * error for error in errors) / len(errors)) if errors else 0.0
    return {
        "equivalent": max_error <= tolerance,
        "format": fmt,
        "samples": len(reference),
        "tolerance": tolerance,
        "max_abs_error": max_error,
        "rmse": rmse,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--format", choices=sorted(FORMAT_TOLERANCES), required=True)
    args = parser.parse_args()
    try:
        report = compare(load_values(args.reference), load_values(args.candidate), args.format)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(str(error), file=sys.stderr)
        return 2
    print(json.dumps(report, sort_keys=True))
    return 0 if report["equivalent"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
