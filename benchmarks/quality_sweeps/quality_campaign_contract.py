#!/usr/bin/env python3
"""Validate M6 learned-campaign evidence before any candidate is promoted."""

from __future__ import annotations

import math
import re
import csv
from collections.abc import Mapping
from pathlib import Path
from typing import Any


class CampaignError(ValueError):
    """Raised when a corrected-campaign manifest or metric row is incomplete."""


_SHA256 = re.compile(r"^[0-9a-fA-F]{64}$")
_DIMENSION = re.compile(r"^[1-9][0-9]*x[1-9][0-9]*$")
_SAFE_ID = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]*$")
_SYNTHETIC = re.compile(r"^(?:synthetic(?:_|\.)|source(?:_|\.)|supersampled_aa)", re.I)
_REQUIRED_TEMPORAL_METRICS = frozenset({
    "static_flicker",
    "edge_variance",
    "motion_compensated_error",
    "ghost_duration_frames",
    "reset_recovery_frames",
})
_CAPTURE_ENV_PREFIXES = (
    "TFORGE_QUALITY_",
    "TFORGE_FSR4_",
    "TFORGE_UPSCALE_",
    "TFORGE_JITTER_",
    "TFORGE_REVIEW_",
    "TFORGE_BENCHMARK_",
)
_CAPTURE_ENV_KEYS = frozenset({
    "TFORGE_ALLOW_SPATIAL_TEMPORAL_CONTROL",
    "TFORGE_DISABLE_HW_DECODE",
})


def _mapping(value: Any, name: str) -> Mapping[str, Any]:
    if not isinstance(value, Mapping):
        raise CampaignError(f"{name} must be an object")
    return value


def _string(value: Any, name: str) -> str:
    if not isinstance(value, str) or not value:
        raise CampaignError(f"{name} must be a non-empty string")
    return value


def _dimension(value: Any, name: str) -> None:
    text = _string(value, name)
    if not _DIMENSION.fullmatch(text):
        raise CampaignError(f"{name} must be WIDTHxHEIGHT")


def _hash(value: Any, name: str) -> None:
    text = _string(value, name)
    if not _SHA256.fullmatch(text):
        raise CampaignError(f"{name} must be a SHA-256 digest")


def _optional_hash(value: Any, name: str) -> None:
    """Validate an optional digest without turning unavailable provenance into a fake value."""
    if value is not None:
        _hash(value, name)


def _validate_environment(value: Any, name: str) -> None:
    """Restrict declared settings to string-valued capture controls."""
    if value is None:
        return
    environment = _mapping(value, name)
    for key, setting in environment.items():
        if not isinstance(key, str) or not (
            key in _CAPTURE_ENV_KEYS or key.startswith(_CAPTURE_ENV_PREFIXES)
        ):
            raise CampaignError(f"{name} contains unsupported capture key: {key!r}")
        if not isinstance(setting, str):
            raise CampaignError(f"{name}.{key} must be a string")


def validate_metric_row(row: Any, required_metrics: list[str]) -> None:
    """Require finite paired metrics, including temporal failure signals."""
    values = _mapping(row, "metric row")
    for metric in required_metrics:
        value = values.get(metric)
        if not isinstance(value, (int, float)) or isinstance(value, bool):
            raise CampaignError(f"metric row is missing numeric {metric}")
        if not math.isfinite(float(value)):
            raise CampaignError(f"metric {metric} must be finite")


def validate_metrics_csv(
    path: Path, required_classes: list[str], required_metrics: list[str]
) -> None:
    """Validate one paired-metric table has exactly the declared class coverage."""
    if not path.is_file():
        raise CampaignError(f"metrics file does not exist: {path}")
    seen: set[str] = set()
    try:
        with path.open("r", newline="", encoding="utf-8") as stream:
            rows = list(csv.DictReader(stream))
    except OSError as error:
        raise CampaignError(f"cannot read metrics file: {path}") from error
    for row in rows:
        scene_class = row.get("class")
        if not isinstance(scene_class, str) or not scene_class:
            raise CampaignError("metric row is missing class")
        if scene_class in seen:
            raise CampaignError(f"duplicate metric class: {scene_class}")
        seen.add(scene_class)
        parsed: dict[str, float] = {}
        for metric in required_metrics:
            raw = row.get(metric)
            try:
                parsed[metric] = float(raw) if raw is not None else float("nan")
            except (TypeError, ValueError):
                parsed[metric] = float("nan")
        validate_metric_row(parsed, required_metrics)
    missing = set(required_classes) - seen
    unexpected = seen - set(required_classes)
    if missing or unexpected:
        raise CampaignError(
            f"metric class coverage mismatch; missing={sorted(missing)}, "
            f"unexpected={sorted(unexpected)}"
        )


def _validate_review_asset(asset: Any) -> None:
    """Require a real-world, dimensioned review image with source identity."""
    item = _mapping(asset, "review asset")
    scene = _string(item.get("scene"), "review asset.scene")
    if _SYNTHETIC.match(scene):
        raise CampaignError("synthetic review assets are not human-review inputs")
    _string(item.get("path"), "review asset.path")
    if isinstance(item.get("frame"), bool) or not isinstance(item.get("frame"), int):
        raise CampaignError("review asset.frame must be an integer")
    width = item.get("width")
    height = item.get("height")
    if isinstance(width, bool) or not isinstance(width, int) or width <= 0:
        raise CampaignError("review asset.width must be positive")
    if isinstance(height, bool) or not isinstance(height, int) or height <= 0:
        raise CampaignError("review asset.height must be positive")


def validate_campaign(campaign: Any) -> None:
    """Validate the M6 candidate, corpus, metric, and evidence contract."""
    root = _mapping(campaign, "campaign")
    if root.get("schemaVersion") != 2:
        raise CampaignError("campaign.schemaVersion must be 2")
    _string(root.get("campaignId"), "campaign.campaignId")
    evidence_mode = root.get("evidenceMode", "visual_and_metrics")
    if evidence_mode not in {"visual_and_metrics", "metrics_only"}:
        raise CampaignError(
            "campaign.evidenceMode must be visual_and_metrics or metrics_only"
        )
    _validate_environment(root.get("environment"), "campaign.environment")
    baseline = _string(root.get("baselineCandidateId"), "campaign.baselineCandidateId")
    corpus = _mapping(root.get("corpus"), "campaign.corpus")
    _string(corpus.get("manifestPath"), "campaign.corpus.manifestPath")
    selection = corpus.get("selection")
    if not isinstance(selection, list) or not selection or not all(
        isinstance(item, str) and item for item in selection
    ):
        raise CampaignError("campaign.corpus.selection must be a non-empty string list")
    for clip_id in selection:
        if _SYNTHETIC.match(clip_id):
            raise CampaignError(
                f"campaign.corpus.selection contains synthetic clip: {clip_id}"
            )
    classes = root.get("classes")
    if not isinstance(classes, list) or not classes or not all(
        isinstance(item, str) and item for item in classes
    ):
        raise CampaignError("campaign.classes must be a non-empty string list")
    class_selections = root.get("classSelections")
    if class_selections is not None:
        if not isinstance(class_selections, Mapping):
            raise CampaignError("campaign.classSelections must be an object")
        selected_scenes = set(selection)
        if set(class_selections) != selected_scenes:
            raise CampaignError(
                "campaign.classSelections must name every selected scene exactly once"
            )
        declared_classes = set(classes)
        for scene, scene_classes in class_selections.items():
            if not isinstance(scene_classes, list) or any(
                not isinstance(item, str) or not item for item in scene_classes
            ):
                raise CampaignError(
                    f"campaign.classSelections[{scene}] must be a string list (possibly empty)"
                )
            if len(scene_classes) != len(set(scene_classes)):
                raise CampaignError(f"campaign.classSelections[{scene}] contains duplicates")
            unknown = set(scene_classes) - declared_classes
            if unknown:
                raise CampaignError(
                    f"campaign.classSelections[{scene}] names undeclared classes: {sorted(unknown)}"
                )
    temporal = root.get("temporalEvidence")
    if temporal is not None:
        temporal_item = _mapping(temporal, "campaign.temporalEvidence")
        if not isinstance(temporal_item.get("complete"), bool):
            raise CampaignError("campaign.temporalEvidence.complete must be boolean")
        status = _string(temporal_item.get("status"), "campaign.temporalEvidence.status")
        if status not in {"pending", "complete"}:
            raise CampaignError("campaign.temporalEvidence.status must be pending or complete")
        rows = temporal_item.get("rows")
        if not isinstance(rows, list):
            raise CampaignError("campaign.temporalEvidence.rows must be a list")
        if not temporal_item["complete"] and rows:
            raise CampaignError("pending temporal evidence cannot contain temporal rows")
    required = root.get("requiredMetrics")
    if not isinstance(required, list) or not required or not all(
        isinstance(item, str) and item for item in required
    ):
        raise CampaignError("campaign.requiredMetrics must be a non-empty string list")
    missing_temporal = _REQUIRED_TEMPORAL_METRICS - set(required)
    if missing_temporal:
        raise CampaignError(
            "campaign.requiredMetrics is missing temporal metrics: "
            f"{sorted(missing_temporal)}"
        )
    candidates = root.get("candidates")
    if not isinstance(candidates, list) or not candidates:
        raise CampaignError("campaign.candidates must be non-empty")
    ids: set[str] = set()
    for candidate in candidates:
        item = _mapping(candidate, "candidate")
        candidate_id = _string(item.get("id"), "candidate.id")
        if not _SAFE_ID.fullmatch(candidate_id):
            raise CampaignError(f"candidate.id contains unsafe path characters: {candidate_id}")
        if candidate_id in ids:
            raise CampaignError(f"duplicate candidate id: {candidate_id}")
        ids.add(candidate_id)
        _string(item.get("configPath"), f"candidate {candidate_id}.configPath")
        _hash(item.get("binarySha256"), f"candidate {candidate_id}.binarySha256")
        _optional_hash(item.get("configSha256"), f"candidate {candidate_id}.configSha256")
        _validate_environment(
            item.get("environment"), f"candidate {candidate_id}.environment"
        )
        if item.get("gitCommit") is not None:
            commit = _string(item.get("gitCommit"), f"candidate {candidate_id}.gitCommit")
            if not re.fullmatch(r"[0-9a-fA-F]{7,64}", commit):
                raise CampaignError(f"candidate {candidate_id}.gitCommit is not a commit id")
        dimensions = _mapping(item.get("dimensions"), f"candidate {candidate_id}.dimensions")
        _dimension(dimensions.get("source"), f"candidate {candidate_id}.dimensions.source")
        _dimension(dimensions.get("output"), f"candidate {candidate_id}.dimensions.output")
        assets = item.get("reviewAssets")
        if evidence_mode == "metrics_only":
            if assets is not None and not isinstance(assets, list):
                raise CampaignError(
                    f"candidate {candidate_id}.reviewAssets must be a list when present"
                )
        else:
            if not isinstance(assets, list) or not assets:
                raise CampaignError(f"candidate {candidate_id} needs reviewAssets")
            for asset in assets:
                _validate_review_asset(asset)
    if baseline not in ids:
        raise CampaignError("baselineCandidateId must identify a candidate")


def runner_plans(campaign: Any) -> list[dict[str, Any]]:
    """Translate each validated candidate into an isolated legacy-run plan.

    The existing corpus runner accepts one source selector and one viewport per
    invocation. Returning one plan per candidate preserves those dimensions
    instead of collapsing unlike candidates into a misleading shared run.
    """
    validate_campaign(campaign)
    corpus = campaign["corpus"]
    # run_quality.sh evaluates this selector with Bash [[ value =~ regex ]],
    # which accepts POSIX ERE grouping but not PCRE's non-capturing group.
    clip_regex = "^(" + "|".join(
        re.escape(item) for item in corpus["selection"]
    ) + ")$"
    plans: list[dict[str, Any]] = []
    for candidate in campaign["candidates"]:
        dimensions = candidate["dimensions"]
        plans.append({
            "name": f"{campaign['campaignId']}-{candidate['id']}",
            "environment": dict(campaign.get("environment", {})),
            "dimensions": dimensions["source"],
            "outputDimensions": dimensions["output"],
            "corpusManifest": corpus["manifestPath"],
            "clipRegex": clip_regex,
            "experiments": [{
                "id": candidate["id"],
                "config": candidate["configPath"],
                "environment": dict(candidate.get("environment", {})),
            }],
            "jitter": campaign.get("jitter", {"mode": "current", "controlledStrength": 1.0}),
            "classSelections": campaign.get("classSelections"),
            "qualityClassAnnotationsPath": campaign.get("qualityClassAnnotationsPath"),
            "evidenceMode": campaign.get("evidenceMode", "visual_and_metrics"),
        })
    return plans
