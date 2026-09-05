#!/usr/bin/env python3
"""Assemble a grounded spatial-only schema-v2 matrix from existing evidence.

Upstream: the checked-in campaign, its candidate review-asset records, and a
completed quality-run ``results.json`` containing one class-attributed raw CSV
per candidate.
Downstream: a reviewable matrix whose identity is strict and whose metric
strings are copied byte-for-byte from the raw CSV.  This module never runs the
player, computes a new metric, or creates temporal rows.
"""

from __future__ import annotations

import csv
import json
from collections.abc import Mapping
from pathlib import Path
from typing import Any

from .campaign_matrix import MatrixError
from .campaign_provenance import ProvenanceError, validate_execution_provenance
from .quality_campaign_contract import validate_campaign
from .paired_spatial_metrics import PAIR_KEY_FIELDS, PAIR_METRICS, SpatialPairingError, pair_spatial_metrics
from .spatial_capture import SpatialCaptureError, load_region_annotations


class SpatialMatrixError(MatrixError):
    """Raised when existing spatial evidence cannot be joined without guessing."""


# These are the only source-to-schema renames permitted at this boundary.  In
# particular, comparator deltas and similarly named fields are not aliases.
CANONICAL_METRIC_MAP = {
    "fsr_psnr_db": "psnr_db",
    "fsr_ssim": "ssim",
    "fsr_edge_ssim": "edge_ssim",
}
_RAW_METADATA_FIELDS = {
    *PAIR_KEY_FIELDS,
    "class",
    "preset",
    "scale",
    "output_path",
    "difference_path",
    "control_source_path",
    "control_source_sha256",
}


def _read_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise SpatialMatrixError(f"cannot read JSON evidence {path}: {error}") from error


def _resolve_campaign_path(value: str, repo_root: Path) -> Path:
    path = Path(value)
    return path if path.is_absolute() else (repo_root / path).resolve()


def _matrix_metrics(raw: dict[str, str]) -> dict[str, str]:
    """Map only proven runner fields and retain every other metric field."""
    metrics: dict[str, str] = {
        destination: raw[source].strip()
        for source, destination in CANONICAL_METRIC_MAP.items()
    }
    for name, value in raw.items():
        if name in _RAW_METADATA_FIELDS or name in CANONICAL_METRIC_MAP:
            continue
        if value is not None and value.strip():
            metrics[name] = value.strip()
    return metrics


def _raw_rows(
    path: Path,
    class_selections: Mapping[str, Any] | None = None,
) -> dict[tuple[str, ...], dict[str, str]]:
    """Read class-attributed spatial rows without assigning classes in code."""
    if not path.is_file():
        raise SpatialMatrixError(f"candidate metrics file does not exist: {path}")
    with path.open("r", encoding="utf-8", newline="") as stream:
        reader = csv.DictReader(stream)
        fields = set(reader.fieldnames or ())
        missing_identity = sorted((set(PAIR_KEY_FIELDS) | set(PAIR_METRICS) | {"class"}) - fields)
        if missing_identity:
            if missing_identity == ["class"]:
                raise SpatialMatrixError(
                    f"{path}: class-attributed spatial CSV requires a 'class' field; "
                    "legacy scene rows cannot be expanded into campaign classes"
                )
            raise SpatialMatrixError(
                f"{path}: missing required fields: {', '.join(missing_identity)}"
            )
        key_fields = (*PAIR_KEY_FIELDS, "class")
        rows: dict[tuple[str, ...], dict[str, str]] = {}
        for row_number, row in enumerate(reader, start=2):
            key = tuple((row.get(field) or "").strip() for field in key_fields)
            if any(not value for value in key):
                raise SpatialMatrixError(f"{path}: row {row_number} has an empty identity field")
            if class_selections is not None:
                scene, quality_class = key[0], key[-1]
                if scene not in class_selections:
                    raise SpatialMatrixError(
                        f"evidence scene not selected: {scene}/{quality_class}"
                    )
                selected = class_selections[scene]
                if quality_class not in selected:
                    raise SpatialMatrixError(
                        f"unexpected class-attributed evidence row for "
                        f"{key[0]}/{quality_class}"
                    )
            if key in rows:
                raise SpatialMatrixError(f"{path}: duplicate pairing key at row {row_number}: {key!r}")
            rows[key] = row
    if not rows:
        raise SpatialMatrixError(f"{path}: no metric rows are available")
    # Validate ancillary provenance only after identity/class semantics. This
    # keeps the actionable error when a row names an impossible class, while
    # still rejecting otherwise valid rows that omit control provenance.
    missing_provenance = sorted({"control_source_path", "control_source_sha256"} - fields)
    if missing_provenance:
        raise SpatialMatrixError(
            f"{path}: missing required fields: {', '.join(missing_provenance)}"
        )
    return rows


def _asset(
    candidate: dict[str, Any], scene: str, frame: int, *, metrics_only: bool = False
) -> dict[str, Any] | None:
    matches = [item for item in candidate.get("reviewAssets", [])
               if item.get("scene") == scene and item.get("frame") == frame]
    if len(matches) != 1:
        if metrics_only and not matches:
            return None
        raise SpatialMatrixError(
            f"candidate {candidate.get('id')!r} must have exactly one review asset for {scene} frame {frame}"
        )
    return dict(matches[0])


def _result_map(results_path: Path, campaign: dict[str, Any], repo_root: Path) -> dict[str, dict[str, Any]]:
    results = _read_json(results_path)
    if not isinstance(results, list):
        raise SpatialMatrixError("results evidence must be a list")
    candidates = {item["id"]: item for item in campaign["candidates"]}
    result_map: dict[str, dict[str, Any]] = {}
    for result in results:
        if not isinstance(result, dict):
            raise SpatialMatrixError("results evidence contains a non-object entry")
        candidate_id = result.get("candidateId")
        if candidate_id not in candidates:
            raise SpatialMatrixError(f"results name unknown campaign candidate: {candidate_id!r}")
        if candidate_id in result_map:
            raise SpatialMatrixError(f"duplicate result for candidate: {candidate_id}")
        if result.get("exitCode") != 0:
            raise SpatialMatrixError(f"candidate {candidate_id} did not complete successfully")
        candidate = candidates[candidate_id]
        for field, expected in (("configSha256", candidate["configSha256"]),
                                ("binarySha256", candidate["binarySha256"])):
            if result.get(field) != expected:
                raise SpatialMatrixError(f"candidate {candidate_id} result {field} does not match campaign")
        result_config = result.get("configSource")
        result_config_path = (
            Path(result_config) if isinstance(result_config, str) and Path(result_config).is_absolute()
            else repo_root / str(result_config)
        )
        if not isinstance(result_config, str) or result_config_path.resolve() != (repo_root / candidate["configPath"]).resolve():
            raise SpatialMatrixError(f"candidate {candidate_id} result configSource does not match campaign")
        if result.get("dimensions") != campaign["dimensions"]["source"]:
            raise SpatialMatrixError(f"candidate {candidate_id} result dimensions do not match campaign")
        if result.get("outputDimensions") != campaign["dimensions"]["output"]:
            raise SpatialMatrixError(f"candidate {candidate_id} result output dimensions do not match campaign")
        if result.get("frame") != campaign["frame"]:
            raise SpatialMatrixError(f"candidate {candidate_id} result frame does not match campaign")
        if not isinstance(result.get("csv"), str):
            raise SpatialMatrixError(f"candidate {candidate_id} result has no CSV path")
        result_map[candidate_id] = result
    missing = sorted(set(candidates) - set(result_map))
    if missing:
        raise SpatialMatrixError(f"missing candidate result evidence: {missing}")
    return result_map


def assemble_spatial_matrix(campaign: dict[str, Any], results_path: Path, repo_root: Path) -> dict[str, Any]:
    """Join producer-attributed class rows to the grounded campaign."""
    try:
        validate_campaign(campaign)
    except Exception as error:
        raise SpatialMatrixError(f"campaign is not valid: {error}") from error
    if campaign.get("executionProvenance") is not None:
        try:
            validate_execution_provenance(
                campaign,
                _read_json(Path(results_path)),
                Path(repo_root),
            )
        except ProvenanceError as error:
            raise SpatialMatrixError(f"campaign execution provenance is invalid: {error}") from error
    class_selections = campaign.get("classSelections")
    if not isinstance(class_selections, Mapping):
        raise SpatialMatrixError(
            "campaign.classSelections is required for scene-grounded spatial assembly"
        )
    region_by_key: dict[tuple[str, str], dict[str, Any]] | None = None
    annotation_value = campaign.get("qualityClassAnnotationsPath")
    if annotation_value is not None:
        if not isinstance(annotation_value, str) or not annotation_value:
            raise SpatialMatrixError("qualityClassAnnotationsPath must be a non-empty path")
        try:
            region_by_key = load_region_annotations(
                _resolve_campaign_path(annotation_value, Path(repo_root)),
                class_selections=class_selections,
                frame=campaign["frame"],
                output_dimensions=campaign["dimensions"]["output"],
                repo_root=Path(repo_root),
                require_asset=campaign.get("evidenceMode") != "metrics_only",
            )
        except (SpatialCaptureError, KeyError) as error:
            raise SpatialMatrixError(f"invalid class-region annotation source: {error}") from error
    results = _result_map(Path(results_path), campaign, Path(repo_root))
    candidates = {item["id"]: item for item in campaign["candidates"]}
    raw_by_candidate: dict[str, dict[tuple[str, ...], dict[str, str]]] = {}
    for candidate_id, result in results.items():
        raw_by_candidate[candidate_id] = _raw_rows(
            Path(result["csv"]), class_selections=class_selections
        )

    baseline_id = campaign["baselineCandidateId"]
    try:
        for candidate_id in candidates:
            if candidate_id == baseline_id:
                continue
            pair_spatial_metrics(
                Path(results[baseline_id]["csv"]), Path(results[candidate_id]["csv"]),
                baseline_id=baseline_id, candidate_id=candidate_id,
            )
    except (SpatialPairingError, KeyError) as error:
        raise SpatialMatrixError(f"existing paired metric evidence cannot be joined: {error}") from error

    rows: list[dict[str, Any]] = []
    selected_scenes = set(campaign["corpus"]["selection"])
    expected_classes_by_scene = {
        scene: set(class_selections[scene]) for scene in campaign["corpus"]["selection"]
    }
    for candidate_id, candidate in candidates.items():
        for key in raw_by_candidate[candidate_id]:
            scene = key[0]
            quality_class = key[-1]
            if scene not in selected_scenes:
                family = (
                    "synthetic"
                    if scene.lower().startswith(
                        ("synthetic_", "synthetic.", "source_", "source.", "supersampled_aa")
                    )
                    else "unselected"
                )
                raise SpatialMatrixError(
                    f"{family} evidence scene is not selected: "
                    f"{candidate_id}/{scene}/{quality_class}"
                )
            if quality_class not in expected_classes_by_scene[scene]:
                raise SpatialMatrixError(
                    f"unexpected class-attributed evidence row for "
                    f"{candidate_id}/{scene}/{quality_class}"
                )
        for scene in campaign["corpus"]["selection"]:
            classes = class_selections.get(scene, [])
            for quality_class in classes:
                source_width, source_height = campaign["dimensions"]["source"].split("x")
                output_width, output_height = campaign["dimensions"]["output"].split("x")
                matches = [
                    (key, raw) for key, raw in raw_by_candidate[candidate_id].items()
                    if key[0] == scene and key[1] == source_width and key[2] == source_height
                    and key[3] == output_width and key[4] == output_height
                    and key[5] == campaign["quality"] and key[7] == str(campaign["frame"])
                    and key[8] == quality_class
                ]
                if not matches:
                    same_frame = [
                        key for key in raw_by_candidate[candidate_id]
                        if key[0] == scene and key[5] == campaign["quality"]
                        and key[7] == str(campaign["frame"])
                    ]
                    if same_frame:
                        raise SpatialMatrixError(
                            f"evidence row dimensions do not match campaign for {candidate_id}/{scene}"
                        )
                    raise SpatialMatrixError(
                        f"missing required evidence row for {candidate_id}/{scene}/{quality_class}"
                    )
                if len(matches) != 1:
                    raise SpatialMatrixError(
                        f"missing or ambiguous required evidence row for {candidate_id}/{scene}/{quality_class}"
                    )
                key, raw = matches[0]
                asset = _asset(
                    candidate,
                    scene,
                    campaign["frame"],
                    metrics_only=campaign.get("evidenceMode") == "metrics_only",
                )
                asset_path = repo_root / asset["path"] if asset is not None else None
                if asset_path is not None and not asset_path.is_file() and campaign.get("evidenceMode") != "metrics_only":
                    raise SpatialMatrixError(f"review asset does not exist: {asset_path}")
                region = None
                if region_by_key is not None:
                    region = region_by_key.get((scene, quality_class))
                    if region is None:
                        raise SpatialMatrixError(
                            f"missing class-region mapping for {scene}/{quality_class}"
                        )
                rows.append({
                    "candidateId": candidate_id,
                    "scene": scene,
                    "qualityClass": quality_class,
                    "inputResolution": campaign["dimensions"]["source"],
                    "outputResolution": campaign["dimensions"]["output"],
                    "frame": campaign["frame"],
                    "provenance": {
                        "gitCommit": candidate["gitCommit"],
                        "binarySha256": candidate["binarySha256"],
                        "configPath": candidate["configPath"],
                        "configSha256": candidate["configSha256"],
                    },
                    "reviewAsset": asset,
                    "metrics": _matrix_metrics(raw),
                    "metricSource": {
                        "path": str(results[candidate_id]["csv"]),
                        "pairKey": list(key),
                        "capturedClass": raw["class"],
                        "controlSource": {
                            "path": raw["control_source_path"],
                            "sha256": raw["control_source_sha256"],
                        },
                        "canonicalMappings": dict(CANONICAL_METRIC_MAP),
                    },
                    "region": (
                        {
                            "x": region["x"],
                            "y": region["y"],
                            "width": region["width"],
                            "height": region["height"],
                        }
                        if region is not None
                        else None
                    ),
                    "regionSource": (
                        {
                            "annotationPath": region["annotationPath"],
                            "assetPath": region["assetPath"],
                            "annotationIndex": region["annotationIndex"],
                        }
                        if region is not None
                        else None
                    ),
                })
    return {
        "schemaVersion": 2,
        "matrixType": "spatial",
        "campaignId": campaign["campaignId"],
        "metricValuesPreserved": True,
        "temporalRows": [],
        "rows": rows,
    }
