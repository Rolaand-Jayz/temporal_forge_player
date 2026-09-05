#!/usr/bin/env python3
"""Capture-free contract test for the swarm orchestrator."""

from __future__ import annotations

import json
import tempfile
from pathlib import Path

from sweep_orchestrator import expand_matrix, plan, read_json, validate_dimensions


def main() -> int:
    root = Path(__file__).resolve().parents[4]
    matrix_path = Path(__file__).with_name("matrix.json")
    matrix = read_json(matrix_path)
    jobs = expand_matrix(matrix, matrix_path)
    assert len(jobs) == len(matrix["axes"]["configuration"])
    assert {job["status"] for job in jobs} == {"pending"}
    assert all(job["sourceDimensions"] == "426x240" for job in jobs)
    assert all(job["outputDimensions"] == "1280x720" for job in jobs)
    try:
        validate_dimensions("1280", "test")
    except ValueError:
        pass
    else:
        raise AssertionError("invalid dimensions were accepted")
    with tempfile.TemporaryDirectory(prefix="tforge-orchestrator-test-") as directory:
        path = Path(directory) / "jobs.json"
        manifest = plan(matrix_path, path)
        assert path.is_file()
        assert manifest["mode"] == "csv-and-provenance-only"
        assert read_json(path)["jobs"] == manifest["jobs"]
    assert root.is_dir()
    print(f"capture-free self-test passed ({len(jobs)} jobs; no player launched)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
