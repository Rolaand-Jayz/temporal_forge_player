#!/usr/bin/env python3
"""Report missing evidence for an M6 schemaVersion 2 quality campaign.

This command is deliberately capture-free.  It reads the existing legacy
spatial manifest and any explicitly supplied evidence sidecars or metric CSVs,
then reports which campaign and matrix keys are still ungrounded.  It never
turns a scene label into a quality class and never writes a campaign manifest.

Upstream: the saved M6 spatial manifest plus optional existing result/sidecar
files.  Downstream: a machine-readable report used to decide whether a
schemaVersion 2 campaign can be authored without inventing provenance.
"""

from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from collections.abc import Iterable, Mapping, Sequence
from pathlib import Path
from typing import Any


_EXACT_CLIP_REGEX = re.compile(r"^\^\(([^()]+(?:\|[^()]+)*)\)\$$")
_TEMPORAL_METRICS = (
    "static_flicker",
    "edge_variance",
    "motion_compensated_error",
    "ghost_duration_frames",
    "reset_recovery_frames",
)
_SPATIAL_METRICS = ("psnr_db", "ssim", "edge_ssim")


class GapReportError(ValueError):
    """Raised when an evidence input cannot be read as a supported artifact."""


def _load_json(path: Path) -> Mapping[str, Any]:
    """Read one JSON object and reject arrays or scalars as ambiguous evidence."""

    try:
        with path.open("r", encoding="utf-8") as stream:
            value = json.load(stream)
    except (OSError, json.JSONDecodeError) as error:
        raise GapReportError(f"cannot read JSON evidence: {path}: {error}") from error
    if not isinstance(value, Mapping):
        raise GapReportError(f"JSON evidence must be an object: {path}")
    return value


def _string(value: Any) -> str | None:
    """Return a non-empty string without coercing evidence into a label."""

    return value if isinstance(value, str) and value else None


def _exact_clip_selection(value: Any) -> list[str]:
    """Extract scenes only from the legacy runner's exact anchored alternation."""

    if not isinstance(value, str):
        return []
    match = _EXACT_CLIP_REGEX.fullmatch(value)
    if not match:
        return []
    values = match.group(1).split("|")
    if not values or any(not item or re.search(r"[^A-Za-z0-9_.-]", item) for item in values):
        return []
    return values


def _read_temporal_labels(paths: Sequence[Path]) -> tuple[list[str], set[str]]:
    """Read CSV class labels and metric fields without interpreting their meaning."""

    labels: set[str] = set()
    fields: set[str] = set()
    for path in paths:
        try:
            with path.open("r", newline="", encoding="utf-8") as stream:
                reader = csv.DictReader(stream)
                if reader.fieldnames:
                    fields.update(name for name in reader.fieldnames if name)
                for row in reader:
                    label = _string(row.get("class"))
                    if label:
                        labels.add(label)
        except OSError as error:
            raise GapReportError(f"cannot read temporal metrics CSV: {path}: {error}") from error
    return sorted(labels), fields


def _result_observations(paths: Sequence[Path]) -> dict[str, Any]:
    """Collect explicit identity fields from saved spatial result JSON files."""

    observations = {
        "candidateIds": set(),
        "scenes": set(),
        "frames": set(),
        "inputResolutions": set(),
        "outputResolutions": set(),
        "binarySha256": set(),
        "configPaths": set(),
        "candidateBinarySha256": {},
        "candidateConfigPaths": {},
        "candidateScenePairs": set(),
    }
    for path in paths:
        value = _load_json(path)
        for key, target in (
            ("candidateId", "candidateIds"),
            ("binarySha256", "binarySha256"),
            ("configSource", "configPaths"),
        ):
            item = _string(value.get(key))
            if item:
                observations[target].add(item)
        candidate_id = _string(value.get("candidateId"))
        binary_sha256 = _string(value.get("binarySha256"))
        config_path = _string(value.get("configSource"))
        if candidate_id and binary_sha256:
            observations["candidateBinarySha256"][candidate_id] = binary_sha256
        if candidate_id and config_path:
            observations["candidateConfigPaths"][candidate_id] = config_path
        frame = value.get("frame")
        if isinstance(frame, int) and not isinstance(frame, bool):
            observations["frames"].add(str(frame))
        for clip in value.get("metrics", {}).get("clips", []):
            if not isinstance(clip, Mapping):
                continue
            scene = _string(clip.get("clip_id"))
            if scene:
                observations["scenes"].add(scene)
                if candidate_id:
                    observations["candidateScenePairs"].add((candidate_id, scene))
            clip_frame = clip.get("frame")
            if isinstance(clip_frame, str) and clip_frame:
                observations["frames"].add(clip_frame)
            width = clip.get("width")
            height = clip.get("height")
            if isinstance(width, str) and isinstance(height, str):
                observations["inputResolutions"].add(f"{width}x{height}")
            output_width = clip.get("output_width")
            output_height = clip.get("output_height")
            if isinstance(output_width, str) and isinstance(output_height, str):
                observations["outputResolutions"].add(f"{output_width}x{output_height}")
    return observations


def _sidecar_identity(path: Path) -> bool:
    """Return whether a sidecar explicitly carries campaign row identity."""

    value = _load_json(path)
    return all(value.get(key) is not None for key in ("candidateId", "scene", "frame"))


def _add_gap(gaps: list[dict[str, str]], key: str, reason: str) -> None:
    """Append one stable missing-key explanation unless it is already present."""

    if not any(item["key"] == key for item in gaps):
        gaps.append({"key": key, "reason": reason})


def build_gap_report(
    legacy_manifest: Path,
    *,
    spatial_results: Iterable[Path] = (),
    temporal_metrics: Iterable[Path] = (),
    motion_sidecars: Iterable[Path] = (),
    static_masks: Iterable[Path] = (),
    event_sidecars: Iterable[Path] = (),
) -> dict[str, Any]:
    """Build a truthful schema-v2 readiness report without launching captures."""

    manifest = _load_json(legacy_manifest)
    spatial_results = list(spatial_results)
    temporal_metrics = list(temporal_metrics)
    motion_sidecars = list(motion_sidecars)
    static_masks = list(static_masks)
    event_sidecars = list(event_sidecars)

    experiments = manifest.get("experiments")
    if not isinstance(experiments, list):
        experiments = []
    candidate_ids = [
        item.get("id")
        for item in experiments
        if isinstance(item, Mapping) and _string(item.get("id"))
    ]
    candidate_ids = [str(item) for item in candidate_ids]
    candidate_configs = {
        str(item["id"]): item["config"]
        for item in experiments
        if isinstance(item, Mapping)
        and _string(item.get("id"))
        and _string(item.get("config"))
    }
    scenes = _exact_clip_selection(manifest.get("clipRegex"))
    input_resolution = _string(manifest.get("dimensions"))
    output_resolution = _string(manifest.get("outputDimensions"))
    frame = manifest.get("frame") if isinstance(manifest.get("frame"), int) else None
    temporal_labels, temporal_fields = _read_temporal_labels(temporal_metrics)
    result_observations = _result_observations(spatial_results)

    gaps: list[dict[str, str]] = []
    for key, reason in (
        ("campaign.schemaVersion", "the saved source is a legacy runner manifest, not schemaVersion 2"),
        ("campaign.campaignId", "the saved source has a run name but no campaign identity field"),
        ("campaign.baselineCandidateId", "no baseline candidate is declared in the saved source"),
        ("campaign.corpus.manifestPath", "the saved source has no corpus manifest path"),
        ("campaign.classes", "no quality-class taxonomy is grounded by the selected spatial evidence"),
        ("campaign.requiredMetrics", "the saved source does not declare the complete metric contract"),
    ):
        _add_gap(gaps, key, reason)

    for candidate_id in candidate_ids:
        if candidate_id not in result_observations["candidateBinarySha256"]:
            _add_gap(
                gaps,
                f"candidates.{candidate_id}.binarySha256",
                "the legacy manifest names the candidate but does not carry a binary digest",
            )
        _add_gap(
            gaps,
            f"candidates.{candidate_id}.reviewAssets",
            "the legacy manifest has no structured review asset records",
        )

    spatial_has_rows = bool(spatial_results)
    if not spatial_has_rows:
        for field in ("candidateId", "scene", "qualityClass", "inputResolution", "outputResolution", "frame"):
            _add_gap(
                gaps,
                f"matrix.spatial.rows[*].{field}",
                "no schema-v2 spatial row evidence was supplied",
            )
    else:
        _add_gap(
            gaps,
            "matrix.spatial.rows[*].qualityClass",
            "saved spatial results have scene IDs but no grounded quality-class field",
        )
    for field in ("gitCommit", "binarySha256", "configPath"):
        _add_gap(
            gaps,
            f"matrix.spatial.rows[*].provenance.{field}",
            "spatial result evidence is not a complete schema-v2 row provenance record",
        )
    for metric in _SPATIAL_METRICS:
        _add_gap(
            gaps,
            f"matrix.spatial.rows[*].metrics.{metric}",
            "the legacy result format uses benchmark-specific metric names rather than the matrix contract",
        )

    for field in ("candidateId", "scene", "qualityClass", "inputResolution", "outputResolution", "frame"):
        _add_gap(
            gaps,
            f"matrix.temporal.rows[*].{field}",
            "temporal CSV rows contain a label and dimensions but no complete campaign row identity",
        )
    for field in ("gitCommit", "binarySha256", "configPath"):
        _add_gap(
            gaps,
            f"matrix.temporal.rows[*].provenance.{field}",
            "temporal metric CSVs do not carry candidate-linked provenance",
        )

    if not motion_sidecars:
        _add_gap(
            gaps,
            "matrix.temporal.rows[*].sidecars.motion",
            "no motion sidecar was supplied for the temporal matrix rows",
        )
    if not static_masks:
        _add_gap(
            gaps,
            "matrix.temporal.rows[*].sidecars.staticMask",
            "no static-mask sidecar was supplied for the temporal matrix rows",
        )
    for label, paths in (("motion", motion_sidecars), ("staticMask", static_masks)):
        for path in paths:
            if not _sidecar_identity(path):
                _add_gap(
                    gaps,
                    f"matrix.temporal.rows[*].sidecars.candidateId",
                    f"{label} sidecars are present but do not carry candidate/scene/frame attribution",
                )
                break

    report: dict[str, Any] = {
        "reportSchemaVersion": 1,
        "sourceManifest": str(legacy_manifest),
        "readyForSchemaVersion2": not gaps,
        "grounded": {
            "candidateIds": candidate_ids,
            "candidateConfigs": candidate_configs,
            "scenes": scenes,
            "inputResolution": input_resolution,
            "outputResolution": output_resolution,
            "frame": frame,
            "quality": _string(manifest.get("quality")),
            "qualityClasses": [],
        },
        "observed": {
            "spatialResultFiles": [str(path) for path in spatial_results],
            "spatialResultCandidates": sorted(result_observations["candidateIds"]),
            "spatialResultScenes": sorted(result_observations["scenes"]),
            "temporalMetricFiles": [str(path) for path in temporal_metrics],
            "temporalLabels": temporal_labels,
            "temporalMetricFields": sorted(temporal_fields),
            "motionSidecars": [str(path) for path in motion_sidecars],
            "staticMaskSidecars": [str(path) for path in static_masks],
            "eventSidecars": [str(path) for path in event_sidecars],
            "captureInvocations": 0,
        },
        "coverage": {
            "candidateCount": len(candidate_ids),
            "sceneCount": len(scenes),
            "groundedQualityClassCount": 0,
            "groundedSpatialCandidateScenePairs": len(result_observations["candidateScenePairs"]),
            "requiredSpatialCandidateScenePairs": len(candidate_ids) * len(scenes),
            "requiredMatrixKeys": [],
        },
        "missingKeys": [item["key"] for item in gaps],
        "gaps": gaps,
    }
    return report


def parse_args() -> argparse.Namespace:
    """Define the explicit, read-only evidence paths accepted by the reporter."""

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("legacy_manifest", type=Path)
    parser.add_argument("--spatial-result", action="append", type=Path, default=[])
    parser.add_argument("--temporal-metrics", action="append", type=Path, default=[])
    parser.add_argument("--motion-sidecar", action="append", type=Path, default=[])
    parser.add_argument("--static-mask", action="append", type=Path, default=[])
    parser.add_argument("--event-sidecar", action="append", type=Path, default=[])
    return parser.parse_args()


def main() -> int:
    """Print the report as JSON and never launch a capture process."""

    args = parse_args()
    report = build_gap_report(
        args.legacy_manifest,
        spatial_results=args.spatial_result,
        temporal_metrics=args.temporal_metrics,
        motion_sidecars=args.motion_sidecar,
        static_masks=args.static_mask,
        event_sidecars=args.event_sidecar,
    )
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (GapReportError, OSError, json.JSONDecodeError, KeyError, TypeError) as error:
        print(f"quality matrix gap report error: {error}", file=sys.stderr)
        raise SystemExit(2)
