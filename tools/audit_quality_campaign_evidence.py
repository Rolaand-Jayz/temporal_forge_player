#!/usr/bin/env python3
"""Classify historical quality-campaign evidence without requiring images.

An image is useful for review, but it is not evidence that an arm ran.  This
audit consumes the durable data products: experiment manifests, runtime
traces, CSV rows, hashes, and explicit invalidation markers.  It emits both a
machine-readable report and a short Markdown summary.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from collections import defaultdict
from pathlib import Path


METHODS = (
    "current_cas20",
    "base_only_bilinear_cas20",
    "fsr_direct_cas20",
    *(
        f"fsr_{int(scale * 100):03d}x_downsample_{suffix}"
        for scale in (2.00, 2.25, 2.50, 2.75, 3.00)
        for suffix in ("resolve_cas20", "external_post_cas20", "no_cas")
    ),
    "fsr_nativeaa_downsample_resolve_cas20",
    "fsr_nativeaa_downsample_external_post_cas20",
    "fsr_nativeaa_downsample_no_cas",
    "conventional_lanczos",
    "conventional_bicubic",
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def json_files(root: Path, name: str) -> list[Path]:
    return sorted(path for path in root.rglob(name) if path.is_file())


def inspect_record(path: Path) -> tuple[str, list[str], dict[str, object]]:
    try:
        record = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        return "INVALID", [f"unreadable experiment record: {error}"], {"record": str(path)}
    reasons: list[str] = []
    required = ("experiment_id", "run_id", "output_sha256", "runtime_trace", "metrics")
    reasons.extend(f"missing {key}" for key in required if not record.get(key))
    trace = record.get("runtime_trace")
    if not isinstance(trace, dict):
        reasons.append("runtime semantic trace missing")
    elif trace.get("run_id") != record.get("run_id"):
        reasons.append("runtime trace/run ID mismatch")
    output = record.get("output_artifact", record.get("output_path"))
    if output and Path(output).is_file() and record.get("output_sha256"):
        if sha256(Path(output)) != record["output_sha256"]:
            reasons.append("output hash mismatch")
    elif output:
        reasons.append("output artifact unavailable (data-only evidence)")
    status = "VALID" if not reasons else "INCOMPLETE PROVENANCE"
    return status, reasons, {
        "record": str(path),
        "experiment_id": record.get("experiment_id"),
        "run_id": record.get("run_id"),
        "method": record.get("method", record.get("arm_id", "unknown")),
        "scene": record.get("scene"),
        "status": record.get("status", "unknown"),
    }


def audit(root: Path) -> dict[str, object]:
    invalidated = [path for path in json_files(root, "INVALIDATED.json")]
    invalid_roots = {path.parent for path in invalidated}
    records: list[dict[str, object]] = []
    method_runs: defaultdict[str, set[str]] = defaultdict(set)
    method_records: defaultdict[str, list[dict[str, object]]] = defaultdict(list)

    for path in json_files(root, "experiment.json"):
        status, reasons, detail = inspect_record(path)
        detail["classification"] = status
        detail["reasons"] = reasons
        if any(path.is_relative_to(invalid_root) for invalid_root in invalid_roots):
            detail["classification"] = "INVALID"
            detail["reasons"] = ["artifact root explicitly invalidated", *reasons]
        records.append(detail)
        method = str(detail["method"])
        method_runs[method].add(str(detail.get("run_id")))
        method_records[method].append(detail)

    # CSV-only historical roots are retained as evidence, but cannot be called
    # valid because they predate per-arm experiment manifests/traces.
    for path in json_files(root, "raw.csv"):
        if any(path.parent == Path(item["record"]).parent for item in records):
            continue
        try:
            with path.open(newline="", encoding="utf-8") as stream:
                row_count = sum(1 for _ in csv.DictReader(stream))
        except (OSError, UnicodeError, csv.Error):
            row_count = None
        records.append({
            "record": str(path),
            "classification": "INCOMPLETE PROVENANCE",
            "reasons": ["CSV exists without per-arm experiment.json/runtime trace"],
            "row_count": row_count,
        })

    classifications: dict[str, str] = {}
    for method in METHODS:
        entries = method_records.get(method, [])
        if not entries:
            classifications[method] = "RECAPTURE REQUIRED"
            continue
        classes = {str(entry["classification"]) for entry in entries}
        runs = {str(entry.get("run_id")) for entry in entries}
        if len(runs) < len(entries) and len(entries) > 1:
            classifications[method] = "DUPLICATED ARM"
        elif "INVALID" in classes:
            classifications[method] = "INVALID"
        elif classes == {"VALID"}:
            classifications[method] = "VALID"
        else:
            classifications[method] = "INCOMPLETE PROVENANCE"

    return {
        "schema": "temporal_forge.quality_evidence_audit.v1",
        "root": str(root),
        "image_payloads_required": False,
        "invalidated_roots": sorted(str(path) for path in invalid_roots),
        "methods": classifications,
        "records": records,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("root", type=Path)
    parser.add_argument("--json", type=Path, required=True)
    parser.add_argument("--markdown", type=Path, required=True)
    args = parser.parse_args()
    result = audit(args.root)
    args.json.parent.mkdir(parents=True, exist_ok=True)
    args.json.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    lines = [
        "# Quality campaign evidence audit",
        "",
        f"Root: `{result['root']}`",
        "",
        "Image payloads are not required; classifications use durable data products.",
        "",
        "| Method | Classification |",
        "|---|---|",
    ]
    lines.extend(f"| `{method}` | {classification} |"
                 for method, classification in result["methods"].items())
    args.markdown.parent.mkdir(parents=True, exist_ok=True)
    args.markdown.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(json.dumps({"methods": len(result["methods"]), "records": len(result["records"])}, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
