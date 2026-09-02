#!/usr/bin/env python3
"""Assemble authoritative per-frame runtime event records for one capture.

The player emits one record per captured output frame. This command joins those
records with explicit capture identity and explicit metric thresholds. It never
looks at image error, saved out-of-range indices, or a motion ``reset`` field to
invent an event.
"""

from __future__ import annotations

import argparse
import json
import math
from collections.abc import Mapping
from pathlib import Path
from typing import Any


SCHEMA = "temporal_forge.event_trace.v1"
DETECTOR_CONTRACT = "side_buffer_scene_cut.v1"
MIN_PRE_EVENT_FRAMES = 2
MIN_POST_EVENT_FRAMES = 2


def _number(value: object, name: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f"{name} must be numeric")
    result = float(value)
    if not math.isfinite(result):
        raise ValueError(f"{name} must be finite")
    return result


def _text(value: object, name: str) -> str:
    if not isinstance(value, str) or not value:
        raise ValueError(f"{name} must be a non-empty string")
    return value


def _record_paths(records_dir: Path, expected_frames: int) -> list[Path]:
    if expected_frames < MIN_PRE_EVENT_FRAMES + MIN_POST_EVENT_FRAMES + 1:
        raise ValueError("complete event trace needs enough pre-event and post-event frames")
    paths = [records_dir / f"event_trace_{index:04d}.json" for index in range(expected_frames)]
    if any(not path.is_file() or path.stat().st_size == 0 for path in paths):
        raise ValueError("complete event trace is required for every captured frame")
    extras = sorted(records_dir.glob("event_trace_*.json"))
    if len(extras) != expected_frames or set(extras) != set(paths):
        raise ValueError("event trace contains missing or out-of-range frame records")
    return paths


def _load_records(records_dir: Path, expected_frames: int) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    previous_pts: float | None = None
    previous_decoder_index: int | None = None
    for index, path in enumerate(_record_paths(records_dir, expected_frames)):
        try:
            value = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            raise ValueError(f"invalid event trace record {path}: {error}") from error
        if not isinstance(value, dict) or value.get("schema") != SCHEMA:
            raise ValueError(f"event trace record {path} has the wrong schema")
        if value.get("eventIndex") != index or value.get("eventFrameIndex") != index:
            raise ValueError(f"event trace record {path} is not aligned to capture index {index}")
        expected_transition = None if index == 0 else index - 1
        if value.get("transitionIndex") != expected_transition:
            raise ValueError(f"event trace record {path} has an invalid transition index")
        pts = _number(value.get("ptsUs"), f"{path}.ptsUs")
        if previous_pts is not None and pts <= previous_pts:
            raise ValueError("event trace timestamps must be strictly increasing")
        previous_pts = pts
        decoder_index = value.get("decoderReceiveIndex")
        if isinstance(decoder_index, bool) or not isinstance(decoder_index, int):
            raise ValueError(f"{path}.decoderReceiveIndex must be an integer")
        if previous_decoder_index is not None and decoder_index != previous_decoder_index + 1:
            raise ValueError("event trace decoder receive indices must be contiguous")
        previous_decoder_index = decoder_index
        inputs = value.get("detectorInputs")
        provenance = value.get("thresholdProvenance")
        if not isinstance(inputs, Mapping) or not isinstance(provenance, Mapping):
            raise ValueError(f"event trace record {path} lacks detector provenance")
        if provenance.get("contract") != DETECTOR_CONTRACT:
            raise ValueError(f"event trace record {path} lacks the detector contract")
        for name in (
            "histogramDelta",
            "avgLumaDelta",
            "motionConfidence",
            "ptsGapMs",
            "expectedFrameIntervalMs",
        ):
            _number(inputs.get(name), f"{path}.detectorInputs.{name}")
        for name in (
            "histogramDeltaGreaterThan",
            "motionConfidenceLessThan",
            "ptsGapMultiplierGreaterThan",
        ):
            _number(provenance.get(name), f"{path}.thresholdProvenance.{name}")
        if not isinstance(value.get("event"), bool):
            raise ValueError(f"event trace record {path}.event must be boolean")
        records.append(value)
    return records


def _classify_event(record: Mapping[str, Any]) -> dict[str, Any]:
    """Classify a recorded detector event without changing its raw labels."""

    inputs = record["detectorInputs"]
    provenance = record["thresholdProvenance"]
    signals: list[str] = []
    if inputs["histogramDelta"] > provenance["histogramDeltaGreaterThan"]:
        signals.append("histogram_delta")
    if inputs["motionConfidence"] < provenance["motionConfidenceLessThan"]:
        signals.append("motion_confidence")
    if inputs["ptsGapMs"] > (
        inputs["expectedFrameIntervalMs"] * provenance["ptsGapMultiplierGreaterThan"]
    ):
        signals.append("pts_gap")
    cause = signals[0] if len(signals) == 1 else (
        "compound" if signals else "unclassified"
    )
    return {
        "eventIndex": record["eventIndex"],
        "cause": cause,
        "signals": signals,
        "ptsUs": record["ptsUs"],
        "ptsDeltaMs": record["ptsDeltaMs"],
        "detectorInputs": dict(inputs),
        "thresholdProvenance": dict(provenance),
        "rawDetectorSceneCut": record["detectorSceneCut"],
        "rawResetCause": record["resetCause"],
        "rawGhostCause": record["ghostCause"],
    }


def assemble_event_trace(
    records_dir: Path,
    *,
    expected_frames: int,
    candidate_id: str,
    scene: str,
    quality_class: str | None = None,
    analysis_frame_indices: list[int] | None = None,
    config_id: str,
    start_frame: int,
    source_path: str,
    reference_path: str,
    output_width: int,
    output_height: int,
    ghost_threshold: float,
    reset_threshold: float,
    allow_no_event: bool = False,
) -> dict[str, Any]:
    """Return one identity-bound, event-spanning trace document."""

    if start_frame < 0:
        raise ValueError("start_frame must be non-negative")
    identity = {
        "candidateId": _text(candidate_id, "candidate_id"),
        "scene": _text(scene, "scene"),
        "configId": _text(config_id, "config_id"),
        "sourcePath": _text(source_path, "source_path"),
        "referencePath": _text(reference_path, "reference_path"),
        "startFrame": start_frame,
        "endFrame": start_frame + expected_frames - 1,
        "outputWidth": output_width,
        "outputHeight": output_height,
    }
    if quality_class is not None:
        identity["qualityClass"] = _text(quality_class, "quality_class")
    if analysis_frame_indices is not None:
        if len(analysis_frame_indices) < 2:
            raise ValueError("analysis_frame_indices must select at least two frames")
        if analysis_frame_indices != sorted(set(analysis_frame_indices)):
            raise ValueError("analysis_frame_indices must be sorted and unique")
        if any(
            isinstance(index, bool)
            or not isinstance(index, int)
            or index < 0
            or index >= expected_frames
            for index in analysis_frame_indices
        ):
            raise ValueError("analysis_frame_indices contains an out-of-range value")
    if output_width <= 0 or output_height <= 0:
        raise ValueError("output dimensions must be positive")
    ghost_limit = _number(ghost_threshold, "ghost_threshold")
    reset_limit = _number(reset_threshold, "reset_threshold")
    records = _load_records(records_dir, expected_frames)
    events = [
        record for record in records
        if record.get("event") is True and record.get("eventIndex", 0) != 0
    ]
    detector_events = [record for record in events if record.get("detectorSceneCut") is True]
    if not detector_events and not allow_no_event:
        raise ValueError("no later detector scene-cut event was emitted")
    eligible_events = [
        record
        for record in detector_events
        if int(record["eventIndex"]) >= MIN_PRE_EVENT_FRAMES
        and expected_frames - int(record["eventIndex"]) - 1 >= MIN_POST_EVENT_FRAMES
    ]
    if not eligible_events and not allow_no_event:
        raise ValueError(
            "no later detector scene-cut event has enough pre-event and post-event frames"
        )
    classifications = {
        int(record["eventIndex"]): _classify_event(record)
        for record in detector_events
    }
    pts_gap_events = [
        record for record in eligible_events
        if classifications[int(record["eventIndex"])]["cause"] == "pts_gap"
    ]
    authoritative = pts_gap_events[0] if pts_gap_events else (
        eligible_events[0] if eligible_events else None
    )
    authoritative_classification = (
        classifications[int(authoritative["eventIndex"])] if authoritative else None
    )
    event_index = int(authoritative["eventIndex"]) if authoritative else None
    reset_index = authoritative.get("transitionIndex") if authoritative else None
    ghost_index = reset_index + 1 if isinstance(reset_index, int) else None
    if authoritative:
        if not isinstance(reset_index, int):
            raise ValueError("authoritative event has no measured reset transition")
        if ghost_index >= expected_frames - 1:
            raise ValueError("event has no measured post-event transition for ghost analysis")

    document = {
        "schema": SCHEMA,
        "identity": identity,
        "capture": {"frames": expected_frames, "recordDirectory": str(records_dir)},
        "authoritativeEvent": authoritative,
        "events": events,
        "eventCauseClassifications": [
            classifications[int(record["eventIndex"])] for record in detector_events
        ],
        "authoritativeCause": authoritative_classification,
        "authoritativeEventIndex": event_index,
        "eventFrameIndex": event_index,
        "eventTransitionIndex": reset_index,
        "resetIndex": reset_index,
        "resetCause": authoritative["resetCause"] if authoritative else None,
        "ghostEventIndex": ghost_index,
        "ghostCause": authoritative["ghostCause"] if authoritative else None,
        "ghostThreshold": ghost_limit,
        "resetThreshold": reset_limit,
        "thresholdProvenance": {
            "detector": authoritative["thresholdProvenance"] if authoritative else None,
            "metricContract": "m6.event_metrics.v1",
            "metricThresholdSource": "explicit_capture_arguments",
            "ghostThreshold": ghost_limit,
            "resetThreshold": reset_limit,
            "ghostMetricStart": "first_measured_transition_after_event_transition",
        },
        "frames": records,
        "eventRequired": not allow_no_event,
    }
    if analysis_frame_indices is not None:
        document["analysisFrameIndices"] = analysis_frame_indices
    return document


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--records-dir", required=True, type=Path)
    parser.add_argument("--expected-frames", required=True, type=int)
    parser.add_argument("--candidate-id", required=True)
    parser.add_argument("--scene", required=True)
    parser.add_argument("--quality-class")
    parser.add_argument(
        "--analysis-frame-indices",
        help="comma-separated captured-frame indices used for static metrics",
    )
    parser.add_argument("--config-id", required=True)
    parser.add_argument("--start-frame", required=True, type=int)
    parser.add_argument("--source-path", required=True)
    parser.add_argument("--reference-path", required=True)
    parser.add_argument("--output-width", required=True, type=int)
    parser.add_argument("--output-height", required=True, type=int)
    parser.add_argument("--ghost-threshold", required=True, type=float)
    parser.add_argument("--reset-threshold", required=True, type=float)
    parser.add_argument(
        "--allow-no-event", action="store_true",
        help="permit a valid sequence with no later scene-cut event",
    )
    parser.add_argument("--output", required=True, type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    analysis_frame_indices = None
    if args.analysis_frame_indices is not None:
        try:
            analysis_frame_indices = [
                int(value) for value in args.analysis_frame_indices.split(",") if value
            ]
        except ValueError as error:
            raise ValueError("analysis-frame-indices must be comma-separated integers") from error
        if not analysis_frame_indices:
            raise ValueError("analysis-frame-indices must not be empty")
    document = assemble_event_trace(
        args.records_dir,
        expected_frames=args.expected_frames,
        candidate_id=args.candidate_id,
        scene=args.scene,
        quality_class=args.quality_class,
        analysis_frame_indices=analysis_frame_indices,
        config_id=args.config_id,
        start_frame=args.start_frame,
        source_path=args.source_path,
        reference_path=args.reference_path,
        output_width=args.output_width,
        output_height=args.output_height,
        ghost_threshold=args.ghost_threshold,
        reset_threshold=args.reset_threshold,
        allow_no_event=args.allow_no_event,
    )
    if args.output.exists():
        raise ValueError(f"refusing to overwrite existing event trace: {args.output}")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    print(f"event trace written: {args.output}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"event trace assembly error: {error}")
        raise SystemExit(2)
