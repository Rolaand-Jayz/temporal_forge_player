#!/usr/bin/env python3
"""Resumable, disk-safe planner/runner for explicit Temporal Forge sweeps.

Planning is the default and never starts a player.  Capture execution is an
explicit opt-in and delegates each job to the repository's existing
``run_quality_sweep.py`` with one worker.  The manifest is updated atomically
after every state transition, so an interrupted run can be resumed safely.
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import hashlib
import itertools
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[4]
SWEEP_RUNNER = ROOT / "benchmarks/quality_sweeps/run_quality_sweep.py"
DIMENSIONS = re.compile(r"^[1-9][0-9]*x[1-9][0-9]*$")
SAFE_ID = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]*$")


def read_json(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"expected JSON object: {path}")
    return value


def atomic_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            json.dump(value, stream, indent=2, sort_keys=True)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(name, path)
    except BaseException:
        try:
            os.unlink(name)
        except FileNotFoundError:
            pass
        raise


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def validate_dimensions(value: Any, field: str) -> str:
    if not isinstance(value, str) or not DIMENSIONS.fullmatch(value):
        raise ValueError(f"{field} must be WIDTHxHEIGHT")
    return value


def expand_matrix(matrix: dict[str, Any], matrix_path: Path) -> list[dict[str, Any]]:
    if matrix.get("schema") != "temporal_forge.quality_sweep_matrix.v1":
        raise ValueError("unsupported matrix schema")
    defaults = matrix.get("defaults")
    axes = matrix.get("axes")
    if not isinstance(defaults, dict) or not isinstance(axes, dict) or not axes:
        raise ValueError("matrix requires defaults and non-empty axes")
    source = validate_dimensions(defaults.get("sourceDimensions"), "sourceDimensions")
    output = validate_dimensions(defaults.get("outputDimensions"), "outputDimensions")
    if not isinstance(defaults.get("minFreeBytes"), int) or defaults["minFreeBytes"] < 0:
        raise ValueError("minFreeBytes must be a non-negative integer")
    names = list(axes)
    values: list[list[Any]] = []
    for name in names:
        items = axes[name]
        if not isinstance(items, list) or not items:
            raise ValueError(f"axis {name} must be a non-empty list")
        values.append(items)
    jobs = []
    for index, combination in enumerate(itertools.product(*values), start=1):
        fields = dict(defaults)
        selected = dict(zip(names, combination))
        config = selected.get("configuration")
        if not isinstance(config, dict) or not isinstance(config.get("id"), str) or not isinstance(config.get("path"), str):
            raise ValueError("configuration axis entries require id and path")
        job_id = f"{index:04d}-{config['id']}"
        if not SAFE_ID.fullmatch(job_id):
            raise ValueError(f"unsafe generated job id: {job_id}")
        config_path = (ROOT / config["path"]).resolve()
        if not config_path.is_file():
            raise FileNotFoundError(config_path)
        fields.update({key: value for key, value in selected.items() if key != "configuration"})
        jobs.append({
            "jobId": job_id,
            "configurationId": config["id"],
            "configurationPath": str(config_path),
            "sourceDimensions": source,
            "outputDimensions": output,
            "fields": fields,
            "configurationSha256": sha256(config_path),
            "status": "pending",
            "attempts": [],
        })
    return jobs


def plan(matrix_path: Path, manifest_path: Path) -> dict[str, Any]:
    matrix = read_json(matrix_path)
    jobs = expand_matrix(matrix, matrix_path)
    manifest = {
        "schema": "temporal_forge.quality_sweep_job_manifest.v1",
        "createdUtc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "mode": "csv-and-provenance-only",
        "captureExecution": "disabled-unless-explicitly-enabled",
        "matrix": str(matrix_path.resolve()),
        "matrixSha256": sha256(matrix_path),
        "jobs": jobs,
    }
    atomic_json(manifest_path, manifest)
    return manifest


def free_bytes(path: Path) -> int:
    return shutil.disk_usage(path).free


def child_manifest(job: dict[str, Any], path: Path) -> None:
    fields = job["fields"]
    atomic_json(path, {
        "dimensions": fields["sourceDimensions"],
        "outputDimensions": fields["outputDimensions"],
        "frame": fields["frame"],
        "quality": fields["quality"],
        "preset": fields["preset"],
        "clipRegex": fields["clipRegex"],
        "corpusManifest": fields["corpusManifest"],
        "jitter": fields["jitter"],
        "experiments": [{"id": job["jobId"], "config": job["configurationPath"]}],
    })


def validate_csv_dimensions(csv_path: Path, expected: str) -> None:
    expected_width, expected_height = expected.split("x")
    with csv_path.open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    if not rows:
        raise ValueError(f"empty quality CSV: {csv_path}")
    for row in rows:
        if row.get("output_width") != expected_width or row.get("output_height") != expected_height:
            raise ValueError(f"dimension mismatch in {csv_path}: expected {expected}")


def run_manifest(manifest_path: Path, *, binary: Path, output_root: Path, retries: int, execute: bool) -> int:
    manifest = read_json(manifest_path)
    if manifest.get("schema") != "temporal_forge.quality_sweep_job_manifest.v1":
        raise ValueError("unsupported job manifest schema")
    if not execute:
        print(f"capture execution disabled; {len(manifest['jobs'])} jobs remain planned")
        return 0
    output_root.mkdir(parents=True, exist_ok=True)
    for job in manifest["jobs"]:
        if job["status"] == "succeeded":
            continue
        fields = job["fields"]
        for attempt in range(len(job["attempts"]), retries + 1):
            available = free_bytes(output_root)
            minimum = fields["minFreeBytes"]
            if available < minimum:
                job["status"] = "blocked-free-space"
                atomic_json(manifest_path, manifest)
                raise RuntimeError(f"free space {available} below minimum {minimum}")
            attempt_root = output_root / job["jobId"] / f"attempt-{attempt:02d}"
            attempt_root.mkdir(parents=True, exist_ok=False)
            plan_path = attempt_root / "delegated_manifest.json"
            child_manifest(job, plan_path)
            record = {"attempt": attempt, "freeBytesBefore": available, "outputRoot": str(attempt_root)}
            command = [sys.executable, str(SWEEP_RUNNER), "--manifest", str(plan_path), "--binary", str(binary), "--output-root", str(attempt_root / "sweep"), "--workers", "1", "--retries", "0"]
            completed = subprocess.run(command, cwd=ROOT, check=False)
            record["command"] = command
            record["exitCode"] = completed.returncode
            result_csv = next(attempt_root.glob("sweep/*/*/*/quality.csv"), None)
            try:
                if completed.returncode != 0 or result_csv is None:
                    raise ValueError("delegated runner failed or produced no quality CSV")
                validate_csv_dimensions(result_csv, fields["outputDimensions"])
                record["csv"] = str(result_csv)
                record["status"] = "succeeded"
                job["status"] = "succeeded"
                job["attempts"].append(record)
                atomic_json(manifest_path, manifest)
                break
            except ValueError as error:
                record["error"] = str(error)
            record["status"] = "failed"
            job["attempts"].append(record)
            job["status"] = "failed"
            atomic_json(manifest_path, manifest)
        if job["status"] != "succeeded":
            return 2
    atomic_json(manifest_path, manifest)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    p_plan = sub.add_parser("plan")
    p_plan.add_argument("--matrix", type=Path, default=Path(__file__).with_name("matrix.json"))
    p_plan.add_argument("--manifest", type=Path, required=True)
    p_run = sub.add_parser("run")
    p_run.add_argument("--manifest", type=Path, required=True)
    p_run.add_argument("--binary", type=Path, required=True)
    p_run.add_argument("--output-root", type=Path, required=True)
    p_run.add_argument("--retries", type=int, default=2)
    p_run.add_argument("--execute-captures", action="store_true", help="required to launch existing capture runner")
    args = parser.parse_args()
    if args.command == "plan":
        manifest = plan(args.matrix.resolve(), args.manifest.resolve())
        print(f"planned {len(manifest['jobs'])} jobs: {args.manifest}")
        return 0
    if args.retries < 0:
        raise ValueError("retries must be non-negative")
    return run_manifest(args.manifest.resolve(), binary=args.binary.resolve(), output_root=args.output_root.resolve(), retries=args.retries, execute=args.execute_captures)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError, json.JSONDecodeError) as error:
        print(f"orchestrator error: {error}", file=sys.stderr)
        raise SystemExit(2)
