"""Join accepted event-spanning temporal evidence to an existing M6 matrix.

The ordinary M6 temporal matrix has exactly one row per candidate/scene/class
coverage key.  Event captures are deliberately kept in a separate evidence
section because they span a different frame window and must not replace the
retained eight-frame rows or create duplicate strict-matrix keys.
"""

from __future__ import annotations

import hashlib
import json
import math
from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import Any

from .quality_campaign_contract import validate_campaign
from .temporal_matrix import TemporalMatrixError, _read_temporal_csv


_EVENT_METRICS = (
    "static_flicker",
    "edge_variance",
    "motion_compensated_error",
    "ghost_duration_frames",
    "reset_recovery_frames",
)
_EVENT_BOUNDARY_METRICS = (
    "ghost_duration_frames",
    "reset_recovery_frames",
)
_REQUIRED_EVENT_TOP_FIELDS = (
    "authoritativeEventIndex",
    "eventFrameIndex",
    "eventTransitionIndex",
    "resetIndex",
    "ghostEventIndex",
    "ghostThreshold",
    "resetThreshold",
)


def _load_json(path: Path) -> Mapping[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise TemporalMatrixError(f"cannot read event evidence {path}: {error}") from error
    if not isinstance(value, Mapping):
        raise TemporalMatrixError(f"event evidence must be an object: {path}")
    return value


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
    except OSError as error:
        raise TemporalMatrixError(f"cannot hash evidence {path}: {error}") from error
    return digest.hexdigest()


def _finite(value: Any, name: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise TemporalMatrixError(f"{name} must be numeric")
    result = float(value)
    if not math.isfinite(result):
        raise TemporalMatrixError(f"{name} must be finite")
    return result


def _config_matches(captured: str, expected: str) -> bool:
    expected_path = Path(expected).as_posix()
    return captured in {
        expected_path,
        expected_path.removeprefix("benchmarks/quality_sweeps/"),
    }


def _expected_class(campaign: Mapping[str, Any], scene: str, quality_class: str) -> None:
    if scene not in campaign["corpus"]["selection"]:
        raise TemporalMatrixError(f"event evidence names an unselected scene: {scene}")
    if quality_class not in campaign["classes"]:
        raise TemporalMatrixError(f"event evidence names an undeclared class: {quality_class}")
    if quality_class not in campaign["classSelections"].get(scene, []):
        raise TemporalMatrixError(
            f"event evidence class is not selected for scene: {scene}/{quality_class}"
        )


def _check_sidecars(
    motion_path: Path | None,
    mask_path: Path | None,
    *,
    candidate_id: str,
    scene: str,
    start_frame: int,
    end_frame: int,
    width: int,
    height: int,
) -> None:
    if (motion_path is None) != (mask_path is None):
        raise TemporalMatrixError("motion and static-mask sidecars must be supplied together")
    if motion_path is None or mask_path is None:
        return
    motion = _load_json(motion_path)
    if motion.get("schema") != "temporal_forge.codec_motion.v1":
        raise TemporalMatrixError(f"motion sidecar has wrong schema: {motion_path}")
    if motion.get("sourceWidth") != 426 or motion.get("sourceHeight") != 240:
        raise TemporalMatrixError(f"motion sidecar source dimensions drift: {motion_path}")
    if motion.get("targetWidth") != width or motion.get("targetHeight") != height:
        raise TemporalMatrixError(f"motion sidecar output dimensions drift: {motion_path}")
    frames = motion.get("frames")
    if not isinstance(frames, list) or len(frames) != end_frame - start_frame + 1:
        raise TemporalMatrixError(f"motion sidecar frame count drift: {motion_path}")

    mask = _load_json(mask_path)
    if mask.get("schema") != "temporal_forge.static_mask.v1":
        raise TemporalMatrixError(f"static-mask sidecar has wrong schema: {mask_path}")
    if mask.get("width") != width or mask.get("height") != height:
        raise TemporalMatrixError(f"static-mask dimensions drift: {mask_path}")
    # The static region is a scene/class annotation and may be reused across
    # candidate captures. Candidate identity is therefore recorded by the row
    # and trace, not imposed on this shared mask sidecar.
    if mask.get("scene") != scene:
        raise TemporalMatrixError(f"static-mask scene identity drift: {mask_path}")
    frame_range = mask.get("frameRange")
    if not isinstance(frame_range, Mapping) or frame_range.get("start") != start_frame or frame_range.get("end") != end_frame:
        raise TemporalMatrixError(f"static-mask frame range drift: {mask_path}")


def _check_event_trace(
    trace_path: Path,
    row: Mapping[str, Any],
    *,
    expected_candidate: str,
    expected_class: str,
) -> Mapping[str, Any]:
    trace = _load_json(trace_path)
    if trace.get("schema") != "temporal_forge.event_trace.v1":
        raise TemporalMatrixError(f"event trace has wrong schema: {trace_path}")
    identity = trace.get("identity")
    if not isinstance(identity, Mapping):
        raise TemporalMatrixError(f"event trace has no identity: {trace_path}")
    for name, expected in (
        ("candidateId", expected_candidate),
        ("scene", row["scene"]),
        ("configId", row["configId"]),
        ("startFrame", row["startFrame"]),
        ("endFrame", row["endFrame"]),
        ("outputWidth", row["width"]),
        ("outputHeight", row["height"]),
    ):
        if identity.get(name) != expected:
            raise TemporalMatrixError(
                f"event trace identity mismatch for {name}: {trace_path}"
            )
    if identity.get("sourcePath") == "" or identity.get("referencePath") == "":
        raise TemporalMatrixError(f"event trace source/reference is empty: {trace_path}")
    capture = trace.get("capture")
    if not isinstance(capture, Mapping) or capture.get("frames") != row["frames"]:
        raise TemporalMatrixError(f"event trace capture length mismatch: {trace_path}")
    frames = trace.get("frames")
    if not isinstance(frames, list) or len(frames) != row["frames"]:
        raise TemporalMatrixError(f"event trace frame records are incomplete: {trace_path}")
    if any(not isinstance(item, Mapping) for item in frames):
        raise TemporalMatrixError(f"event trace contains a non-object frame: {trace_path}")
    for name in _REQUIRED_EVENT_TOP_FIELDS:
        if name not in trace:
            raise TemporalMatrixError(f"event trace lacks {name}: {trace_path}")
    event_index = trace["eventFrameIndex"]
    reset_index = trace["resetIndex"]
    ghost_index = trace["ghostEventIndex"]
    if any(
        isinstance(value, bool) or not isinstance(value, int) or value < 0 or value >= row["frames"]
        for value in (event_index, reset_index, ghost_index)
    ):
        raise TemporalMatrixError(f"event trace indices are out of range: {trace_path}")
    if _finite(trace["ghostThreshold"], f"{trace_path}.ghostThreshold") < 0 or _finite(trace["resetThreshold"], f"{trace_path}.resetThreshold") < 0:
        raise TemporalMatrixError(f"event trace thresholds must be non-negative: {trace_path}")
    authoritative = trace.get("authoritativeEvent")
    if not isinstance(authoritative, Mapping) or authoritative.get("detectorSceneCut") is not True:
        raise TemporalMatrixError(f"event trace has no authoritative detector cut: {trace_path}")
    if authoritative.get("eventIndex") != trace["authoritativeEventIndex"]:
        raise TemporalMatrixError(f"event trace authoritative index drift: {trace_path}")
    if expected_class != "low-light-shadow-detail":
        raise TemporalMatrixError(f"event evidence class must be low-light-shadow-detail: {trace_path}")
    return trace


def _validate_base_matrix(campaign: Mapping[str, Any], matrix: Mapping[str, Any]) -> None:
    if matrix.get("schemaVersion") != 2 or matrix.get("matrixType") != "combined":
        raise TemporalMatrixError("base matrix must be a schemaVersion 2 combined matrix")
    if matrix.get("campaignId") != campaign["campaignId"]:
        raise TemporalMatrixError("base matrix campaign identity does not match campaign")
    rows = matrix.get("temporal")
    if not isinstance(rows, list) or not rows:
        raise TemporalMatrixError("base matrix must contain temporal rows")
    expected_keys = {
        (candidate["id"], scene, quality_class)
        for candidate in campaign["candidates"]
        for scene in campaign["corpus"]["selection"]
        for quality_class in campaign["classSelections"].get(scene, [])
    }
    actual_keys: set[tuple[str, str, str]] = set()
    for row in rows:
        if not isinstance(row, Mapping):
            raise TemporalMatrixError("base matrix temporal rows must be objects")
        key = (row.get("candidateId"), row.get("scene"), row.get("qualityClass"))
        if key not in expected_keys:
            raise TemporalMatrixError(f"base matrix has unexpected temporal key: {key}")
        if key in actual_keys:
            raise TemporalMatrixError(f"base matrix has duplicate temporal key: {key}")
        actual_keys.add(key)
        sequence = row.get("sequence")
        if not isinstance(sequence, Mapping) or sequence.get("frames") != 8:
            raise TemporalMatrixError("base matrix temporal rows must remain eight-frame rows")
        metrics = row.get("metrics", {})
        if isinstance(metrics, Mapping) and any(
            metrics.get(name) is not None for name in _EVENT_BOUNDARY_METRICS
        ):
            raise TemporalMatrixError(
                "separate event evidence cannot populate event metrics on eight-frame base rows"
            )
    if actual_keys != expected_keys:
        raise TemporalMatrixError(
            "base matrix temporal keys do not match campaign coverage; "
            f"missing={sorted(expected_keys - actual_keys)}, "
            f"extra={sorted(actual_keys - expected_keys)}"
        )


def assemble_event_matrix(
    campaign: Mapping[str, Any],
    base_matrix: Mapping[str, Any],
    event_inputs: Sequence[Mapping[str, Any]],
) -> dict[str, Any]:
    """Return the base matrix plus identity-validated event evidence rows."""

    try:
        validate_campaign(campaign)
    except Exception as error:
        raise TemporalMatrixError(f"campaign is not valid: {error}") from error
    _validate_base_matrix(campaign, base_matrix)
    candidates = {candidate["id"]: candidate for candidate in campaign["candidates"]}
    seen: set[tuple[str, str, str]] = set()
    event_rows: list[dict[str, Any]] = []
    for item in event_inputs:
        csv_path = Path(item["temporalCsv"])
        trace_path = Path(item["eventTrace"])
        motion_path = Path(item["motionJson"]) if item.get("motionJson") else None
        mask_path = Path(item["staticMaskJson"]) if item.get("staticMaskJson") else None
        declared_class = item["declaredClass"]
        raw = _read_temporal_csv(csv_path)
        candidate_id = raw["candidateId"]
        candidate = candidates.get(candidate_id)
        if candidate is None:
            raise TemporalMatrixError(f"event evidence names unknown candidate: {candidate_id}")
        _expected_class(campaign, raw["scene"], declared_class)
        key = (candidate_id, raw["scene"], declared_class)
        if key in seen:
            raise TemporalMatrixError(f"duplicate event evidence row: {key}")
        seen.add(key)
        if raw["class"] != declared_class:
            raw_class = raw["class"]
        else:
            raw_class = None
        if not _config_matches(raw["configId"], candidate["configPath"]):
            raise TemporalMatrixError(f"event CSV config identity mismatch: {csv_path}")
        expected_width, expected_height = (int(value) for value in campaign["dimensions"]["output"].split("x"))
        if raw["width"] != expected_width or raw["height"] != expected_height:
            raise TemporalMatrixError(f"event CSV dimensions mismatch: {csv_path}")
        if raw["frames"] != raw["endFrame"] - raw["startFrame"] + 1 or raw["frames"] < 5:
            raise TemporalMatrixError(f"event CSV frame range is invalid: {csv_path}")
        for metric in _EVENT_METRICS:
            if raw[metric] is None:
                raise TemporalMatrixError(f"event CSV lacks accepted metric {metric}: {csv_path}")
        trace = _check_event_trace(
            trace_path,
            raw,
            expected_candidate=candidate_id,
            expected_class=declared_class,
        )
        _check_sidecars(
            motion_path,
            mask_path,
            candidate_id=candidate_id,
            scene=raw["scene"],
            start_frame=raw["startFrame"],
            end_frame=raw["endFrame"],
            width=raw["width"],
            height=raw["height"],
        )
        sidecar_source = {
            "motion": str(motion_path) if motion_path is not None else None,
            "staticMask": str(mask_path) if mask_path is not None else None,
        }
        sidecar_hashes = {}
        if motion_path is not None and mask_path is not None:
            sidecar_hashes = {
                "motion": _sha256(motion_path),
                "staticMask": _sha256(mask_path),
            }
        event_rows.append({
            "candidateId": candidate_id,
            "scene": raw["scene"],
            "qualityClass": declared_class,
            "inputResolution": campaign["dimensions"]["source"],
            "outputResolution": campaign["dimensions"]["output"],
            "sequence": {
                "startFrame": raw["startFrame"],
                "endFrame": raw["endFrame"],
                "frames": raw["frames"],
                "width": raw["width"],
                "height": raw["height"],
            },
            "metrics": {name: raw[name] for name in _EVENT_METRICS},
            "event": {
                "authoritativeEventIndex": trace["authoritativeEventIndex"],
                "eventFrameIndex": trace["eventFrameIndex"],
                "eventTransitionIndex": trace["eventTransitionIndex"],
                "resetIndex": trace["resetIndex"],
                "ghostEventIndex": trace["ghostEventIndex"],
                "ghostThreshold": trace["ghostThreshold"],
                "resetThreshold": trace["resetThreshold"],
                "resetCause": trace.get("resetCause"),
                "ghostCause": trace.get("ghostCause"),
                "sourcePath": trace["identity"]["sourcePath"],
                "referencePath": trace["identity"]["referencePath"],
            },
            "provenance": {
                "gitCommit": candidate["gitCommit"],
                "binarySha256": candidate["binarySha256"],
                "configPath": candidate["configPath"],
                "configSha256": candidate["configSha256"],
            },
            "metricSource": {
                "temporalCsv": str(csv_path),
                "eventTrace": str(trace_path),
                **sidecar_source,
                "sha256": {
                    "temporalCsv": _sha256(csv_path),
                    "eventTrace": _sha256(trace_path),
                    **sidecar_hashes,
                },
                "capturedConfigId": raw["configId"],
                "capturedCandidateId": raw["candidateId"],
                **({"rawClass": raw_class} if raw_class is not None else {}),
            },
        })

    expected_event_keys = {
        (candidate["id"], "sintel_cave", "low-light-shadow-detail")
        for candidate in campaign["candidates"]
    }
    if seen != expected_event_keys:
        raise TemporalMatrixError(
            f"event evidence must cover exactly the five cave rows: missing={sorted(expected_event_keys - seen)}, extra={sorted(seen - expected_event_keys)}"
        )
    output = json.loads(json.dumps(base_matrix))
    output["eventEvidence"] = {
        "scope": "separate_event_evidence",
        "complete": True,
        "status": "complete",
        "strictMatrixInput": False,
        "promotesBaseRows": False,
        "coverageKeyFields": ["candidateId", "scene", "qualityClass"],
        "baseTemporalRowCount": len(base_matrix["temporal"]),
        "rowCount": len(event_rows),
        "coverageKeys": [list(key) for key in sorted(seen)],
        "requiredMetrics": list(_EVENT_METRICS),
        "rows": event_rows,
        "note": "Event-spanning rows are a separate evidence set. They do not replace or populate the retained eight-frame rows and are not strict-matrix input.",
    }
    output.setdefault("temporalEvidence", {})["eventEvidence"] = {
        "scope": "separate_event_evidence",
        "complete": True,
        "status": "complete",
        "strictMatrixInput": False,
        "promotesBaseRows": False,
        "rowCount": len(event_rows),
    }
    output["eventEvidenceCsvCount"] = len(event_rows)
    return output


__all__ = ["assemble_event_matrix"]
