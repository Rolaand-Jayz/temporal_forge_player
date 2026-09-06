#!/usr/bin/env python3
"""Assemble the authoritative M6 schema-v2 matrix from accepted event rows.

The prior matrix is used only to prove strict-key replacement.  Spatial metric
values are taken from the supplied spatial matrix, and temporal event metrics
are taken from the matching event CSV; no metric is copied across keys.
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import math
import sys
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from benchmarks.quality_sweeps.quality_campaign_contract import validate_campaign  # noqa: E402
from benchmarks.quality_sweeps.temporal_matrix import _read_temporal_csv  # noqa: E402


EVENT_METRICS = (
    "static_flicker",
    "edge_variance",
    "motion_compensated_error",
    "ghost_duration_frames",
    "reset_recovery_frames",
)
EVENT_FIELDS = (
    "authoritativeEventIndex",
    "eventFrameIndex",
    "eventTransitionIndex",
    "resetIndex",
    "ghostEventIndex",
    "ghostThreshold",
    "resetThreshold",
)


class AssemblyError(ValueError):
    """Raised when accepted evidence cannot be joined without ambiguity."""


def _load(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise AssemblyError(f"cannot read JSON {path}: {error}") from error


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
    except OSError as error:
        raise AssemblyError(f"cannot hash evidence {path}: {error}") from error
    return digest.hexdigest()


def _finite(value: Any, name: str) -> None:
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(float(value)):
        raise AssemblyError(f"{name} must be finite numeric evidence")


def _numeric_metrics(row: dict[str, Any]) -> dict[str, Any]:
    """Normalize numeric metrics while preserving path/provenance metadata."""

    normalized = copy.deepcopy(row)
    metrics = normalized.get("metrics")
    if not isinstance(metrics, dict):
        raise AssemblyError("matrix row metrics must be an object")
    for name, value in metrics.items():
        if isinstance(value, str):
            try:
                metrics[name] = float(value)
            except ValueError:
                # Review assets are carried beside numeric measurements in the
                # same object. Their paths are identity evidence, not metrics,
                # and must remain lossless through the combined matrix join.
                continue
    return normalized


def _config_matches(captured: str, expected: str) -> str | None:
    expected_path = Path(expected).as_posix()
    if captured == expected_path:
        return "campaign_config_path"
    if captured == expected_path.removeprefix("benchmarks/quality_sweeps/"):
        return "campaign_config_path_without_prefix"
    return None


def _expected_keys(campaign: dict[str, Any]) -> set[tuple[str, str, str]]:
    return {
        (candidate["id"], scene, quality_class)
        for candidate in campaign["candidates"]
        for scene in campaign["corpus"]["selection"]
        for quality_class in campaign["classSelections"].get(scene, [])
    }


def _row_keys(rows: Any, label: str) -> set[tuple[str, str, str]]:
    if not isinstance(rows, list):
        raise AssemblyError(f"{label} rows must be a list")
    result: set[tuple[str, str, str]] = set()
    for row in rows:
        if not isinstance(row, dict):
            raise AssemblyError(f"{label} contains a non-object row")
        key = (row.get("candidateId"), row.get("scene"), row.get("qualityClass"))
        if key in result:
            raise AssemblyError(f"duplicate {label} strict key: {key}")
        result.add(key)
    return result


def _trace(path: Path, raw: dict[str, Any], quality_class: str) -> dict[str, Any]:
    trace = _load(path)
    if not isinstance(trace, dict) or trace.get("schema") != "temporal_forge.event_trace.v1":
        raise AssemblyError(f"event trace has wrong schema: {path}")
    identity = trace.get("identity")
    if not isinstance(identity, dict):
        raise AssemblyError(f"event trace has no identity: {path}")
    expected_identity = {
        "candidateId": raw["candidateId"],
        "scene": raw["scene"],
        "configId": raw["configId"],
        "startFrame": raw["startFrame"],
        "endFrame": raw["endFrame"],
        "outputWidth": raw["width"],
        "outputHeight": raw["height"],
    }
    for field, expected in expected_identity.items():
        if identity.get(field) != expected:
            raise AssemblyError(f"event trace identity mismatch for {field}: {path}")
    if not identity.get("sourcePath") or not identity.get("referencePath"):
        raise AssemblyError(f"event trace source/reference is empty: {path}")
    if trace.get("capture", {}).get("frames") != raw["frames"]:
        raise AssemblyError(f"event trace frame count mismatch: {path}")
    frames = trace.get("frames")
    if not isinstance(frames, list) or len(frames) != raw["frames"]:
        raise AssemblyError(f"event trace frame records are incomplete: {path}")
    if trace.get("authoritativeEvent", {}).get("detectorSceneCut") is not True:
        raise AssemblyError(f"event trace lacks authoritative detector cut: {path}")
    for field in EVENT_FIELDS:
        if field not in trace:
            raise AssemblyError(f"event trace lacks {field}: {path}")
    for field in ("authoritativeEventIndex", "eventFrameIndex", "eventTransitionIndex", "resetIndex", "ghostEventIndex"):
        value = trace[field]
        if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value < raw["frames"]:
            raise AssemblyError(f"event trace index {field} is out of range: {path}")
    for field in ("ghostThreshold", "resetThreshold"):
        _finite(trace[field], f"{path}.{field}")
        if trace[field] < 0:
            raise AssemblyError(f"event trace threshold is negative: {path}")
    if quality_class not in {"faces-hair-skin", "fine-fabric-texture", "high-contrast-architecture", "low-light-shadow-detail"}:
        raise AssemblyError(f"unsupported quality class: {quality_class}")
    return trace


def _sidecar(path: Path, *, schema: str, raw: dict[str, Any], kind: str) -> dict[str, Any]:
    value = _load(path)
    if not isinstance(value, dict) or value.get("schema") != schema:
        raise AssemblyError(f"{kind} sidecar has wrong schema: {path}")
    if kind == "causalMotion":
        if value.get("sourceWidth") != 426 or value.get("sourceHeight") != 240:
            raise AssemblyError(f"causal motion source dimensions drift: {path}")
        if value.get("targetWidth") != raw["width"] or value.get("targetHeight") != raw["height"]:
            raise AssemblyError(f"causal motion target dimensions drift: {path}")
        if not isinstance(value.get("frames"), list) or len(value["frames"]) != raw["frames"]:
            raise AssemblyError(f"causal motion frame count drift: {path}")
    else:
        if value.get("width") != raw["width"] or value.get("height") != raw["height"]:
            raise AssemblyError(f"static mask dimensions drift: {path}")
        if value.get("scene") != raw["scene"]:
            raise AssemblyError(f"static mask scene drift: {path}")
        frame_range = value.get("frameRange")
        if frame_range is not None and frame_range != {"start": raw["startFrame"], "end": raw["endFrame"]}:
            raise AssemblyError(f"static mask frame range drift: {path}")
    return value


def _parse_event_spec(spec: str) -> tuple[Path, Path, Path, Path, str]:
    parts = spec.split("|")
    if len(parts) != 5 or any(not part for part in parts):
        raise AssemblyError("--event must be CSV|EVENT_TRACE|CAUSAL_MOTION|STATIC_MASK|QUALITY_CLASS")
    return Path(parts[0]), Path(parts[1]), Path(parts[2]), Path(parts[3]), parts[4]


def assemble(campaign: dict[str, Any], spatial: dict[str, Any], prior: dict[str, Any], specs: list[str]) -> dict[str, Any]:
    validate_campaign(campaign)
    if spatial.get("schemaVersion") != 2 or spatial.get("matrixType") != "spatial":
        raise AssemblyError("spatial input must be a schemaVersion 2 spatial matrix")
    if prior.get("campaignId") != campaign["campaignId"]:
        raise AssemblyError("prior matrix campaign identity does not match campaign")
    expected = _expected_keys(campaign)
    spatial_rows = spatial.get("rows")
    spatial_by_key = {(row["candidateId"], row["scene"], row["qualityClass"]): row for row in spatial_rows}
    if _row_keys(spatial_rows, "spatial") != expected:
        raise AssemblyError("spatial rows do not exactly cover campaign keys")
    prior_keys = _row_keys(prior.get("temporal"), "prior temporal")
    if prior_keys != expected:
        raise AssemblyError("prior temporal rows do not exactly cover campaign keys")
    if len(specs) != len(expected):
        raise AssemblyError(f"expected {len(expected)} event specs, received {len(specs)}")

    candidates = {candidate["id"]: candidate for candidate in campaign["candidates"]}
    event_rows: dict[tuple[str, str, str], dict[str, Any]] = {}
    for spec in specs:
        csv_path, trace_path, motion_path, mask_path, declared_class = _parse_event_spec(spec)
        raw = _read_temporal_csv(csv_path)
        raw_class = raw["class"]
        if raw_class != declared_class and not (declared_class == "low-light-shadow-detail" and raw_class == "low_light_shadow_detail"):
            raise AssemblyError(f"event class mismatch: {csv_path}")
        key = (raw["candidateId"], raw["scene"], declared_class)
        if key not in expected:
            raise AssemblyError(f"event row has unexpected strict key: {key}")
        if key in event_rows:
            raise AssemblyError(f"duplicate event strict key: {key}")
        candidate = candidates[key[0]]
        config_resolution = _config_matches(raw["configId"], candidate["configPath"])
        if config_resolution is None:
            raise AssemblyError(f"event config identity mismatch: {csv_path}")
        if raw["frames"] < 5 or raw["frames"] != raw["endFrame"] - raw["startFrame"] + 1:
            raise AssemblyError(f"event frame range is invalid: {csv_path}")
        expected_width, expected_height = (int(value) for value in campaign["dimensions"]["output"].split("x"))
        if (raw["width"], raw["height"]) != (expected_width, expected_height):
            raise AssemblyError(f"event output dimensions mismatch: {csv_path}")
        for field in EVENT_METRICS:
            _finite(raw[field], f"{csv_path}:{field}")
        trace = _trace(trace_path, raw, declared_class)
        motion = _sidecar(motion_path, schema="temporal_forge.codec_motion.v1", raw=raw, kind="causalMotion")
        mask = _sidecar(mask_path, schema="temporal_forge.static_mask.v1", raw=raw, kind="staticMask")
        spatial_row = _numeric_metrics(spatial_by_key[key])
        row = copy.deepcopy(spatial_row)
        row["sequence"] = {
            "startFrame": raw["startFrame"],
            "endFrame": raw["endFrame"],
            "frames": raw["frames"],
            "width": raw["width"],
            "height": raw["height"],
        }
        row["metrics"].update({field: raw[field] for field in EVENT_METRICS})
        row["event"] = {
            **{field: trace[field] for field in EVENT_FIELDS},
            "resetCause": trace.get("resetCause"),
            "ghostCause": trace.get("ghostCause"),
            "authoritativeCause": trace.get("authoritativeCause"),
            "sourcePath": trace["identity"]["sourcePath"],
            "referencePath": trace["identity"]["referencePath"],
        }
        row["eventArtifacts"] = {
            "causalMotion": {"path": str(motion_path), "sha256": _sha256(motion_path), "schema": motion["schema"]},
            "staticMask": {
                "path": str(mask_path),
                "sha256": _sha256(mask_path),
                "schema": mask["schema"],
                "frameRange": mask.get("frameRange"),
            },
        }
        row["metricSource"] = {
            "spatial": spatial_row["metricSource"],
            "temporalCsv": str(csv_path),
            "eventTrace": str(trace_path),
            "causalMotion": str(motion_path),
            "staticMask": str(mask_path),
            "sha256": {
                "temporalCsv": _sha256(csv_path),
                "eventTrace": _sha256(trace_path),
                "causalMotion": _sha256(motion_path),
                "staticMask": _sha256(mask_path),
            },
            "capturedCandidateId": raw["candidateId"],
            "capturedConfigId": raw["configId"],
            "configIdentityResolution": config_resolution,
            **({"capturedClass": raw_class} if raw_class != declared_class else {}),
        }
        event_rows[key] = row

    if set(event_rows) != expected:
        raise AssemblyError(f"event rows do not exactly cover campaign keys: missing={sorted(expected - set(event_rows))}")
    ordered_keys = [(candidate["id"], scene, quality_class) for candidate in campaign["candidates"] for scene in campaign["corpus"]["selection"] for quality_class in campaign["classSelections"].get(scene, [])]
    result = {
        "schemaVersion": 2,
        "campaignId": campaign["campaignId"],
        "matrixType": "combined",
        "complete": True,
        "spatialSourcePath": str(spatial.get("sourcePath", "")),
        "temporalCsvCount": len(event_rows),
        # The strict verifier requires both matrix views to expose the full
        # metric contract. Keep the supplied spatial-only rows losslessly in
        # spatialEvidence; the normalized spatial view is the same exact-key
        # event join as temporal, as in the existing combined-matrix schema.
        "spatial": [event_rows[key] for key in ordered_keys],
        "spatialEvidence": [_numeric_metrics(spatial_by_key[key]) for key in ordered_keys],
        "temporal": [event_rows[key] for key in ordered_keys],
        "temporalEvidence": {
            "complete": True,
            "status": "complete",
            "scope": "authoritative_event_backed_rows",
            "strictMatrixInput": True,
            "rowCount": len(event_rows),
            "sequenceFrames": sorted({event_rows[key]["sequence"]["frames"] for key in ordered_keys}),
            "replacedPriorTemporalRows": len(event_rows),
            "eventMetrics": list(EVENT_METRICS),
        },
        "eventEvidence": {
            "scope": "authoritative_event_backed_rows",
            "strictMatrixInput": True,
            "promotesBaseRows": True,
            "rowCount": len(event_rows),
            "coverageKeys": [list(key) for key in ordered_keys],
            "note": "Event rows replace prior short temporal rows only at the same candidate/scene/qualityClass key; no separate duplicate event rows are retained.",
        },
    }
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("campaign", type=Path)
    parser.add_argument("spatial", type=Path)
    parser.add_argument("prior", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--event", action="append", required=True, help="CSV|EVENT_TRACE|CAUSAL_MOTION|STATIC_MASK|QUALITY_CLASS")
    args = parser.parse_args()
    if args.output.exists():
        parser.error(f"refusing to overwrite existing output: {args.output}")
    try:
        result = assemble(args.campaign and _load(args.campaign), _load(args.spatial), _load(args.prior), args.event)
        result["spatialSourcePath"] = str(args.spatial)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    except (OSError, AssemblyError, KeyError, TypeError) as error:
        parser.error(str(error))
    print(f"M6 final schema-v2 matrix assembled: {args.output} (spatial={len(result['spatial'])}, temporal={len(result['temporal'])}, event-backed={result['temporalEvidence']['rowCount']})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
