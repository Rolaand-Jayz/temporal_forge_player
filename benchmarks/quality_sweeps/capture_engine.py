"""Shared capture primitives used by campaign, qualification, and diagnostics.

This module deliberately contains only policy-neutral operations: renderer
invocation, non-overwriting run roots, artifact hashing, and image validation.
The three front ends supply their own matrix/policy while sharing these
failure semantics and provenance helpers.
"""
from __future__ import annotations

import hashlib
import os
import subprocess
import sys
import time
from pathlib import Path


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def unique_run_root(parent: Path, prefix: str) -> Path:
    """Create a fresh run directory; never reuse or overwrite an old run."""
    parent.mkdir(parents=True, exist_ok=True)
    for _ in range(100):
        candidate = parent / f"{prefix}_{time.strftime('%Y%m%dT%H%M%SZ', time.gmtime())}"
        if not candidate.exists():
            candidate.mkdir()
            return candidate
        time.sleep(1)
    raise RuntimeError(f"unable to allocate unique capture root under {parent}")


def run_renderer(command: list[str], *, cwd: Path, env: dict[str, str] | None = None) -> None:
    """Run one renderer transaction with inherited stdout/stderr for live status."""
    merged = os.environ.copy()
    if env:
        merged.update(env)
    subprocess.run(command, cwd=cwd, env=merged, check=True)


def validate_png(path: Path, expected: tuple[int, int]) -> dict[str, object]:
    """Validate PNG signature, dimensions, non-empty payload, and hash."""
    import struct
    if not path.is_file() or path.stat().st_size == 0:
        raise ValueError(f"missing or empty capture: {path}")
    with path.open("rb") as stream:
        if stream.read(8) != b"\x89PNG\r\n\x1a\n":
            raise ValueError(f"not a PNG capture: {path}")
        stream.seek(16)
        width, height = struct.unpack(">II", stream.read(8))
    if (width, height) != expected:
        raise ValueError(f"capture geometry {width}x{height} != {expected[0]}x{expected[1]}: {path}")
    return {"path": str(path), "width": width, "height": height,
            "bytes": path.stat().st_size, "sha256": sha256(path)}


if __name__ == "__main__":
    if len(sys.argv) < 3 or sys.argv[1] != "--":
        raise SystemExit("usage: python -m benchmarks.quality_sweeps.capture_engine -- COMMAND [ARGS...]")
    run_renderer(sys.argv[2:], cwd=Path.cwd())
