#!/usr/bin/env python3
"""Compatibility entry point for the shared quality campaign capture.

The former resolution-only runner had its own matrix and publication path.
Keeping that implementation would allow it to drift from the checked-in
campaign plan, so all invocations now use the guarded shared runner.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from benchmarks.quality_sweeps.run_harness_campaign import main


if __name__ == "__main__":
    print(
        "run_resolution_matrix.py is deprecated; using the shared guarded "
        "quality campaign runner.",
        file=sys.stderr,
    )
    raise SystemExit(main())
