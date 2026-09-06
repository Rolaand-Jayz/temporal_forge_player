"""Validate and expand the explicit causal codec-motion sidecar.

The player exports sparse codec vectors in source-pixel units.  This module is
the boundary between that diagnostic artifact and the dependency-free temporal
metrics: it rejects ambiguous JSON, rejects future-reference vectors, scales
the declared source coordinates to the captured output grid, and expands
overlapping blocks with the same last-vector-wins rule as the GPU owner pass.
"""

from __future__ import annotations

import json
import math
from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import Any

SCHEMA = "temporal_forge.codec_motion.v1"
COORDINATE_DOMAIN = "current_destination_to_previous_reference"
MOTION_UNITS = "source_pixels"
SAMPLE_CONVENTION = "destination_plus_motion"


class MotionFieldWithValidity(list[list[tuple[float, float]]]):
    """Dense motion rows plus explicit per-pixel block coverage.

    The list base class preserves the existing ``field[y][x]`` API used by
    temporal metrics and callers that consume a ``MotionField`` directly.
    ``validity`` is deliberately separate from the vector values: a covered
    static block has valid ``(0, 0)`` motion, while an uncovered target pixel
    also retains the zero-initialized vector but is marked invalid.
    """

    def __init__(
        self,
        rows: Sequence[Sequence[tuple[float, float]]],
        validity: Sequence[Sequence[bool]],
    ) -> None:
        super().__init__([list(row) for row in rows])
        self.validity = [list(row) for row in validity]


def _mapping(value: object, name: str) -> Mapping[str, Any]:
    """Require a JSON object at one named contract boundary."""

    if not isinstance(value, Mapping):
        raise ValueError(f"{name} must be an object")
    return value


def _positive_integer(value: object, name: str) -> int:
    """Require a positive JSON integer while excluding boolean masquerades."""

    if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
        raise ValueError(f"{name} must be a positive integer")
    return value


def _nonnegative_integer(value: object, name: str) -> int:
    """Require a non-negative JSON integer for frame identity fields."""

    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise ValueError(f"{name} must be a non-negative integer")
    return value


def _finite_number(value: object, name: str) -> float:
    """Require finite numeric motion data without accepting booleans."""

    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f"{name} must be numeric")
    result = float(value)
    if not math.isfinite(result):
        raise ValueError(f"{name} must be finite")
    return result


def _vector(
    item: object,
    frame_index: int,
    vector_index: int,
    source_width: int,
    source_height: int,
) -> dict[str, float | int]:
    """Validate one sparse codec vector and enforce causal direction."""

    value = _mapping(item, f"frames[{frame_index}].vectors[{vector_index}]")
    dst_x = _finite_number(value.get("dstX"), "dstX")
    dst_y = _finite_number(value.get("dstY"), "dstY")
    width = _finite_number(value.get("w"), "w")
    height = _finite_number(value.get("h"), "h")
    source = value.get("source")
    if any(not float(number).is_integer() for number in (dst_x, dst_y, width, height)):
        raise ValueError(f"frames[{frame_index}].vectors[{vector_index}] block fields must be integers")
    if width <= 0 or height <= 0:
        raise ValueError(f"frames[{frame_index}].vectors[{vector_index}] extent must be positive")
    if isinstance(source, bool) or not isinstance(source, int):
        raise ValueError(f"frames[{frame_index}].vectors[{vector_index}].source must be an integer")
    if source != -1:
        raise ValueError(
            f"frames[{frame_index}].vectors[{vector_index}] does not identify the "
            "immediately previous reference"
        )
    mv_x = _finite_number(value.get("mvX"), "mvX")
    mv_y = _finite_number(value.get("mvY"), "mvY")
    if abs(mv_x) > source_width:
        raise ValueError(
            f"frames[{frame_index}].vectors[{vector_index}].mvX exceeds source width"
        )
    if abs(mv_y) > source_height:
        raise ValueError(
            f"frames[{frame_index}].vectors[{vector_index}].mvY exceeds source height"
        )
    return {
        "dstX": int(dst_x),
        "dstY": int(dst_y),
        "mvX": mv_x,
        "mvY": mv_y,
        "w": int(width),
        "h": int(height),
        "source": source,
    }


def _expand_frame(
    vectors: Sequence[Mapping[str, float | int]],
    source_width: int,
    source_height: int,
    target_width: int,
    target_height: int,
) -> MotionFieldWithValidity:
    """Expand source-space sparse blocks into a target-space dense field."""

    scale_x = target_width / source_width
    scale_y = target_height / source_height
    field: list[list[tuple[float, float]]] = [
        [(0.0, 0.0) for _ in range(target_width)]
        for _ in range(target_height)
    ]
    validity = [
        [False for _ in range(target_width)]
        for _ in range(target_height)
    ]
    for item in vectors:
        start_x = max(0, math.floor(float(item["dstX"]) * scale_x))
        start_y = max(0, math.floor(float(item["dstY"]) * scale_y))
        end_x = min(
            target_width,
            math.ceil((float(item["dstX"]) + int(item["w"])) * scale_x),
        )
        end_y = min(
            target_height,
            math.ceil((float(item["dstY"]) + int(item["h"])) * scale_y),
        )
        motion = (float(item["mvX"]) * scale_x, float(item["mvY"]) * scale_y)
        # Iterating in JSON order makes the final vector win on overlaps,
        # matching codec_motion_expand.comp's owner-index resolution.
        for y in range(start_y, end_y):
            for x in range(start_x, end_x):
                field[y][x] = motion
                validity[y][x] = True
    return MotionFieldWithValidity(field, validity)


def load_motion_fields(
    sidecar: Mapping[str, Any],
    *,
    expected_frames: int,
    target_width: int,
    target_height: int,
) -> list[MotionFieldWithValidity | None]:
    """Validate a sidecar object and return output-grid temporal fields.

    ``None`` is allowed only for frame zero, which has no previous frame.  A
    later unavailable field is a capture failure, not an invitation to invent
    identity motion.  The target dimensions must match the actual PPM sequence
    so a sidecar cannot silently describe a different output.
    """

    root = _mapping(sidecar, "motion sidecar")
    if root.get("schema") != SCHEMA:
        raise ValueError(f"motion sidecar.schema must be {SCHEMA}")
    if root.get("coordinateDomain") != COORDINATE_DOMAIN:
        raise ValueError(
            "motion sidecar.coordinateDomain must describe current-to-previous motion"
        )
    if root.get("motionUnits") != MOTION_UNITS:
        raise ValueError("motion sidecar.motionUnits must be source_pixels")
    if root.get("sampleConvention") != SAMPLE_CONVENTION:
        raise ValueError("motion sidecar.sampleConvention must be destination_plus_motion")
    if isinstance(expected_frames, bool) or expected_frames < 2:
        raise ValueError("at least two captured frames are required")
    source_width = _positive_integer(root.get("sourceWidth"), "motion sidecar.sourceWidth")
    source_height = _positive_integer(root.get("sourceHeight"), "motion sidecar.sourceHeight")
    declared_width = _positive_integer(root.get("targetWidth"), "motion sidecar.targetWidth")
    declared_height = _positive_integer(root.get("targetHeight"), "motion sidecar.targetHeight")
    if target_width != declared_width or target_height != declared_height:
        raise ValueError("motion sidecar target dimensions do not match the captured sequence")

    frames = root.get("frames")
    if not isinstance(frames, list) or len(frames) != expected_frames:
        raise ValueError("motion sidecar.frames must match the captured frame count")

    result: list[MotionFieldWithValidity | None] = []
    previous_pts_us: float | None = None
    for index, item in enumerate(frames):
        frame = _mapping(item, f"motion sidecar.frames[{index}]")
        if _nonnegative_integer(frame.get("frameIndex"), f"frames[{index}].frameIndex") != index:
            raise ValueError(f"frames[{index}].frameIndex is not in sequence order")
        pts_us = _finite_number(frame.get("ptsUs"), f"frames[{index}].ptsUs")
        # Motion vectors are consumed in displayed/presentation order. Equal
        # or backward timestamps can pair an otherwise valid vector with the
        # wrong transition and contaminate replay and temporal metrics.
        if previous_pts_us is not None and pts_us <= previous_pts_us:
            raise ValueError(
                f"frames[{index}].ptsUs must increase strictly in presentation order"
            )
        previous_pts_us = pts_us
        if not isinstance(frame.get("reset"), bool):
            raise ValueError(f"frames[{index}].reset must be boolean")
        available = frame.get("motionAvailable")
        if not isinstance(available, bool):
            raise ValueError(f"frames[{index}].motionAvailable must be boolean")
        if index == 0 and available:
            raise ValueError(
                "frames[0] cannot declare motionAvailable because it has no previous frame"
            )
        vectors_value = frame.get("vectors")
        if not isinstance(vectors_value, list):
            raise ValueError(f"frames[{index}].vectors must be a list")
        vectors = [
            _vector(vector, index, vector_index, source_width, source_height)
            for vector_index, vector in enumerate(vectors_value)
        ]
        if not available:
            if vectors:
                raise ValueError(f"frames[{index}] cannot contain vectors when motion is unavailable")
            if index != 0 and not frame.get("reset"):
                raise ValueError(
                    f"motion is unavailable for transition frame {index}; "
                    "only an explicit reset may leave a transition unmeasured"
                )
            result.append(None)
            continue
        if not vectors:
            raise ValueError(f"frames[{index}] declares motion but has no vectors")
        result.append(
            _expand_frame(vectors, source_width, source_height, target_width, target_height)
        )
    return result


def assemble_motion_sidecar(
    records: Sequence[Mapping[str, Any]],
    *,
    target_width: int,
    target_height: int,
) -> dict[str, Any]:
    """Assemble complete per-frame export records into one validated sidecar."""

    if not isinstance(records, Sequence) or isinstance(records, (str, bytes)):
        raise ValueError("motion frame records must be a sequence")
    if len(records) < 2:
        raise ValueError("at least two motion frame records are required")
    source_width: int | None = None
    source_height: int | None = None
    frames: list[dict[str, Any]] = []
    for index, raw_record in enumerate(records):
        record = _mapping(raw_record, f"motion frame record {index}")
        frame_index = _nonnegative_integer(record.get("frameIndex"), f"records[{index}].frameIndex")
        if frame_index != index:
            raise ValueError("motion frame records must be contiguous and ordered")
        current_width = _positive_integer(record.get("sourceWidth"), f"records[{index}].sourceWidth")
        current_height = _positive_integer(record.get("sourceHeight"), f"records[{index}].sourceHeight")
        if source_width is None:
            source_width, source_height = current_width, current_height
        elif (source_width, source_height) != (current_width, current_height):
            raise ValueError("motion frame records must share source dimensions")
        frames.append({
            key: value
            for key, value in record.items()
            if key not in {"sourceWidth", "sourceHeight"}
        })

    assert source_width is not None and source_height is not None
    sidecar: dict[str, Any] = {
        "schema": SCHEMA,
        "coordinateDomain": COORDINATE_DOMAIN,
        "motionUnits": MOTION_UNITS,
        "sampleConvention": SAMPLE_CONVENTION,
        "sourceWidth": source_width,
        "sourceHeight": source_height,
        "targetWidth": _positive_integer(target_width, "target_width"),
        "targetHeight": _positive_integer(target_height, "target_height"),
        "frames": frames,
    }
    # Validate the assembled object before returning it so the caller cannot
    # publish a sidecar that the measurement CLI would later reject.
    load_motion_fields(
        sidecar,
        expected_frames=len(frames),
        target_width=sidecar["targetWidth"],
        target_height=sidecar["targetHeight"],
    )
    return sidecar


def read_motion_fields(
    path: Path,
    *,
    expected_frames: int,
    target_width: int,
    target_height: int,
) -> list[MotionFieldWithValidity | None]:
    """Read one JSON sidecar file and pass it through the strict validator."""

    try:
        with path.open("r", encoding="utf-8") as stream:
            value = json.load(stream)
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"cannot read motion sidecar: {path}") from error
    return load_motion_fields(
        _mapping(value, "motion sidecar"),
        expected_frames=expected_frames,
        target_width=target_width,
        target_height=target_height,
    )


__all__ = [
    "MotionFieldWithValidity",
    "assemble_motion_sidecar",
    "load_motion_fields",
    "read_motion_fields",
]
