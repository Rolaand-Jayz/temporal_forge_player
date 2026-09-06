#!/usr/bin/env python3
"""Generate a validated dense-motion diagnostic for a short video window.

This tool is deliberately outside the playback path. It uses dense
forward/backward optical flow as a high-quality diagnostic reference so the
campaign can answer whether codec motion is the temporal quality limiter. The
saved flow fields are suitable for a later controlled replay experiment, but
this command never changes player defaults, model inputs, or benchmark media.
"""

from __future__ import annotations

import argparse
import json
import math
from dataclasses import dataclass
from pathlib import Path
from typing import Any


@dataclass(frozen=True)
class PairAnalysis:
    """Dense correspondence and validation masks for one adjacent frame pair."""

    forward: Any
    backward: Any
    confidence: Any
    occlusion: Any
    consistency_error: Any
    photometric_error: Any


def replay_frame_vectors(
    pair: PairAnalysis,
    *,
    frame_index: int,
    source_width: int,
    source_height: int,
    tile_size: int = 8,
) -> dict[str, Any]:
    """Convert validated backward flow into conservative source-space tiles.

    The real player uploader accepts sparse rectangular vectors and expands
    them to a dense texture. A tile is emitted only when at least half of its
    pixels passed both validation tests; invalid tiles remain uncovered so the
    temporal shader can reject their history instead of receiving invented
    zero motion.
    """

    _, np = _dependencies()
    if tile_size <= 0 or source_width <= 0 or source_height <= 0:
        raise ValueError("tile and source dimensions must be positive")
    vectors: list[dict[str, Any]] = []
    for y in range(0, source_height, tile_size):
        for x in range(0, source_width, tile_size):
            y1 = min(source_height, y + tile_size)
            x1 = min(source_width, x + tile_size)
            valid = np.logical_not(pair.occlusion[y:y1, x:x1])
            if float(np.mean(valid)) < 0.5:
                continue
            values = pair.backward[y:y1, x:x1][valid]
            confidence = float(np.median(pair.confidence[y:y1, x:x1][valid]))
            vectors.append(
                {
                    "dstX": x,
                    "dstY": y,
                    "mvX": float(np.median(values[:, 0])),
                    "mvY": float(np.median(values[:, 1])),
                    "w": x1 - x,
                    "h": y1 - y,
                    "source": -1,
                    "confidence": confidence,
                }
            )
    return {"frameIndex": frame_index, "vectors": vectors}


def _dependencies() -> tuple[Any, Any]:
    """Load optional campaign-only numerical dependencies with a clear error."""

    try:
        import cv2  # type: ignore
        import numpy as np  # type: ignore
    except ImportError as error:  # pragma: no cover - depends on host tooling
        raise RuntimeError(
            "dense-motion diagnostics require opencv-python and numpy"
        ) from error
    return cv2, np


def analyze_frames(
    previous: Any,
    current: Any,
    *,
    forward_backward_threshold: float,
    photometric_threshold: float,
    method: str = "farneback",
    dis_finest_scale: int = 1,
    dis_variational_refinement_iterations: int = 2,
) -> PairAnalysis:
    """Analyze one pair and return fields plus explicit validity evidence.

    ``forward`` is defined on the previous frame and points previous ->
    current. ``backward`` is defined on the current frame and points current
    -> previous, matching the codec sidecar's current-destination convention.
    A pixel is marked occluded when it leaves the image, fails the
    forward/backward check, or has excessive warped photometric error.
    """

    cv2, np = _dependencies()
    if previous.ndim != 2 or current.ndim != 2 or previous.shape != current.shape:
        raise ValueError("previous and current must be equal-sized grayscale images")
    if forward_backward_threshold <= 0 or photometric_threshold < 0:
        raise ValueError("motion thresholds must be positive/non-negative")

    if method not in {"farneback", "dis", "tvl1"}:
        raise ValueError(f"unsupported dense-flow method: {method}")
    if dis_finest_scale < 0:
        raise ValueError("DIS finest scale must be non-negative")
    if dis_variational_refinement_iterations < 0:
        raise ValueError("DIS variational refinement iterations must be non-negative")
    if method == "farneback":
        flow_kwargs = dict(
            pyr_scale=0.5,
            levels=3,
            winsize=21,
            iterations=3,
            poly_n=5,
            poly_sigma=1.2,
            flags=0,
        )
        forward = cv2.calcOpticalFlowFarneback(previous, current, None, **flow_kwargs)
        backward = cv2.calcOpticalFlowFarneback(current, previous, None, **flow_kwargs)
    elif method == "dis":
        # DIS is a separate diagnostic reference, not a production dependency.
        # Medium preset plus two variational-refinement iterations gives the
        # replay experiment a stronger alternative for large/deforming motion
        # while keeping this tool deterministic and CPU-only.
        dis = cv2.DISOpticalFlow_create(cv2.DISOPTICAL_FLOW_PRESET_MEDIUM)
        dis.setFinestScale(dis_finest_scale)
        dis.setVariationalRefinementIterations(
            dis_variational_refinement_iterations
        )
        forward = dis.calc(previous, current, None)
        backward = dis.calc(current, previous, None)
    else:
        # TV-L1 is available through the installed OpenCV optflow module. It
        # remains diagnostic-only because its CPU cost is higher than the
        # existing estimators; this tests correspondence quality separately
        # from any production-path implementation choice.
        tvl1_factory = getattr(getattr(cv2, "optflow", None), "createOptFlow_DualTVL1", None)
        if tvl1_factory is None:
            raise RuntimeError("TV-L1 dense-flow support is unavailable in this OpenCV build")
        forward = tvl1_factory().calc(previous, current, None)
        backward = tvl1_factory().calc(current, previous, None)

    height, width = current.shape
    yy, xx = np.mgrid[0:height, 0:width].astype(np.float32)
    forward_x = forward[..., 0]
    forward_y = forward[..., 1]
    sample_x = xx + forward_x
    sample_y = yy + forward_y
    inside_forward = (
        (sample_x >= 1.0)
        & (sample_x < width - 1.0)
        & (sample_y >= 1.0)
        & (sample_y < height - 1.0)
    )
    sampled_backward_x = cv2.remap(
        backward[..., 0], sample_x, sample_y, cv2.INTER_LINEAR,
        borderMode=cv2.BORDER_CONSTANT,
    )
    sampled_backward_y = cv2.remap(
        backward[..., 1], sample_x, sample_y, cv2.INTER_LINEAR,
        borderMode=cv2.BORDER_CONSTANT,
    )
    consistency_error = np.hypot(
        forward_x + sampled_backward_x,
        forward_y + sampled_backward_y,
    ).astype(np.float32)

    # Farneback's backward field is defined at current pixels and points to
    # their previous-frame coordinates. Reprojection therefore adds the field;
    # this is the same destination-plus-motion convention consumed by the FSR
    # prepass and by the replay sidecar.
    current_x = xx + backward[..., 0]
    current_y = yy + backward[..., 1]
    warped_previous = cv2.remap(
        previous.astype(np.float32),
        current_x,
        current_y,
        cv2.INTER_LINEAR,
        borderMode=cv2.BORDER_REPLICATE,
    )
    photometric_error = np.abs(
        current.astype(np.float32) / 255.0 - warped_previous / 255.0
    ).astype(np.float32)
    valid = (
        inside_forward
        & (consistency_error <= forward_backward_threshold)
        & (photometric_error <= photometric_threshold)
    )
    # Confidence is continuous for ranking and visualization. The binary
    # occlusion mask remains the authority for whether a history sample may be
    # used in a later controlled replay.
    confidence = np.exp(-consistency_error) * np.exp(-8.0 * photometric_error)
    confidence = np.where(valid, confidence, 0.0).astype(np.float32)
    occlusion = np.logical_not(valid)
    return PairAnalysis(
        forward=forward.astype(np.float32),
        backward=backward.astype(np.float32),
        confidence=confidence,
        occlusion=occlusion,
        consistency_error=consistency_error,
        photometric_error=photometric_error,
    )


def _read_window(path: Path, start_frame: int, frame_count: int) -> list[Any]:
    """Read exactly the requested source window without trusting seek behavior."""

    cv2, _ = _dependencies()
    if start_frame < 0 or frame_count < 2:
        raise ValueError("start-frame must be non-negative and frames must be at least 2")
    capture = cv2.VideoCapture(str(path))
    if not capture.isOpened():
        raise RuntimeError(f"cannot open video: {path}")
    frames: list[Any] = []
    try:
        for index in range(start_frame + frame_count):
            ok, frame = capture.read()
            if not ok:
                raise RuntimeError(
                    f"video ended before requested window at source frame {index}"
                )
            if index >= start_frame:
                frames.append(cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY))
    finally:
        capture.release()
    if len(frames) != frame_count:
        raise RuntimeError("decoded frame count does not match requested window")
    return frames


def _finite(value: Any) -> float:
    """Convert a numerical diagnostic statistic to JSON-safe finite float."""

    result = float(value)
    if not math.isfinite(result):
        raise ValueError("diagnostic produced a non-finite statistic")
    return result


def generate_diagnostic(
    input_path: Path,
    report_path: Path,
    flow_path: Path,
    *,
    start_frame: int,
    frame_count: int,
    forward_backward_threshold: float,
    photometric_threshold: float,
    method: str = "farneback",
    dis_finest_scale: int = 1,
    dis_variational_refinement_iterations: int = 2,
    replay_path: Path | None = None,
    tile_size: int = 8,
) -> dict[str, Any]:
    """Generate the NPZ fields and JSON report, refusing accidental overwrite."""

    _, np = _dependencies()
    if not input_path.is_file():
        raise FileNotFoundError(input_path)
    if tile_size <= 0:
        raise ValueError("tile-size must be positive")
    if report_path.exists() or flow_path.exists() or (replay_path and replay_path.exists()):
        raise FileExistsError(
            f"refusing to overwrite diagnostic output: {report_path}, {flow_path}, or {replay_path}"
        )
    frames = _read_window(input_path, start_frame, frame_count)
    pairs = [
        analyze_frames(
            frames[index],
            frames[index + 1],
            forward_backward_threshold=forward_backward_threshold,
            photometric_threshold=photometric_threshold,
            method=method,
            dis_finest_scale=dis_finest_scale,
            dis_variational_refinement_iterations=dis_variational_refinement_iterations,
        )
        for index in range(frame_count - 1)
    ]
    height, width = frames[0].shape
    pair_reports: list[dict[str, Any]] = []
    for index, pair in enumerate(pairs):
        pair_reports.append(
            {
                "frameIndex": start_frame + index + 1,
                "forwardBackwardMedianPx": _finite(np.median(pair.consistency_error)),
                "forwardBackwardP90Px": _finite(np.percentile(pair.consistency_error, 90)),
                "photometricMae": _finite(np.mean(pair.photometric_error)),
                "validatedCoverage": _finite(np.mean(~pair.occlusion)),
            }
        )
    report = {
        "schema": "temporal_forge.dense_motion_diagnostic.v1",
        "input": str(input_path),
        "startFrame": start_frame,
        "frames": frame_count,
        "width": int(width),
        "height": int(height),
        "method": f"{method}_forward_backward",
        "disFinestScale": dis_finest_scale if method == "dis" else None,
        "disVariationalRefinementIterations": (
            dis_variational_refinement_iterations if method == "dis" else None
        ),
        "forwardBackwardThresholdPx": forward_backward_threshold,
        "photometricThresholdNormalized": photometric_threshold,
        "flowArtifact": str(flow_path),
        "pairs": pair_reports,
    }
    if replay_path is not None:
        # The player exposes dump-sequence frame IDs relative to the requested
        # capture window. Keep source-frame provenance in the report while
        # making the replay sidecar directly addressable by that runtime ID.
        replay_frames = [
            {"frameIndex": 0, "vectors": []}
        ] + [
            replay_frame_vectors(
                pair,
                frame_index=index + 1,
                source_width=width,
                source_height=height,
                tile_size=tile_size,
            )
            for index, pair in enumerate(pairs)
        ]
        replay = {
            "schema": "temporal_forge.codec_motion.v1",
            "coordinateDomain": "current_destination_to_previous_reference",
            "motionUnits": "source_pixels",
            "sampleConvention": "destination_plus_motion",
            "sourceWidth": int(width),
            "sourceHeight": int(height),
            "targetWidth": int(width),
            "targetHeight": int(height),
            "frameIndexBase": "capture_relative",
            "frames": replay_frames,
        }
        report["replayArtifact"] = str(replay_path)
    report_path.parent.mkdir(parents=True, exist_ok=True)
    flow_path.parent.mkdir(parents=True, exist_ok=True)
    np.savez_compressed(
        flow_path,
        forward=np.stack([pair.forward for pair in pairs]),
        backward=np.stack([pair.backward for pair in pairs]),
        confidence=np.stack([pair.confidence for pair in pairs]),
        occlusion=np.stack([pair.occlusion for pair in pairs]),
        frame_indices=np.arange(start_frame + 1, start_frame + frame_count),
    )
    if replay_path is not None:
        replay_path.write_text(json.dumps(replay, indent=2) + "\n", encoding="utf-8")
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return report


def _parser() -> argparse.ArgumentParser:
    """Build the command-line parser for reproducible campaign captures."""

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True, help="JSON report path")
    parser.add_argument("--flow-output", type=Path, required=True, help="compressed NPZ field path")
    parser.add_argument("--replay-output", type=Path, help="optional validated sparse replay sidecar")
    parser.add_argument("--tile-size", type=int, default=8, help="validated replay tile size in source pixels")
    parser.add_argument("--start-frame", type=int, default=0)
    parser.add_argument("--frames", type=int, default=8)
    parser.add_argument("--fb-threshold", type=float, default=1.0)
    parser.add_argument("--photometric-threshold", type=float, default=13.0 / 255.0)
    parser.add_argument(
        "--method", choices=("farneback", "dis", "tvl1"), default="farneback",
        help="dense-flow implementation used for the diagnostic reference",
    )
    parser.add_argument(
        "--dis-finest-scale", type=int, default=1,
        help="DIS finest pyramid scale; lower values inspect finer detail",
    )
    parser.add_argument(
        "--dis-variational-refinement-iterations", type=int, default=2,
        help="DIS variational-refinement iterations",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    """Run the diagnostic and print the report location."""

    args = _parser().parse_args(argv)
    report = generate_diagnostic(
        args.input,
        args.output,
        args.flow_output,
        start_frame=args.start_frame,
        frame_count=args.frames,
        forward_backward_threshold=args.fb_threshold,
        photometric_threshold=args.photometric_threshold,
        method=args.method,
        dis_finest_scale=args.dis_finest_scale,
        dis_variational_refinement_iterations=args.dis_variational_refinement_iterations,
        replay_path=args.replay_output,
        tile_size=args.tile_size,
    )
    print(f"dense motion diagnostic written: {args.output}")
    print(f"validated pairs: {len(report['pairs'])}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
