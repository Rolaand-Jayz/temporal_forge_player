"""Read captured P6 sequences and assemble the M6 temporal measurements.

This adapter is intentionally narrower than a video decoder.  The existing
capture scripts emit P6 image sequences, so the adapter handles exactly that
format, normalizes RGB to luminance, preserves natural frame order, and passes
the result to :mod:`temporal_metrics`.

Motion compensation is never guessed.  A caller must supply one motion field
per frame, and event metrics remain ``None`` until explicit event metadata is
provided.  The campaign verifier can therefore reject incomplete evidence
instead of accepting an identity-flow or zero-duration placeholder.
"""

from __future__ import annotations

import re
from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import Any

from .temporal_metrics import (
    Frame,
    Mask,
    MotionField,
    edge_variance,
    ghost_duration_frames,
    reset_recovery_frames,
    static_flicker,
    temporal_motion_compensated_errors,
)


_WHITESPACE = b" \t\r\n"
_LUMINANCE = (0.2126, 0.7152, 0.0722)
_STATIC_MASK_SCHEMA = "temporal_forge.static_mask.v1"


def _next_header_token(data: bytes, offset: int) -> tuple[bytes, int]:
    """Read one PPM header token while ignoring whitespace and comments."""

    length = len(data)
    while offset < length:
        if data[offset] in _WHITESPACE:
            offset += 1
            continue
        if data[offset] == ord("#"):
            newline = data.find(b"\n", offset)
            offset = length if newline < 0 else newline + 1
            continue
        break
    start = offset
    while offset < length and data[offset] not in _WHITESPACE and data[offset] != ord("#"):
        offset += 1
    if start == offset:
        raise ValueError("PPM header ended before the expected token")
    return data[start:offset], offset


def _payload_start(data: bytes, offset: int) -> int:
    """Consume the P6 header's separator without eating the first pixel byte."""

    if offset >= len(data) or data[offset] not in _WHITESPACE:
        raise ValueError("PPM header is missing the pixel-data separator")
    offset += 1
    if data[offset - 1] == ord("\r") and offset < len(data) and data[offset] == ord("\n"):
        offset += 1
    return offset


def _parse_positive_integer(token: bytes, name: str) -> int:
    """Parse one positive decimal PPM header field."""

    try:
        value = int(token, 10)
    except ValueError as error:
        raise ValueError(f"PPM {name} is not an integer") from error
    if value <= 0:
        raise ValueError(f"PPM {name} must be positive")
    return value


def read_p6(path: Path) -> list[list[float]]:
    """Read one binary RGB PPM and return a normalized grayscale frame.

    P6 samples are decoded as big-endian values for 16-bit files and divided
    by ``maxval``.  The fixed Rec.709-style luminance weights make the output
    compatible with the scalar temporal metrics without introducing a video
    library dependency into the benchmark tooling.
    """

    try:
        data = path.read_bytes()
    except OSError as error:
        raise ValueError(f"cannot read PPM: {path}") from error

    magic, offset = _next_header_token(data, 0)
    if magic != b"P6":
        raise ValueError(f"{path} is not a binary P6 PPM")
    width_token, offset = _next_header_token(data, offset)
    height_token, offset = _next_header_token(data, offset)
    maxval_token, offset = _next_header_token(data, offset)
    width = _parse_positive_integer(width_token, "width")
    height = _parse_positive_integer(height_token, "height")
    maxval = _parse_positive_integer(maxval_token, "maxval")
    if maxval > 65535:
        raise ValueError("PPM maxval must not exceed 65535")

    payload_offset = _payload_start(data, offset)
    bytes_per_sample = 1 if maxval < 256 else 2
    expected_bytes = width * height * 3 * bytes_per_sample
    payload = data[payload_offset:]
    if len(payload) != expected_bytes:
        raise ValueError(
            f"{path} has {len(payload)} pixel bytes; expected {expected_bytes}"
        )

    def sample(index: int) -> int:
        """Decode one channel sample from the P6 payload."""

        if bytes_per_sample == 1:
            return payload[index]
        return int.from_bytes(payload[index:index + 2], byteorder="big")

    frame: list[list[float]] = []
    payload_index = 0
    for _ in range(height):
        row: list[float] = []
        for _ in range(width):
            red = sample(payload_index)
            green = sample(payload_index + bytes_per_sample)
            blue = sample(payload_index + 2 * bytes_per_sample)
            payload_index += 3 * bytes_per_sample
            row.append(
                (_LUMINANCE[0] * red + _LUMINANCE[1] * green + _LUMINANCE[2] * blue)
                / maxval
            )
        frame.append(row)
    return frame


def _natural_path_key(path: Path) -> list[int | str]:
    """Sort frame filenames numerically where they contain frame numbers."""

    return [
        int(part) if part.isdigit() else part.lower()
        for part in re.split(r"(\d+)", path.name)
    ]


def load_p6_sequence(directory: Path, pattern: str = "*.ppm") -> list[list[list[float]]]:
    """Load every matching P6 frame in natural filename order."""

    if not directory.is_dir():
        raise ValueError(f"PPM sequence directory does not exist: {directory}")
    paths = sorted(directory.glob(pattern), key=_natural_path_key)
    if not paths:
        raise ValueError(f"PPM sequence contains no files matching {pattern}: {directory}")
    frames = [read_p6(path) for path in paths]
    expected_shape = (len(frames[0]), len(frames[0][0]))
    if any((len(frame), len(frame[0])) != expected_shape for frame in frames[1:]):
        raise ValueError("PPM sequence frames must have identical dimensions")
    return frames


def load_static_mask(value: Any, *, width: int, height: int) -> Mask:
    """Load a full mask or compact rectangle metadata for one capture grid.

    The rectangle form keeps human-authored static-region annotations small and
    reviewable.  Bounds are strict: silently clipping a typo would make the
    reported region different from the one the reviewer selected.
    """

    if isinstance(value, list):
        return value
    if not isinstance(value, Mapping):
        raise ValueError("static mask JSON must contain an object or 2D array")
    if value.get("schema") != _STATIC_MASK_SCHEMA:
        raise ValueError(f"static mask schema must be {_STATIC_MASK_SCHEMA}")
    declared_width = value.get("width")
    declared_height = value.get("height")
    if (
        isinstance(declared_width, bool)
        or not isinstance(declared_width, int)
        or declared_width != width
        or isinstance(declared_height, bool)
        or not isinstance(declared_height, int)
        or declared_height != height
    ):
        raise ValueError("static mask dimensions must match the captured sequence")
    rectangles = value.get("rectangles")
    if not isinstance(rectangles, list) or not rectangles:
        raise ValueError("static mask rectangles must be a non-empty list")
    mask = [[False for _ in range(width)] for _ in range(height)]
    for index, raw_rectangle in enumerate(rectangles):
        if not isinstance(raw_rectangle, Mapping):
            raise ValueError(f"static mask rectangle {index} must be an object")
        fields = {}
        for name in ("x", "y", "width", "height"):
            field = raw_rectangle.get(name)
            if isinstance(field, bool) or not isinstance(field, int):
                raise ValueError(f"static mask rectangle {index}.{name} must be an integer")
            fields[name] = field
        if fields["x"] < 0 or fields["y"] < 0 or fields["width"] <= 0 or fields["height"] <= 0:
            raise ValueError(f"static mask rectangle {index} must be positive and non-negative")
        if (
            fields["x"] + fields["width"] > width
            or fields["y"] + fields["height"] > height
        ):
            raise ValueError(f"static mask rectangle {index} is outside the capture grid")
        for y in range(fields["y"], fields["y"] + fields["height"]):
            for x in range(fields["x"], fields["x"] + fields["width"]):
                mask[y][x] = True
    return mask


def analysis_frame_indices(events: Mapping[str, Any], *, frame_count: int) -> list[int]:
    """Return the declared stable-analysis window or every captured frame."""

    if frame_count < 2:
        raise ValueError("at least two frames are required for stable analysis")
    raw_indices = events.get("analysisFrameIndices")
    if raw_indices is None:
        return list(range(frame_count))
    if not isinstance(raw_indices, list) or not raw_indices:
        raise ValueError("analysisFrameIndices must be a non-empty list")
    if any(
        isinstance(index, bool)
        or not isinstance(index, int)
        or index < 0
        or index >= frame_count
        for index in raw_indices
    ):
        raise ValueError("analysisFrameIndices contains an out-of-range value")
    if raw_indices != sorted(set(raw_indices)) or len(raw_indices) != len(set(raw_indices)):
        raise ValueError("analysisFrameIndices must be sorted and unique")
    if len(raw_indices) < 2:
        raise ValueError("analysisFrameIndices must select at least two frames")
    return raw_indices


def _event_number(events: Mapping[str, Any], key: str) -> int | float | None:
    """Return one optional event field without treating absence as a zero."""

    value = events.get(key)
    if value is None:
        return None
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f"event field {key} must be numeric")
    return value


def _event_pair(
    events: Mapping[str, Any],
    index_key: str,
    threshold_key: str,
) -> tuple[int, float] | None:
    """Require both pieces of an event definition when either is present."""

    index = _event_number(events, index_key)
    threshold = _event_number(events, threshold_key)
    if index is None and threshold is None:
        return None
    if index is None or threshold is None:
        raise ValueError(f"event metadata needs both {index_key} and {threshold_key}")
    if not isinstance(index, int):
        raise ValueError(f"event field {index_key} must be an integer")
    return index, float(threshold)


def measure_temporal_sequence(
    candidate_frames: Sequence[Frame],
    reference_frames: Sequence[Frame],
    *,
    motion_fields: Sequence[MotionField] | None = None,
    temporal_motion_fields: Sequence[MotionField | None] | None = None,
    static_mask: Mask | None = None,
    events: Mapping[str, Any] | None = None,
) -> dict[str, float | int | None]:
    """Measure one candidate/reference sequence pair.

    ``temporal_motion_fields`` is the only accepted motion input: each later
    field maps the current frame to the
    previous frame, and the resulting error trace is indexed by transition.
    Same-frame motion is rejected because it cannot establish causal temporal
    evidence.  A static mask is mandatory; treating the whole frame as static
    would turn unannotated motion into a false flicker result.
    The returned event metrics are ``None`` unless ``events`` explicitly
    supplies the corresponding index and threshold pair.  This shape is
    intentional: the campaign contract can distinguish a measured zero from a
    metric that the capture did not define.
    """

    if not candidate_frames or not reference_frames:
        raise ValueError("candidate and reference sequences must be non-empty")
    if len(candidate_frames) != len(reference_frames):
        raise ValueError("candidate and reference sequences must have equal length")
    if motion_fields is not None:
        raise ValueError("same-frame motion is not causal; use temporal motion fields")
    if temporal_motion_fields is None:
        raise ValueError("causal motion fields are required for temporal metrics")
    if static_mask is None:
        raise ValueError("explicit static mask is required for temporal metrics")

    metadata = {} if events is None else events
    if not isinstance(metadata, Mapping):
        raise ValueError("events must be an object")
    selected_analysis_indices = analysis_frame_indices(
        metadata, frame_count=len(candidate_frames)
    )
    analysis_frames = [candidate_frames[index] for index in selected_analysis_indices]

    per_frame_error = temporal_motion_compensated_errors(
        candidate_frames,
        reference_frames,
        temporal_motion_fields,
    )
    ghost_event = _event_pair(metadata, "ghostEventIndex", "ghostThreshold")
    reset_event = _event_pair(metadata, "resetIndex", "resetThreshold")

    ghost_duration: int | None = None
    if ghost_event is not None:
        ghost_duration = ghost_duration_frames(
            per_frame_error,
            threshold=ghost_event[1],
            event_index=ghost_event[0],
        )
    reset_recovery: int | None = None
    if reset_event is not None:
        reset_recovery = reset_recovery_frames(
            per_frame_error,
            threshold=reset_event[1],
            reset_index=reset_event[0],
        )

    # A reset or unavailable transition is represented by ``None`` in the
    # causal error trace.  It must remain unavailable rather than being
    # coerced to zero, and it must not crash aggregation by being added to
    # numeric transitions.  Upstream: the motion sidecar and temporal error
    # calculator. Downstream: CSV output and campaign ranking.
    measured_errors = [error for error in per_frame_error if error is not None]
    if not measured_errors:
        raise ValueError("motion has no measurable causal transitions")

    return {
        "static_flicker": static_flicker(analysis_frames, static_mask),
        "edge_variance": edge_variance(analysis_frames, static_mask),
        "motion_compensated_error": sum(measured_errors) / len(measured_errors),
        "ghost_duration_frames": ghost_duration,
        "reset_recovery_frames": reset_recovery,
    }


__all__ = [
    "analysis_frame_indices",
    "load_p6_sequence",
    "load_static_mask",
    "measure_temporal_sequence",
    "read_p6",
]
