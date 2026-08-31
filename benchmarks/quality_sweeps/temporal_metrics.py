"""Pure temporal-quality measurements used by the M6 campaign.

The capture scripts currently report several spatial and frame-delta values.
Those values are intentionally not renamed here.  This module defines the
additional measurements only after their inputs and units are explicit:

* ``static_flicker`` measures luminance change inside a caller-supplied static
  region, so moving content is not mislabeled as flicker.
* ``edge_variance`` measures the population variance of finite-difference edge
  magnitude over time, averaged over the selected pixels.
* ``motion_compensated_error`` compares a candidate frame with a reference
  frame after sampling the reference at the supplied per-pixel motion vectors.
* ``temporal_motion_compensated_errors`` uses a causal current-to-previous
  motion field to compare candidate and reference frame transitions.  This is
  the metric consumed by the real codec-motion sidecar; it does not pretend
  codec motion is a same-frame candidate-to-reference alignment.
* ``ghost_duration_frames`` and ``reset_recovery_frames`` reduce an already
  measured post-event error trace.  They require an explicit event/reset index;
  they do not infer cuts or disocclusions from a still image.

The implementation deliberately uses small dependency-free Python sequences.
The future PPM extractor can convert captured frames into this representation,
while unit tests can exercise the definitions without a GPU or a video file.
"""

from __future__ import annotations

from collections.abc import Sequence
from math import floor, hypot, isfinite
from typing import TypeAlias

try:  # Optional accelerator; the dependency-free fallback remains canonical.
    import numpy as _np
except ImportError:  # pragma: no cover - exercised on minimal reviewer installs.
    _np = None


Frame: TypeAlias = Sequence[Sequence[float]]
Mask: TypeAlias = Sequence[Sequence[bool]]
MotionField: TypeAlias = Sequence[Sequence[Sequence[float]]]
TemporalMotionField: TypeAlias = Sequence[MotionField | None]


def _is_sequence(value: object) -> bool:
    """Return whether ``value`` is a non-string sequence suitable for a row."""

    return isinstance(value, Sequence) and not isinstance(value, (str, bytes))


def _finite_number(value: object, name: str) -> float:
    """Convert one metric input to a finite float or reject ambiguous data."""

    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f"{name} must contain numeric values")
    converted = float(value)
    if not isfinite(converted):
        raise ValueError(f"{name} must contain finite values")
    return converted


def _normalise_frame(frame: Frame, name: str) -> list[list[float]]:
    """Copy one rectangular finite frame so later calculations are deterministic."""

    if not _is_sequence(frame) or not frame:
        raise ValueError(f"{name} must be a non-empty 2D sequence")
    if not _is_sequence(frame[0]) or not frame[0]:
        raise ValueError(f"{name} must contain non-empty rows")
    width = len(frame[0])
    result: list[list[float]] = []
    for row_index, row in enumerate(frame):
        if not _is_sequence(row) or len(row) != width:
            raise ValueError(f"{name} must be rectangular")
        result.append([
            _finite_number(value, f"{name}[{row_index}]")
            for value in row
        ])
    return result


def _normalise_frames(frames: Sequence[Frame]) -> list[list[list[float]]]:
    """Copy a non-empty frame sequence and require one shared image shape."""

    if not _is_sequence(frames) or not frames:
        raise ValueError("frames must be a non-empty sequence")
    if _np is not None:
        array = _np.asarray(frames, dtype=_np.float64)
        if array.ndim != 3 or 0 in array.shape:
            raise ValueError("frames must be a non-empty rectangular sequence")
        if not _np.isfinite(array).all():
            raise ValueError("frames must contain finite values")
        return array  # type: ignore[return-value]
    result = [_normalise_frame(frame, f"frames[{index}]") for index, frame in enumerate(frames)]
    expected_shape = (len(result[0]), len(result[0][0]))
    if any((len(frame), len(frame[0])) != expected_shape for frame in result[1:]):
        raise ValueError("all frames must have the same dimensions")
    return result


def _normalise_mask(mask: Mask | None, height: int, width: int) -> list[list[bool]]:
    """Validate a pixel-selection mask, defaulting to every pixel."""

    if mask is None:
        return [[True for _ in range(width)] for _ in range(height)]
    if not _is_sequence(mask) or len(mask) != height:
        raise ValueError("mask must have the same height as the frame")
    result: list[list[bool]] = []
    for row in mask:
        if not _is_sequence(row) or len(row) != width:
            raise ValueError("mask must have the same dimensions as the frame")
        if not all(isinstance(value, bool) for value in row):
            raise ValueError("mask values must be boolean")
        result.append(list(row))
    if not any(any(row) for row in result):
        raise ValueError("mask must select at least one pixel")
    return result


def _validate_pair_shape(
    candidate: Sequence[Sequence[float]],
    reference: Sequence[Sequence[float]],
) -> None:
    """Require two normalised frames to represent the same pixel grid."""

    if (len(candidate), len(candidate[0])) != (len(reference), len(reference[0])):
        raise ValueError("candidate and reference must have the same dimensions")


def static_flicker(frames: Sequence[Frame], mask: Mask | None = None) -> float:
    """Return mean absolute frame-to-frame luminance change in a static region.

    Values are in the same units as the input pixels, normally ``0..1`` after
    PPM normalization.  With no mask every pixel is included; callers working
    with moving scenes should provide a region known to be static.
    """

    normalised = _normalise_frames(frames)
    height, width = len(normalised[0]), len(normalised[0][0])
    selected = _normalise_mask(mask, height, width)
    if len(normalised) == 1:
        return 0.0

    total = 0.0
    count = 0
    for previous, current in zip(normalised, normalised[1:]):
        for y in range(height):
            for x in range(width):
                if selected[y][x]:
                    total += abs(current[y][x] - previous[y][x])
                    count += 1
    return total / count


def _edge_magnitude(frame: list[list[float]]) -> list[list[float]]:
    """Compute forward-difference gradient magnitude for one frame.

    The right/down differences are used at each pixel.  The last column and
    row have no forward neighbor and therefore contribute zero in that
    direction; this fixed convention makes the metric reproducible across
    extractors and tests without hiding a padding policy.
    """

    height, width = len(frame), len(frame[0])
    result: list[list[float]] = []
    for y in range(height):
        row: list[float] = []
        for x in range(width):
            horizontal = frame[y][x + 1] - frame[y][x] if x + 1 < width else 0.0
            vertical = frame[y + 1][x] - frame[y][x] if y + 1 < height else 0.0
            row.append(hypot(horizontal, vertical))
        result.append(row)
    return result


def edge_variance(frames: Sequence[Frame], mask: Mask | None = None) -> float:
    """Return mean per-pixel temporal variance of finite-difference edge energy."""

    if _np is not None:
        # Avoid first converting millions of PPM samples through Python lists.
        # NumPy's shape and finiteness checks preserve the same rejection rules
        # as _normalise_frames while keeping the hot validation path bounded.
        pixels = _np.asarray(frames, dtype=_np.float64)
        if pixels.ndim != 3 or 0 in pixels.shape:
            raise ValueError("frames must be a non-empty rectangular sequence")
        if not _np.isfinite(pixels).all():
            raise ValueError("frames must contain finite values")
        height, width = int(pixels.shape[1]), int(pixels.shape[2])
        selected = _normalise_mask(mask, height, width)
        horizontal = _np.zeros_like(pixels)
        vertical = _np.zeros_like(pixels)
        horizontal[:, :, :-1] = pixels[:, :, 1:] - pixels[:, :, :-1]
        vertical[:, :-1, :] = pixels[:, 1:, :] - pixels[:, :-1, :]
        edges = _np.hypot(horizontal, vertical)
        selected_array = _np.asarray(selected, dtype=bool)
        return float(_np.var(edges[:, selected_array], axis=0).mean())

    normalised = _normalise_frames(frames)
    height, width = len(normalised[0]), len(normalised[0][0])
    selected = _normalise_mask(mask, height, width)

    # The capture validator commonly receives dozens of 1080p frames. Keep
    # the exact forward-difference and population-variance definition, but
    # let NumPy perform the pixel loops when available. The list implementation
    # below remains the portable path for standalone installs without NumPy.
    edge_frames = [_edge_magnitude(frame) for frame in normalised]

    total_variance = 0.0
    selected_count = 0
    for y in range(height):
        for x in range(width):
            if not selected[y][x]:
                continue
            values = [edge_frame[y][x] for edge_frame in edge_frames]
            mean = sum(values) / len(values)
            total_variance += sum((value - mean) ** 2 for value in values) / len(values)
            selected_count += 1
    return total_variance / selected_count


def _normalise_motion(
    motion: MotionField,
    height: int,
    width: int,
) -> list[list[tuple[float, float]]]:
    """Validate a per-candidate-pixel ``(dx, dy)`` reference mapping."""

    if not _is_sequence(motion) or len(motion) != height:
        raise ValueError("motion must have the same height as the candidate frame")
    if _np is not None:
        array = _np.asarray(motion, dtype=_np.float64)
        if array.shape != (height, width, 2):
            raise ValueError("motion must have the same dimensions as the candidate frame")
        if not _np.isfinite(array).all():
            raise ValueError("motion must contain finite values")
        return array  # type: ignore[return-value]
    result: list[list[tuple[float, float]]] = []
    for y, row in enumerate(motion):
        if not _is_sequence(row) or len(row) != width:
            raise ValueError("motion must have the same dimensions as the candidate frame")
        output_row: list[tuple[float, float]] = []
        for x, vector in enumerate(row):
            if not _is_sequence(vector) or len(vector) != 2:
                raise ValueError(f"motion[{y}][{x}] must contain dx and dy")
            output_row.append((
                _finite_number(vector[0], f"motion[{y}][{x}].dx"),
                _finite_number(vector[1], f"motion[{y}][{x}].dy"),
            ))
        result.append(output_row)
    return result


def _bilinear_sample(frame: list[list[float]], x: float, y: float) -> float | None:
    """Sample a frame at a fractional coordinate, rejecting outside pixels."""

    height, width = len(frame), len(frame[0])
    if x < 0.0 or y < 0.0 or x > width - 1 or y > height - 1:
        return None
    x0, y0 = floor(x), floor(y)
    x1, y1 = min(x0 + 1, width - 1), min(y0 + 1, height - 1)
    x_weight, y_weight = x - x0, y - y0
    top = frame[y0][x0] * (1.0 - x_weight) + frame[y0][x1] * x_weight
    bottom = frame[y1][x0] * (1.0 - x_weight) + frame[y1][x1] * x_weight
    return top * (1.0 - y_weight) + bottom * y_weight


def motion_compensated_error(
    candidate: Frame,
    reference: Frame,
    motion: MotionField,
    mask: Mask | None = None,
) -> float:
    """Return mean absolute error after applying a reference sampling field.

    ``motion[y][x] = (dx, dy)`` maps a candidate pixel at ``(x, y)`` to the
    reference sample at ``(x + dx, y + dy)``.  Pixels whose mapped sample is
    outside the reference frame are skipped, which keeps translation borders
    from being mistaken for reconstruction error.  A mask can additionally
    exclude disocclusions or other pixels without a valid correspondence.
    """

    candidate_frame = _normalise_frame(candidate, "candidate")
    reference_frame = _normalise_frame(reference, "reference")
    _validate_pair_shape(candidate_frame, reference_frame)
    height, width = len(candidate_frame), len(candidate_frame[0])
    selected = _normalise_mask(mask, height, width)
    vectors = _normalise_motion(motion, height, width)

    total = 0.0
    count = 0
    for y in range(height):
        for x in range(width):
            if not selected[y][x]:
                continue
            dx, dy = vectors[y][x]
            sampled = _bilinear_sample(reference_frame, x + dx, y + dy)
            if sampled is None:
                continue
            total += abs(candidate_frame[y][x] - sampled)
            count += 1
    if count == 0:
        raise ValueError("motion field has no valid samples")
    return total / count


def temporal_motion_compensated_errors(
    candidate_frames: Sequence[Frame],
    reference_frames: Sequence[Frame],
    motion_fields: TemporalMotionField,
    mask: Mask | None = None,
) -> list[float]:
    """Measure candidate/reference residual error for every causal transition.

    ``motion_fields[index]`` maps a pixel in frame ``index`` to the sample in
    frame ``index - 1`` using ``(x + dx, y + dy)``.  The first frame has no
    transition and may therefore use ``None``.  For every later frame the
    candidate and reference temporal residuals are computed with the same
    declared flow, then compared.  A perfect candidate/reference pair has a
    zero residual even when the scene is moving.

    The returned list is indexed by transition, not by source frame: element
    zero describes frame 1 relative to frame 0.  This distinction is part of
    the sidecar contract and keeps reset/ghost event indices unambiguous.
    """

    candidates = _normalise_frames(candidate_frames)
    references = _normalise_frames(reference_frames)
    if len(candidates) != len(references):
        raise ValueError("candidate and reference sequences must have equal length")
    if len(candidates) < 2:
        raise ValueError("at least two frames are required for temporal motion error")
    if not _is_sequence(motion_fields) or len(motion_fields) != len(candidates):
        raise ValueError("one temporal motion entry is required for every frame")

    height, width = len(candidates[0]), len(candidates[0][0])
    selected = _normalise_mask(mask, height, width)
    errors: list[float | None] = []

    if _np is not None:
        selected_array = _np.asarray(selected, dtype=bool)
        grid_x, grid_y = _np.meshgrid(
            _np.arange(width, dtype=_np.float64),
            _np.arange(height, dtype=_np.float64),
        )

        def sample(frame, sample_x, sample_y):
            valid = (
                (sample_x >= 0.0) & (sample_y >= 0.0) &
                (sample_x <= width - 1) & (sample_y <= height - 1)
            )
            x0 = _np.floor(_np.clip(sample_x, 0, width - 1)).astype(_np.intp)
            y0 = _np.floor(_np.clip(sample_y, 0, height - 1)).astype(_np.intp)
            x1 = _np.minimum(x0 + 1, width - 1)
            y1 = _np.minimum(y0 + 1, height - 1)
            wx = sample_x - x0
            wy = sample_y - y0
            top = frame[y0, x0] * (1.0 - wx) + frame[y0, x1] * wx
            bottom = frame[y1, x0] * (1.0 - wx) + frame[y1, x1] * wx
            return top * (1.0 - wy) + bottom * wy, valid

        for index in range(1, len(candidates)):
            motion = motion_fields[index]
            if motion is None:
                errors.append(None)
                continue
            vectors = _np.asarray(motion, dtype=_np.float64)
            if vectors.shape != (height, width, 2) or not _np.isfinite(vectors).all():
                raise ValueError("motion must match the frame dimensions and contain finite values")
            validity_value = getattr(motion, "validity", None)
            if validity_value is None:
                correspondence_valid = _np.ones((height, width), dtype=bool)
            else:
                correspondence_valid = _np.asarray(validity_value, dtype=bool)
                if correspondence_valid.shape != (height, width):
                    raise ValueError("motion validity must match the frame dimensions")
            sample_x = grid_x + vectors[:, :, 0]
            sample_y = grid_y + vectors[:, :, 1]
            candidate_sample, valid = sample(candidates[index - 1], sample_x, sample_y)
            reference_sample, reference_valid = sample(references[index - 1], sample_x, sample_y)
            valid &= reference_valid & selected_array & correspondence_valid
            if not valid.any():
                raise ValueError(f"motion has no valid samples for transition frame {index}")
            residual = (
                candidates[index] - candidate_sample -
                references[index] + reference_sample
            )
            errors.append(float(_np.abs(residual[valid]).mean()))
        return errors

    for index in range(1, len(candidates)):
        motion = motion_fields[index]
        if motion is None:
            # A reset transition may intentionally have no causal vector. It
            # remains unavailable so the caller can measure recovery without
            # pretending identity motion was observed.
            errors.append(None)
            continue
        vectors = _normalise_motion(motion, height, width)
        validity_value = getattr(motion, "validity", None)
        if validity_value is None:
            correspondence_valid = [[True] * width for _ in range(height)]
        else:
            correspondence_valid = [list(row) for row in validity_value]
            if len(correspondence_valid) != height or any(
                len(row) != width for row in correspondence_valid
            ):
                raise ValueError("motion validity must match the frame dimensions")
        current_candidate = candidates[index]
        previous_candidate = candidates[index - 1]
        current_reference = references[index]
        previous_reference = references[index - 1]
        total = 0.0
        count = 0
        for y in range(height):
            for x in range(width):
                if not selected[y][x] or not correspondence_valid[y][x]:
                    continue
                dx, dy = vectors[y][x]
                previous_candidate_sample = _bilinear_sample(
                    previous_candidate, x + dx, y + dy
                )
                previous_reference_sample = _bilinear_sample(
                    previous_reference, x + dx, y + dy
                )
                if previous_candidate_sample is None or previous_reference_sample is None:
                    continue
                candidate_residual = current_candidate[y][x] - previous_candidate_sample
                reference_residual = current_reference[y][x] - previous_reference_sample
                total += abs(candidate_residual - reference_residual)
                count += 1
        if count == 0:
            raise ValueError(f"motion has no valid samples for transition frame {index}")
        errors.append(total / count)
    return errors


def temporal_motion_compensated_error(
    candidate_frames: Sequence[Frame],
    reference_frames: Sequence[Frame],
    motion_fields: TemporalMotionField,
    mask: Mask | None = None,
) -> float:
    """Return the mean of the causal transition errors for one sequence pair."""

    errors = temporal_motion_compensated_errors(
        candidate_frames, reference_frames, motion_fields, mask
    )
    measured = [error for error in errors if error is not None]
    if not measured:
        raise ValueError("temporal motion has no measured transitions")
    return sum(measured) / len(measured)


def _normalise_error_trace(errors: Sequence[float | None]) -> list[float | None]:
    """Copy an event trace while preserving explicit unavailable transitions."""

    if not _is_sequence(errors) or not errors:
        raise ValueError("errors must be a non-empty sequence")
    return [
        None if value is None else _finite_number(value, f"errors[{index}]")
        for index, value in enumerate(errors)
    ]


def _event_index(errors: Sequence[float | None], index: int, name: str) -> None:
    """Validate an event marker, allowing an event immediately after the trace."""

    if isinstance(index, bool) or not isinstance(index, int):
        raise ValueError(f"{name} must be an integer")
    if index < 0 or index > len(errors):
        raise ValueError(f"{name} must be within the error trace")


def ghost_duration_frames(
    errors: Sequence[float | None],
    threshold: float,
    event_index: int,
) -> int:
    """Count the contiguous above-threshold run beginning at an explicit event."""

    trace = _normalise_error_trace(errors)
    limit = _finite_number(threshold, "threshold")
    _event_index(trace, event_index, "event_index")
    if trace[event_index] is None:
        raise ValueError("ghost event must begin on a measured transition")
    duration = 0
    for value in trace[event_index:]:
        if value is None:
            break
        if value <= limit:
            break
        duration += 1
    return duration


def reset_recovery_frames(
    errors: Sequence[float | None],
    threshold: float,
    reset_index: int,
) -> int | None:
    """Return frames from an explicit reset until error reaches the threshold.

    ``None`` means the captured trace ended before recovery.  That result must
    remain unavailable in campaign output rather than being converted to a
    fabricated zero or a guessed duration.
    """

    trace = _normalise_error_trace(errors)
    limit = _finite_number(threshold, "threshold")
    _event_index(trace, reset_index, "reset_index")
    for index in range(reset_index, len(trace)):
        if trace[index] is not None and trace[index] <= limit:
            return index - reset_index
    return None


__all__ = [
    "Frame",
    "Mask",
    "MotionField",
    "edge_variance",
    "ghost_duration_frames",
    "motion_compensated_error",
    "reset_recovery_frames",
    "static_flicker",
    "temporal_motion_compensated_error",
    "temporal_motion_compensated_errors",
]
