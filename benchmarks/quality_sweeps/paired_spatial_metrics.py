#!/usr/bin/env python3
"""Strict, paired spatial metrics for comparable Temporal Forge candidates.

The quality runner produces one CSV per candidate.  This module is the small
boundary between those raw captures and a comparison: it requires the same
clip, source dimensions, output dimensions, codec settings, and frame on both
sides before it calculates a delta.  When class-attributed rows are present,
the captured class is part of that identity as well. That keeps a missing
capture or a changed benchmark tuple from becoming a misleading ranking.
"""

from __future__ import annotations

import csv
import json
import math
import re
from pathlib import Path
from statistics import fmean, median
from typing import Any, Iterable


# Upstream: the corpus runner's stable CSV schema.  Downstream: strict row
# pairing, per-row deltas, and aggregate candidate comparisons.
PAIR_KEY_FIELDS = (
    "clip_id",
    "width",
    "height",
    "output_width",
    "output_height",
    "quality",
    "crf",
    "frame",
)

# These metrics are deliberately limited to values that are present in the
# corrected spatial campaign.  Adding a metric requires a real CSV producer
# and a test for its comparison semantics; it must not silently become null.
PAIR_METRICS = (
    "fsr_psnr_db",
    "fsr_ssim",
    "fsr_edge_ssim",
    "fsr_lowfreq_luma_mae",
    "fsr_lowfreq_luma_bias",
)

# Higher PSNR/SSIM is better, while a larger error delta is worse.  Bias is
# signed, so its worst case is the delta with the largest absolute magnitude.
HIGHER_IS_BETTER = {"fsr_psnr_db", "fsr_ssim", "fsr_edge_ssim"}
ERROR_METRICS = {"fsr_lowfreq_luma_mae"}
BIAS_METRICS = {"fsr_lowfreq_luma_bias"}


class SpatialPairingError(ValueError):
    """Raised when two candidate CSVs cannot be compared without guessing."""


def _parse_number(value: str | None, *, field: str, path: Path, row_number: int) -> float:
    """Parse one finite metric and reject blanks, NaN, and infinity."""
    if value is None or not value.strip():
        raise SpatialPairingError(
            f"{path}: row {row_number} is missing required numeric field {field!r}"
        )
    try:
        number = float(value)
    except ValueError as error:
        raise SpatialPairingError(
            f"{path}: row {row_number} has non-numeric {field!r}: {value!r}"
        ) from error
    if not math.isfinite(number):
        raise SpatialPairingError(
            f"{path}: row {row_number} has non-finite {field!r}: {value!r}"
        )
    return number


def _key_value(value: str | None, *, field: str, path: Path, row_number: int) -> str:
    """Normalize one comparison-key field without changing its meaning."""
    if value is None or not value.strip():
        raise SpatialPairingError(
            f"{path}: row {row_number} is missing pairing field {field!r}"
        )
    return value.strip()


def _read_candidate_rows(path: Path) -> dict[tuple[str, ...], dict[str, Any]]:
    """Read and validate one runner CSV into a unique pairing-key map."""
    if not path.is_file():
        raise SpatialPairingError(f"candidate metrics file does not exist: {path}")

    with path.open("r", encoding="utf-8", newline="") as stream:
        reader = csv.DictReader(stream)
        fields = set(reader.fieldnames or ())
        has_class = "class" in fields
        key_fields = (*PAIR_KEY_FIELDS, "class") if has_class else PAIR_KEY_FIELDS
        required = set(PAIR_KEY_FIELDS) | set(PAIR_METRICS)
        missing = sorted(required - fields)
        if missing:
            raise SpatialPairingError(f"{path}: missing required fields: {', '.join(missing)}")

        parsed: dict[tuple[str, ...], dict[str, Any]] = {}
        for row_number, raw_row in enumerate(reader, start=2):
            key = tuple(
                _key_value(raw_row.get(field), field=field, path=path, row_number=row_number)
                for field in key_fields
            )
            if key in parsed:
                raise SpatialPairingError(
                    f"{path}: duplicate pairing key at row {row_number}: {key!r}"
                )
            control_path = (raw_row.get("control_source_path") or "").strip()
            control_sha256 = (raw_row.get("control_source_sha256") or "").strip()
            if bool(control_path) != bool(control_sha256):
                raise SpatialPairingError(
                    f"{path}: row {row_number} has incomplete control-source provenance"
                )
            if control_sha256 and not re.fullmatch(r"[0-9a-f]{64}", control_sha256):
                raise SpatialPairingError(
                    f"{path}: row {row_number} has invalid control-source SHA-256"
                )
            parsed[key] = {
                "key": key,
                "fields": {field: raw_row[field].strip() for field in key_fields},
                "metrics": {
                    field: _parse_number(
                        raw_row.get(field), field=field, path=path, row_number=row_number
                    )
                    for field in PAIR_METRICS
                },
                "controlSource": (
                    {"path": control_path, "sha256": control_sha256}
                    if control_path
                    else None
                ),
            }
    if not parsed:
        raise SpatialPairingError(f"{path}: no metric rows are available for pairing")
    return parsed


def _display_dimensions(fields: dict[str, str]) -> dict[str, str]:
    """Expose source/output dimensions in a readable, stable report shape."""
    return {
        "input": f"{fields['width']}x{fields['height']}",
        "output": f"{fields['output_width']}x{fields['output_height']}",
    }


def _worst_delta(metric: str, values: list[float]) -> float:
    """Return the directionally worst observed candidate-minus-baseline delta."""
    if metric in HIGHER_IS_BETTER:
        return min(values)
    if metric in ERROR_METRICS:
        return max(values)
    if metric in BIAS_METRICS:
        return max(values, key=abs)
    raise SpatialPairingError(f"no worst-case rule exists for metric {metric!r}")


def _summary(rows: list[dict[str, Any]]) -> dict[str, dict[str, float]]:
    """Calculate mean, median, and directionally worst deltas for each metric."""
    summaries: dict[str, dict[str, float]] = {}
    for metric in PAIR_METRICS:
        values = [float(row["delta"][metric]) for row in rows]
        summaries[metric] = {
            "mean": fmean(values),
            "median": float(median(values)),
            "worst": _worst_delta(metric, values),
        }

    # The public report is easier to consume when each aggregate is grouped by
    # statistic rather than requiring every consumer to transpose the table.
    return {
        "mean": {metric: values["mean"] for metric, values in summaries.items()},
        "median": {metric: values["median"] for metric, values in summaries.items()},
        "worst": {metric: values["worst"] for metric, values in summaries.items()},
    }


def pair_spatial_metrics(
    baseline_path: Path,
    candidate_path: Path,
    *,
    baseline_id: str,
    candidate_id: str,
) -> dict[str, Any]:
    """Pair two strict CSVs and return auditable per-row and aggregate deltas."""
    baseline_rows = _read_candidate_rows(Path(baseline_path))
    candidate_rows = _read_candidate_rows(Path(candidate_path))
    baseline_keys = set(baseline_rows)
    candidate_keys = set(candidate_rows)
    if baseline_keys != candidate_keys:
        missing = sorted(baseline_keys - candidate_keys)
        unexpected = sorted(candidate_keys - baseline_keys)
        raise SpatialPairingError(
            "pairing keys differ; "
            f"missing from candidate={missing!r}, unexpected in candidate={unexpected!r}"
        )

    rows: list[dict[str, Any]] = []
    for key in sorted(baseline_keys):
        baseline = baseline_rows[key]
        candidate = candidate_rows[key]
        baseline_control = baseline["controlSource"]
        candidate_control = candidate["controlSource"]
        if (baseline_control is None) != (candidate_control is None):
            raise SpatialPairingError(
                f"control-source provenance differs for pairing key {key!r}"
            )
        if baseline_control is not None and (
            baseline_control["sha256"] != candidate_control["sha256"]
        ):
            raise SpatialPairingError(
                f"control source pixels differ for pairing key {key!r}"
            )
        delta = {
            metric: candidate["metrics"][metric] - baseline["metrics"][metric]
            for metric in PAIR_METRICS
        }
        fields = baseline["fields"]
        rows.append(
            {
                "clipId": fields["clip_id"],
                "dimensions": _display_dimensions(fields),
                "quality": fields["quality"],
                "crf": int(fields["crf"]),
                "frame": int(fields["frame"]),
                "baseline": baseline["metrics"],
                "candidate": candidate["metrics"],
                "baselineControlSource": baseline_control,
                "candidateControlSource": candidate_control,
                "delta": delta,
            }
        )

    return {
        "baselineCandidateId": baseline_id,
        "candidateId": candidate_id,
        "rowCount": len(rows),
        "rows": rows,
                "summary": _summary(rows),
    }


def _result_csv(result: dict[str, Any]) -> Path:
    """Resolve a successful sweep result's raw metrics path."""
    csv_value = result.get("csv")
    if not isinstance(csv_value, str):
        raise SpatialPairingError(f"candidate {result.get('candidateId')!r} has no CSV path")
    return Path(csv_value)


def write_paired_reports(
    run_root: Path,
    results: Iterable[dict[str, Any]],
    *,
    baseline_id: str,
) -> list[dict[str, Any]]:
    """Write per-candidate paired JSON and aggregate CSV/JSON rankings.

    Upstream: successful `run_quality_sweep.py` candidate results.  Downstream:
    reviewer-facing paired evidence beside the raw candidate artifacts.  Every
    non-baseline candidate is paired; failures are rejected instead of being
    omitted from the report.
    """
    result_list = list(results)
    by_id = {result.get("candidateId"): result for result in result_list}
    baseline = by_id.get(baseline_id)
    if baseline is None:
        raise SpatialPairingError(f"baseline candidate is not present: {baseline_id!r}")
    if baseline.get("exitCode") != 0:
        raise SpatialPairingError(f"baseline candidate failed: {baseline_id!r}")

    baseline_path = _result_csv(baseline)
    paired_dir = run_root / "paired"
    paired_dir.mkdir(exist_ok=True)
    reports: list[dict[str, Any]] = []
    for result in sorted(result_list, key=lambda item: str(item.get("candidateId"))):
        candidate_id = result.get("candidateId")
        if candidate_id == baseline_id:
            continue
        if result.get("exitCode") != 0:
            raise SpatialPairingError(f"candidate failed and cannot be paired: {candidate_id!r}")
        if not isinstance(candidate_id, str) or not candidate_id:
            raise SpatialPairingError(f"candidate has invalid id: {candidate_id!r}")
        report = pair_spatial_metrics(
            baseline_path,
            _result_csv(result),
            baseline_id=baseline_id,
            candidate_id=candidate_id,
        )
        reports.append(report)
        (paired_dir / f"{candidate_id}.json").write_text(
            json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )

    ranking_rows: list[dict[str, Any]] = []
    for report in reports:
        ranking_rows.append(
            {
                "baselineCandidateId": report["baselineCandidateId"],
                "candidateId": report["candidateId"],
                "rowCount": report["rowCount"],
                "meanFsrPsnrDbDelta": report["summary"]["mean"]["fsr_psnr_db"],
                "medianFsrPsnrDbDelta": report["summary"]["median"]["fsr_psnr_db"],
                "worstFsrPsnrDbDelta": report["summary"]["worst"]["fsr_psnr_db"],
                "meanFsrSsimDelta": report["summary"]["mean"]["fsr_ssim"],
                "medianFsrSsimDelta": report["summary"]["median"]["fsr_ssim"],
                "worstFsrSsimDelta": report["summary"]["worst"]["fsr_ssim"],
                "meanFsrEdgeSsimDelta": report["summary"]["mean"]["fsr_edge_ssim"],
                "meanLowfreqLumaMaeDelta": report["summary"]["mean"]["fsr_lowfreq_luma_mae"],
                "worstLowfreqLumaMaeDelta": report["summary"]["worst"]["fsr_lowfreq_luma_mae"],
                "meanLowfreqLumaBiasDelta": report["summary"]["mean"]["fsr_lowfreq_luma_bias"],
                "worstLowfreqLumaBiasDelta": report["summary"]["worst"]["fsr_lowfreq_luma_bias"],
            }
        )
    ranking_rows.sort(
        key=lambda row: (
            row["meanFsrSsimDelta"],
            row["meanFsrEdgeSsimDelta"],
            row["candidateId"],
        ),
        reverse=True,
    )
    ranking_fields = list(ranking_rows[0]) if ranking_rows else ["candidateId"]
    with (run_root / "paired_rankings.csv").open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=ranking_fields)
        writer.writeheader()
        writer.writerows(ranking_rows)
    (run_root / "paired_rankings.json").write_text(
        json.dumps(ranking_rows, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return reports
