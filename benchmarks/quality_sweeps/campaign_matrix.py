#!/usr/bin/env python3
"""Validate the complete candidate/scene/class matrix used by the M6 gate.

Upstream: a schema-validated campaign plus spatial and temporal metric rows.
Downstream: a strict acceptance decision that prevents partial, dimension-drifted,
or provenance-mismatched evidence from being ranked as a complete campaign.
This module only validates existing artifacts; it never launches the player or
changes reconstruction behavior.
"""

from __future__ import annotations

import math
import re
from collections.abc import Mapping, Sequence
from typing import Any

from .quality_campaign_contract import CampaignError, validate_campaign


class MatrixError(CampaignError):
    """Raised when spatial and temporal campaign coverage cannot be joined."""


_COMMIT = re.compile(r"^[0-9a-fA-F]{7,64}$")
_DIMENSION = re.compile(r"^[1-9][0-9]*x[1-9][0-9]*$")
_MATRIX_KEY = (
    "candidateId",
    "scene",
    "qualityClass",
    "inputResolution",
    "outputResolution",
    "frame",
)


def _mapping(value: Any, name: str) -> Mapping[str, Any]:
    if not isinstance(value, Mapping):
        raise MatrixError(f"{name} must be an object")
    return value


def _string(value: Any, name: str) -> str:
    if not isinstance(value, str) or not value:
        raise MatrixError(f"{name} must be a non-empty string")
    return value


def _dimension(value: Any, name: str) -> str:
    text = _string(value, name)
    if not _DIMENSION.fullmatch(text):
        raise MatrixError(f"{name} must be WIDTHxHEIGHT")
    return text


def _finite(value: Any, name: str) -> None:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise MatrixError(f"{name} must be numeric")
    if not math.isfinite(float(value)):
        raise MatrixError(f"{name} must be finite")


def _candidate_map(campaign: Mapping[str, Any]) -> dict[str, Mapping[str, Any]]:
    return {candidate["id"]: candidate for candidate in campaign["candidates"]}


def _row_key(
    row: Any,
    campaign: Mapping[str, Any],
    candidates: Mapping[str, Mapping[str, Any]],
    row_number: int,
) -> tuple[str, str, str, str, str, int]:
    """Validate one row's identity, dimensions, provenance, and metrics."""

    item = _mapping(row, f"matrix row {row_number}")
    candidate_id = _string(item.get("candidateId"), f"matrix row {row_number}.candidateId")
    candidate = candidates.get(candidate_id)
    if candidate is None:
        raise MatrixError(f"matrix row {row_number} names unknown candidate: {candidate_id}")
    scene = _string(item.get("scene"), f"matrix row {row_number}.scene")
    if scene not in campaign["corpus"]["selection"]:
        raise MatrixError(f"matrix row {row_number} names unselected scene: {scene}")
    quality_class = _string(item.get("qualityClass"), f"matrix row {row_number}.qualityClass")
    if quality_class not in campaign["classes"]:
        raise MatrixError(f"matrix row {row_number} names undeclared class: {quality_class}")
    class_selections = campaign.get("classSelections")
    if not isinstance(class_selections, Mapping):
        raise MatrixError(
            "campaign.classSelections is required for scene-grounded matrix validation"
        )
    if quality_class not in class_selections[scene]:
        raise MatrixError(
            f"matrix row {row_number} class is not selected for scene: "
            f"{scene}/{quality_class}"
        )
    input_resolution = _dimension(
        item.get("inputResolution"), f"matrix row {row_number}.inputResolution"
    )
    output_resolution = _dimension(
        item.get("outputResolution"), f"matrix row {row_number}.outputResolution"
    )
    dimensions = candidate["dimensions"]
    if input_resolution != dimensions["source"] or output_resolution != dimensions["output"]:
        raise MatrixError(
            f"matrix row {row_number} dimensions do not match candidate {candidate_id}"
        )
    frame = item.get("frame")
    if isinstance(frame, bool) or not isinstance(frame, int) or frame < 0:
        raise MatrixError(f"matrix row {row_number}.frame must be a non-negative integer")

    provenance = _mapping(item.get("provenance"), f"matrix row {row_number}.provenance")
    commit = provenance.get("gitCommit")
    if commit is None:
        execution = campaign.get("executionProvenance", {})
        candidate_git = candidate.get("gitCommit")
        if not (
            execution.get("gitCommitStatus") == "unrecorded"
            and execution.get("gitCommit") is None
            and execution.get("gitCommitAuthoritative") is False
            and candidate_git is None
        ):
            raise MatrixError(
                f"matrix row {row_number}.provenance.gitCommit is missing without explicit unrecorded provenance"
            )
    else:
        commit = _string(commit, f"matrix row {row_number}.provenance.gitCommit")
        if not _COMMIT.fullmatch(commit):
            raise MatrixError(f"matrix row {row_number}.provenance.gitCommit is not a commit id")
    if provenance.get("binarySha256") != candidate["binarySha256"]:
        raise MatrixError(
            f"matrix row {row_number}.provenance.binarySha256 does not match candidate {candidate_id}"
        )
    if provenance.get("configPath") != candidate["configPath"]:
        raise MatrixError(
            f"matrix row {row_number}.provenance.configPath does not match candidate {candidate_id}"
        )

    metrics = _mapping(item.get("metrics"), f"matrix row {row_number}.metrics")
    for metric in campaign["requiredMetrics"]:
        _finite(metrics.get(metric), f"matrix row {row_number}.metrics.{metric}")
    return candidate_id, scene, quality_class, input_resolution, output_resolution, frame


def _validate_rows(
    rows: Sequence[Any],
    campaign: Mapping[str, Any],
    candidates: Mapping[str, Mapping[str, Any]],
    label: str,
) -> set[tuple[str, str, str, str, str, int]]:
    if not isinstance(rows, Sequence) or isinstance(rows, (str, bytes)) or not rows:
        raise MatrixError(f"{label} matrix must be a non-empty list")
    keys: set[tuple[str, str, str, str, str, int]] = set()
    coverage_keys: set[tuple[str, str, str]] = set()
    for row_number, row in enumerate(rows, start=1):
        key = _row_key(row, campaign, candidates, row_number)
        if key in keys:
            raise MatrixError(f"duplicate {label} matrix key: {key}")
        coverage_key = key[:3]
        if coverage_key in coverage_keys:
            raise MatrixError(
                f"duplicate {label} coverage key: {coverage_key}; "
                "each candidate/scene/qualityClass combination must have exactly one row"
            )
        keys.add(key)
        coverage_keys.add(coverage_key)
    return keys


def _coverage_keys(
    campaign: Mapping[str, Any],
    candidates: Mapping[str, Mapping[str, Any]],
) -> set[tuple[str, str, str, str, str]]:
    expected: set[tuple[str, str, str, str, str]] = set()
    class_selections = campaign.get("classSelections")
    if not isinstance(class_selections, Mapping):
        raise MatrixError(
            "campaign.classSelections is required for scene-grounded matrix coverage"
        )
    for candidate_id, candidate in candidates.items():
        dimensions = candidate["dimensions"]
        for scene in campaign["corpus"]["selection"]:
            scene_classes = class_selections[scene]
            for quality_class in scene_classes:
                expected.add(
                    (
                        candidate_id,
                        scene,
                        quality_class,
                        dimensions["source"],
                        dimensions["output"],
                    )
                )
    return expected


def validate_complete_matrix(
    campaign: Mapping[str, Any],
    spatial_rows: Sequence[Any],
    temporal_rows: Sequence[Any],
) -> None:
    """Require exact candidate/scene/class coverage and spatial/temporal parity."""

    validate_campaign(campaign)
    if not isinstance(campaign.get("classSelections"), Mapping):
        raise MatrixError(
            "campaign.classSelections is required for scene-grounded matrix validation"
        )
    candidates = _candidate_map(campaign)
    spatial_keys = _validate_rows(spatial_rows, campaign, candidates, "spatial")
    temporal_keys = _validate_rows(temporal_rows, campaign, candidates, "temporal")
    if spatial_keys != temporal_keys:
        raise MatrixError(
            "spatial and temporal matrix keys do not match; "
            f"spatial_only={sorted(spatial_keys - temporal_keys)}, "
            f"temporal_only={sorted(temporal_keys - spatial_keys)}"
        )
    coverage = {(candidate, scene, quality_class, source, output) for candidate, scene, quality_class, source, output, _ in spatial_keys}
    expected = _coverage_keys(campaign, candidates)
    if coverage != expected:
        raise MatrixError(
            "campaign matrix coverage mismatch; "
            f"missing={sorted(expected - coverage)}, extra={sorted(coverage - expected)}"
        )
