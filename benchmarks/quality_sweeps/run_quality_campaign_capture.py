#!/usr/bin/env python3
"""Plan or execute the shared quality-campaign and review-harness capture.

Planning is the default and never launches the player. Live capture requires
the explicit ``--execute`` gate. The implementation lives in
``run_harness_campaign`` so the historical entry point remains compatible.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from benchmarks.quality_sweeps.run_harness_campaign import main


if __name__ == "__main__":
    raise SystemExit(main())
