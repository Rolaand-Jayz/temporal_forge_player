"""Assemble schema-v2 temporal rows from existing captured evidence.

This module is capture-free.  It joins one already-written temporal CSV per
candidate/scene/class with the existing spatial matrix and records the exact
sidecar paths beside each row.  Missing event metrics remain ``None`` so an
incomplete capture cannot be promoted by the assembly step.
"""

from __future__ import annotations

import csv
import json
import math
from collections.abc import Iterable, Mapping
from pathlib import Path
from typing import Any

from .campaign_matrix import MatrixError
from .quality_campaign_contract import validate_campaign


class TemporalMatrixError(MatrixError):
    """Raised when existing temporal evidence cannot be joined safely."""


_FIELDS = (
    "candidateId",
    "scene",
    "configId",
    "startFrame",
    "endFrame",
    "class",
    "frames",
    "width",
    "height",
    "static_flicker",
    "edge_variance",
    "motion_compensated_error",
    "ghost_duration_frames",
    "reset_recovery_frames",
)
_TEMPORAL_METRICS = (
    "static_flicker",
    "edge_variance",
    "motion_compensated_error",
    "ghost_duration_frames",
    "reset_recovery_frames",
)
_EVENT_METRICS = (
    "ghost_duration_frames",
    "reset_recovery_frames",
)
_SPATIAL_TO_MATRIX = {
    "psnr_db": "fsr_psnr_db",
    "ssim": "fsr_ssim",
    "edge_ssim": "fsr_edge_ssim",
}


def _config_identity_resolution(
    captured: str,
    candidate_id: str,
    expected: str,
) -> str | None:
    """Return the evidence-backed config identity form, without rewriting it."""

    expected_path = Path(expected).as_posix()
    if captured in {
        expected_path,
        expected_path.removeprefix("benchmarks/quality_sweeps/"),
    }:
        return "campaign_config_path"
    if captured == candidate_id:
        # The retained capture identifies the config by the candidate token. The
        # campaign still supplies the file path; the raw capture token remains
        # in metricSource/provenance so this is a resolution, not a relabel.
        return "candidate_id_matches_campaign_candidate"
    return None


def _load_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise TemporalMatrixError(f"cannot read JSON evidence {path}: {error}") from error


def _string(value: Any, name: str) -> str:
    if not isinstance(value, str) or not value:
        raise TemporalMatrixError(f"{name} must be a non-empty string")
    return value


def _number(raw: Any, name: str) -> float | None:
    if raw is None or raw == "":
        return None
    try:
        value = float(raw)
    except (TypeError, ValueError) as error:
        raise TemporalMatrixError(f"{name} must be numeric or blank") from error
    if not math.isfinite(value):
        raise TemporalMatrixError(f"{name} must be finite")
    return value


def _integer(raw: Any, name: str) -> int:
    try:
        value = int(raw)
    except (TypeError, ValueError) as error:
        raise TemporalMatrixError(f"{name} must be an integer") from error
    if str(value) != str(raw).strip() and not isinstance(raw, int):
        raise TemporalMatrixError(f"{name} must be an integer")
    return value


def _read_temporal_csv(path: Path) -> dict[str, Any]:
    if not path.is_file():
        raise TemporalMatrixError(f"temporal metrics file does not exist: {path}")
    try:
        with path.open("r", newline="", encoding="utf-8") as stream:
            reader = csv.DictReader(stream)
            fields = set(reader.fieldnames or ())
            missing = sorted(set(_FIELDS) - fields)
            if missing:
                raise TemporalMatrixError(f"{path}: missing required fields: {', '.join(missing)}")
            rows = list(reader)
    except OSError as error:
        raise TemporalMatrixError(f"cannot read temporal metrics file {path}: {error}") from error
    if len(rows) != 1:
        raise TemporalMatrixError(f"{path}: expected exactly one temporal row, found {len(rows)}")
    row = rows[0]
    result: dict[str, Any] = {field: (row.get(field) or "").strip() for field in _FIELDS}
    for field in ("candidateId", "scene", "configId", "class"):
        _string(result[field], f"{path}:{field}")
    for field in ("startFrame", "endFrame", "frames", "width", "height"):
        result[field] = _integer(result[field], f"{path}:{field}")
    for field in _TEMPORAL_METRICS:
        result[field] = _number(result[field], f"{path}:{field}")
    return result


def _spatial_rows(path: Path) -> dict[tuple[str, str, str], dict[str, Any]]:
    value = _load_json(path)
    if not isinstance(value, Mapping) or value.get("matrixType") != "spatial":
        raise TemporalMatrixError(f"spatial evidence is not a spatial matrix: {path}")
    rows = value.get("rows")
    if not isinstance(rows, list):
        raise TemporalMatrixError(f"spatial evidence rows must be a list: {path}")
    result: dict[tuple[str, str, str], dict[str, Any]] = {}
    for row in rows:
        if not isinstance(row, Mapping):
            raise TemporalMatrixError(f"spatial evidence contains a non-object row: {path}")
        key = (_string(row.get("candidateId"), "spatial.candidateId"),
               _string(row.get("scene"), "spatial.scene"),
               _string(row.get("qualityClass"), "spatial.qualityClass"))
        if key in result:
            raise TemporalMatrixError(f"duplicate spatial matrix key: {key}")
        result[key] = dict(row)
    return result


def _expected_keys(campaign: Mapping[str, Any]) -> list[tuple[str, str, str]]:
    class_selections = campaign["classSelections"]
    return [
        (candidate["id"], scene, quality_class)
        for candidate in campaign["candidates"]
        for scene in campaign["corpus"]["selection"]
        for quality_class in class_selections.get(scene, [])
    ]


def _issue(key: tuple[str, str, str], code: str, detail: str) -> dict[str, Any]:
    return {
        "candidateId": key[0],
        "scene": key[1],
        "qualityClass": key[2],
        "code": code,
        "detail": detail,
    }


def assemble_temporal_matrix(
    campaign: Mapping[str, Any],
    spatial_matrix_path: Path,
    temporal_csvs: Iterable[Path | tuple[str, Path]],
    repo_root: Path,
) -> dict[str, Any]:
    """Join supplied CSVs to the grounded campaign without inventing values."""

    try:
        validate_campaign(campaign)
    except Exception as error:
        raise TemporalMatrixError(f"campaign is not valid: {error}") from error

    candidates = {item["id"]: item for item in campaign["candidates"]}
    spatial = _spatial_rows(Path(spatial_matrix_path))
    expected = _expected_keys(campaign)
    supplied: dict[tuple[str, str, str], tuple[Path, dict[str, Any]]] = {}
    for path_value in temporal_csvs:
        declared_candidate: str | None = None
        if isinstance(path_value, tuple):
            declared_candidate, path_value = path_value
            declared_candidate = _string(declared_candidate, "declared candidate id")
        path = Path(path_value)
        row = _read_temporal_csv(path)
        raw_candidate = row["candidateId"]
        candidate_id = declared_candidate or raw_candidate
        key = (candidate_id, row["scene"], row["class"])
        if key not in expected:
            raise TemporalMatrixError(f"temporal CSV names an unexpected row: {key} ({path})")
        if key in supplied:
            raise TemporalMatrixError(f"duplicate temporal CSV for row: {key}")
        if declared_candidate and raw_candidate != declared_candidate:
            row["declaredCandidateId"] = declared_candidate
        supplied[key] = (path, row)

    missing = [key for key in expected if key not in supplied]
    if missing:
        raise TemporalMatrixError(f"missing temporal CSV rows: {missing}")
    if len(supplied) != len(expected):
        raise TemporalMatrixError("temporal CSV set does not have exact campaign coverage")

    rows: list[dict[str, Any]] = []
    issues: list[dict[str, Any]] = []
    evidence_gaps: list[dict[str, Any]] = []
    for key in expected:
        candidate_id, scene, quality_class = key
        candidate = candidates[candidate_id]
        path, raw = supplied[key]
        spatial_row = spatial.get(key)
        if spatial_row is None:
            raise TemporalMatrixError(f"missing spatial row for temporal key: {key}")

        expected_config = candidate["configPath"]
        captured_config = raw["configId"]
        captured_candidate = raw["candidateId"]
        if captured_candidate != candidate_id:
            issues.append(_issue(key, "candidate_identity_mismatch", f"CSV candidateId={captured_candidate!r}; declared candidateId={candidate_id!r}"))
        config_identity_resolution = _config_identity_resolution(
            captured_config,
            candidate_id,
            expected_config,
        )
        if config_identity_resolution is None:
            issues.append(_issue(key, "config_identity_mismatch", f"CSV configId={captured_config!r}; campaign configPath={expected_config!r}"))

        if raw["startFrame"] != campaign["frame"]:
            issues.append(_issue(key, "start_frame_mismatch", f"CSV startFrame={raw['startFrame']}; campaign frame={campaign['frame']}"))
        if raw["width"] != int(campaign["dimensions"]["output"].split("x")[0]) or raw["height"] != int(campaign["dimensions"]["output"].split("x")[1]):
            issues.append(_issue(key, "output_dimension_mismatch", f"CSV dimensions={raw['width']}x{raw['height']}; campaign output={campaign['dimensions']['output']}"))
        if raw["endFrame"] < raw["startFrame"] or raw["frames"] != raw["endFrame"] - raw["startFrame"] + 1:
            issues.append(_issue(key, "frame_range_mismatch", f"CSV start/end/frames={raw['startFrame']}/{raw['endFrame']}/{raw['frames']}"))

        motion_path = path.with_name("codec_motion.json")
        mask_path = path.with_name("static_mask.json")
        if not motion_path.is_file():
            issues.append(_issue(key, "missing_motion_sidecar", str(motion_path)))
        if not mask_path.is_file():
            issues.append(_issue(key, "missing_static_mask", str(mask_path)))
        if any(raw[name] is None for name in _EVENT_METRICS):
            missing_events = [name for name in _EVENT_METRICS if raw[name] is None]
            evidence_gaps.append(_issue(
                key,
                "missing_event_metrics",
                ", ".join(missing_events),
            ))

        metrics: dict[str, float | None] = {}
        for matrix_metric, spatial_metric in _SPATIAL_TO_MATRIX.items():
            value = spatial_row.get("metrics", {}).get(spatial_metric)
            metrics[matrix_metric] = _number(value, f"spatial {key}.{spatial_metric}")
        for name in _TEMPORAL_METRICS:
            metrics[name] = raw[name]

        provenance = dict(spatial_row["provenance"])
        provenance["capturedConfigId"] = captured_config
        provenance["capturedCandidateId"] = captured_candidate
        metric_source = {
            "temporalCsv": str(path),
            "capturedCandidateId": captured_candidate,
            "capturedConfigId": captured_config,
            "expectedConfigPath": expected_config,
            "sidecars": {
                "motion": str(motion_path),
                "staticMask": str(mask_path),
            },
        }
        if config_identity_resolution is not None:
            metric_source["configIdentityResolution"] = config_identity_resolution
            provenance["configIdentityResolution"] = config_identity_resolution
        if "declaredCandidateId" in raw:
            metric_source["declaredCandidateId"] = raw["declaredCandidateId"]

        rows.append({
            "candidateId": candidate_id,
            "scene": scene,
            "qualityClass": quality_class,
            "inputResolution": campaign["dimensions"]["source"],
            "outputResolution": campaign["dimensions"]["output"],
            "frame": campaign["frame"],
            "sequence": {
                "startFrame": raw["startFrame"],
                "endFrame": raw["endFrame"],
                "frames": raw["frames"],
                "width": raw["width"],
                "height": raw["height"],
            },
            "provenance": provenance,
            "reviewAsset": spatial_row["reviewAsset"],
            "metrics": metrics,
            "metricSource": metric_source,
            "spatialMetricSource": spatial_row["metricSource"],
        })

    # The matrix verifier requires both views to carry the full metric contract
    # and to have identical keys.  Keep the original spatial artifact rows
    # separately, while exposing the joined, normalized rows to that verifier.
    normalized_spatial = [dict(row) for row in rows]
    available_metrics = [
        name for name in _TEMPORAL_METRICS
        if all(row["metrics"].get(name) is not None for row in rows)
    ]
    unavailable_metrics = [
        name for name in _TEMPORAL_METRICS
        if any(row["metrics"].get(name) is None for row in rows)
    ]
    temporal_complete = not issues and not evidence_gaps
    return {
        "schemaVersion": 2,
        "campaignId": campaign["campaignId"],
        "matrixType": "combined",
        "complete": temporal_complete,
        "spatialSourcePath": str(Path(spatial_matrix_path)),
        "temporalCsvCount": len(rows),
        "issues": issues,
        "evidenceGaps": evidence_gaps,
        "temporalEvidence": {
            "complete": temporal_complete,
            "status": "complete" if temporal_complete else "pending",
            "availableMetrics": available_metrics,
            "unavailableMetrics": unavailable_metrics,
            "unavailableRows": evidence_gaps,
            "missingEvidence": (
                [
                    "explicit event metadata with ghostEventIndex/ghostThreshold "
                    "and resetIndex/resetThreshold pairs"
                ]
                if unavailable_metrics
                else []
            ),
        },
        "spatial": normalized_spatial,
        "spatialEvidence": list(spatial.values()),
        "temporal": rows,
    }


__all__ = ["TemporalMatrixError", "assemble_temporal_matrix"]
