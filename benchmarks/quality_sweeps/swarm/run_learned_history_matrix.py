#!/usr/bin/env python3
"""Run the isolated 12-arm learned/history/jitter/confidence matrix.

This is deliberately a campaign runner, not a reconstruction implementation.
It delegates frame production to ``run_temporal_quality.sh`` and only owns
matrix expansion, environment isolation, provenance, fresh artifact paths,
timing retention, and fail-closed comparisons against a product baseline.

The matrix is:

    learned strength (2) x jitter (2) x
    (history off/confidence 0 OR history on/confidence 0/0.75) = 12 arms.

The runner never edits the player, shaders, model files, benchmark images, or
existing result directories. A numerically passing arm is still marked
``review_required`` because human visual review is a required promotion gate.
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import hashlib
import itertools
import json
import math
import os
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[3]
TEMPORAL_RUNNER = ROOT / "benchmarks/video_corpus/run_temporal_quality.sh"
SCENES = ("tos_daylight", "tos_debris", "sintel_rooftop", "sintel_cave")
INPUT_RESOLUTIONS = ("426x240", "1280x720")
OUTPUT_BY_INPUT = {"426x240": "1920x1080", "1280x720": "3840x2160"}
DIMENSIONS = re.compile(r"^[1-9][0-9]*x[1-9][0-9]*$")
SAFE_ID = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]*$")
TIMING = re.compile(
    r"GPU\(all\s+[0-9]+\s+passes\)=(?P<gpu>[0-9.]+)ms"
)
REQUIRED_CSV_FIELDS = {
    "frames",
    "output_width",
    "output_height",
    "fsr_ssim_mean",
    "fsr_temporal_delta_abs_error",
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.replace(temporary, path)


def parse_dimensions(value: str, field: str) -> tuple[int, int]:
    if not DIMENSIONS.fullmatch(value):
        raise ValueError(f"{field} must be WIDTHxHEIGHT: {value!r}")
    width, height = (int(part) for part in value.split("x"))
    return width, height


def expand_arms(input_resolution: str) -> list[dict[str, Any]]:
    """Create exactly 12 meaningful arms without invalid confidence states."""
    conservative, stronger = {
        "426x240": (0.05, 0.15),
        "1280x720": (0.15, 0.55),
    }[input_resolution]
    arms: list[dict[str, Any]] = []
    for strength_name, strength in (("conservative", conservative), ("stronger", stronger)):
        for jitter_name, jitter in (("jitter_off", "0"), ("jitter_on", "1")):
            for history_name, confidence_values in (
                ("history_off", (None,)),
                ("history_on", ("0", "0.75")),
            ):
                for confidence in confidence_values:
                    confidence_name = "confidence_0" if confidence is None else f"confidence_{confidence.replace('.', '')}"
                    arm_id = f"{strength_name}-{jitter_name}-{history_name}-{confidence_name}"
                    arms.append(
                        {
                            "id": arm_id,
                            "learnedStrength": strength,
                            "learnedStrengthName": strength_name,
                            "jitter": jitter,
                            "jitterName": jitter_name,
                            "history": history_name,
                            "confidence": 0.0 if confidence is None else float(confidence),
                            "environment": {
                                "TFORGE_FSR4_LEARNED_STRENGTH": f"{strength:.6g}",
                                "TFORGE_FSR4_JITTER_MODE": "controlled",
                                "TFORGE_FSR4_CONTROLLED_JITTER": jitter,
                                **(
                                    {"TFORGE_FSR4_DISABLE_COLOR_HISTORY": "1"}
                                    if history_name == "history_off"
                                    else {
                                        "TFORGE_FSR4_ENABLE_COLOR_HISTORY": "1",
                                        "TFORGE_FSR4_LEARNED_CONFIDENCE_BLEND": confidence or "0",
                                    }
                                ),
                            },
                        }
                    )
    if len(arms) != 12 or len({arm["id"] for arm in arms}) != 12:
        raise AssertionError("matrix expansion did not produce 12 unique arms")
    return arms


def scrubbed_environment() -> dict[str, str]:
    """Keep desktop/runtime state, but remove every inherited capture knob."""
    return {
        key: value
        for key, value in os.environ.items()
        if not key.startswith("TFORGE_") and key != "ELECTRON_RUN_AS_NODE"
    }


def make_timeout_wrapper(directory: Path, log_path: Path) -> Path:
    """Retain the player's timing stdout while preserving runner semantics."""
    real_timeout = shutil.which("timeout")
    if not real_timeout:
        raise FileNotFoundError("timeout is required")
    wrapper = directory / "timeout"
    wrapper.write_text(
        "#!/usr/bin/env bash\n"
        "set -o pipefail\n"
        f"{real_timeout} \"$@\" | tee {json.dumps(str(log_path))}\n"
        "exit ${PIPESTATUS[0]}\n",
        encoding="utf-8",
    )
    wrapper.chmod(0o755)
    return wrapper


def finite_float(value: str, field: str, path: Path) -> float:
    try:
        parsed = float(value)
    except (TypeError, ValueError) as error:
        raise ValueError(f"{path}: invalid {field}: {value!r}") from error
    if not math.isfinite(parsed):
        raise ValueError(f"{path}: non-finite {field}: {value!r}")
    return parsed


def read_temporal_csv(path: Path, expected_output: str, expected_frames: int) -> dict[str, float]:
    """Require one complete temporal result row with finite gate metrics."""
    if not path.is_file():
        raise ValueError(f"missing temporal result: {path}")
    with path.open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    if len(rows) != 1:
        raise ValueError(f"{path}: expected exactly one result row, got {len(rows)}")
    row = rows[0]
    missing = sorted(REQUIRED_CSV_FIELDS - set(row))
    if missing:
        raise ValueError(f"{path}: missing required fields: {', '.join(missing)}")
    width, height = parse_dimensions(expected_output, "expected output")
    if int(row["frames"]) != expected_frames:
        raise ValueError(f"{path}: frame count mismatch")
    if int(row["output_width"]) != width or int(row["output_height"]) != height:
        raise ValueError(f"{path}: output dimension mismatch")
    return {
        "ssim": finite_float(row["fsr_ssim_mean"], "fsr_ssim_mean", path),
        "temporalError": finite_float(
            row["fsr_temporal_delta_abs_error"],
            "fsr_temporal_delta_abs_error",
            path,
        ),
    }


def read_gpu_median(path: Path) -> float:
    values = [finite_float(match.group("gpu"), "GPU timing", path) for match in TIMING.finditer(path.read_text(encoding="utf-8", errors="replace"))]
    if not values:
        raise ValueError(f"{path}: no GPU timing samples were retained")
    return float(statistics.median(values))


def run_one(
    *,
    output_root: Path,
    arm: dict[str, Any] | None,
    scene: str,
    input_resolution: str,
    binary: Path,
    warmup: int,
    frames: int,
) -> dict[str, Any]:
    """Run one fresh baseline/arm capture with no inherited FSR state."""
    label = "current_default" if arm is None else arm["id"]
    job_root = output_root / label / scene / input_resolution
    if job_root.exists():
        raise FileExistsError(f"refusing to reuse capture namespace: {job_root}")
    job_root.mkdir(parents=True)
    csv_path = job_root / "temporal.csv"
    artifact_dir = job_root / "artifacts"
    log_path = job_root / "player-timing.log"
    wrapper_dir = job_root / "bin"
    wrapper_dir.mkdir()
    make_timeout_wrapper(wrapper_dir, log_path)
    clip = ROOT / "benchmarks/video_corpus/clips" / f"{scene}_{input_resolution}_high_crf12.mp4"
    reference = ROOT / "benchmarks/video_corpus/references" / f"{scene}_2160p_lossless.mkv"
    if not clip.is_file() or clip.stat().st_size == 0:
        raise FileNotFoundError(f"missing clip: {clip}")
    if not reference.is_file() or reference.stat().st_size == 0:
        raise FileNotFoundError(f"missing reference: {reference}")
    environment = scrubbed_environment()
    environment.update(
        {
            "PATH": f"{wrapper_dir}{os.pathsep}{environment.get('PATH', '')}",
            "TFORGE_TEMPORAL_WARMUP_FRAMES": str(warmup),
            "TFORGE_FSR4_PROFILE_TIMINGS": "1",
            "TFORGE_TEMPORAL_ARTIFACT_DIR": str(artifact_dir),
        }
    )
    if arm is not None:
        environment.update(arm["environment"])
    command = [
        str(TEMPORAL_RUNNER),
        str(binary),
        str(clip),
        str(reference),
        str(csv_path),
        str(frames),
    ]
    completed = subprocess.run(command, cwd=ROOT, env=environment, text=True, capture_output=True, check=False)
    (job_root / "runner.log").write_text(completed.stdout + completed.stderr, encoding="utf-8")
    if completed.returncode != 0:
        raise RuntimeError(f"capture failed ({completed.returncode}); see {job_root / 'runner.log'}")
    metrics = read_temporal_csv(csv_path, OUTPUT_BY_INPUT[input_resolution], frames)
    metrics["gpuMedianMs"] = read_gpu_median(log_path)
    return {
        "label": label,
        "scene": scene,
        "inputResolution": input_resolution,
        "outputResolution": OUTPUT_BY_INPUT[input_resolution],
        "environment": {} if arm is None else arm["environment"],
        "metrics": metrics,
        "binary": str(binary),
        "binarySha256": sha256(binary),
        "runner": str(TEMPORAL_RUNNER),
        "artifacts": str(job_root),
    }


def gate( baseline: dict[str, float], candidate: dict[str, float]) -> dict[str, Any]:
    ssim_delta = candidate["ssim"] - baseline["ssim"]
    temporal_delta = candidate["temporalError"] - baseline["temporalError"]
    if baseline["temporalError"] <= 1e-12:
        temporal_relative = 0.0 if temporal_delta <= 0 else float("inf")
    else:
        temporal_relative = temporal_delta / baseline["temporalError"]
    gpu_delta = candidate["gpuMedianMs"] - baseline["gpuMedianMs"]
    passed = (
        ssim_delta >= -0.0005
        and temporal_relative <= 0.02
        and gpu_delta <= 0.25
    )
    return {
        "pass": passed,
        "ssimDelta": ssim_delta,
        "temporalErrorDelta": temporal_delta,
        "temporalErrorRelativeIncrease": temporal_relative,
        "gpuMedianDeltaMs": gpu_delta,
    }


def run(args: argparse.Namespace) -> int:
    if args.frames < 1 or args.warmup < 0:
        raise ValueError("frames must be positive and warmup must be non-negative")
    binary = args.binary.resolve()
    if not binary.is_file() or not os.access(binary, os.X_OK):
        raise FileNotFoundError(f"executable binary not found: {binary}")
    if not TEMPORAL_RUNNER.is_file() or not os.access(TEMPORAL_RUNNER, os.X_OK):
        raise FileNotFoundError(f"temporal runner not executable: {TEMPORAL_RUNNER}")
    selected_scenes = tuple(args.scene or SCENES)
    selected_resolutions = tuple(args.input_resolution or INPUT_RESOLUTIONS)
    if any(scene not in SCENES for scene in selected_scenes):
        raise ValueError("unknown scene selected")
    if any(resolution not in INPUT_RESOLUTIONS for resolution in selected_resolutions):
        raise ValueError("unknown input resolution selected")
    output_root = args.output_root.resolve()
    if output_root.exists():
        if any(output_root.iterdir()):
            raise FileExistsError(f"output root must be new and empty: {output_root}")
    else:
        output_root.mkdir(parents=True)

    matrix = {
        "schema": "temporal_forge.learned_history_matrix.v1",
        "createdUtc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "armsPerResolution": 12,
        "baseline": "current_default",
        "scenes": selected_scenes,
        "inputResolutions": selected_resolutions,
        "framePolicy": {"warmupFrames": args.warmup, "scoredFrames": args.frames},
        "gates": {
            "finiteMetrics": True,
            "expectedOutputDimensions": True,
            "expectedScoredFrames": args.frames,
            "perPairSsimFloorDelta": -0.0005,
            "perPairTemporalErrorRelativeIncreaseAtMost": 0.02,
            "perPairGpuMedianIncreaseMsAtMost": 0.25,
            "minimumImprovedPairs": 3,
            "meanSsimDeltaAtLeast": 0.0,
            "meanTemporalErrorDeltaAtMost": 0.0,
            "humanVisualReviewRequired": True,
            "promotionBlockedUntilHumanReview": True,
        },
        "arms": {resolution: expand_arms(resolution) for resolution in selected_resolutions},
        "noSourceChanges": True,
    }
    write_json(output_root / "matrix.json", matrix)
    results: list[dict[str, Any]] = []
    baseline_results: dict[tuple[str, str], dict[str, Any]] = {}
    for resolution in selected_resolutions:
        for scene in selected_scenes:
            result = run_one(
                output_root=output_root,
                arm=None,
                scene=scene,
                input_resolution=resolution,
                binary=binary,
                warmup=args.warmup,
                frames=args.frames,
            )
            results.append(result)
            baseline_results[(scene, resolution)] = result
            write_json(output_root / "results.partial.json", results)

    decisions: list[dict[str, Any]] = []
    for resolution in selected_resolutions:
        for arm in expand_arms(resolution):
            arm_pairs: list[dict[str, Any]] = []
            for scene in selected_scenes:
                candidate = run_one(
                    output_root=output_root,
                    arm=arm,
                    scene=scene,
                    input_resolution=resolution,
                    binary=binary,
                    warmup=args.warmup,
                    frames=args.frames,
                )
                results.append(candidate)
                pair = gate(
                    baseline_results[(scene, resolution)]["metrics"],
                    candidate["metrics"],
                )
                pair.update({"scene": scene, "inputResolution": resolution})
                arm_pairs.append(pair)
                write_json(output_root / "results.partial.json", results)
            ssim_deltas = [pair["ssimDelta"] for pair in arm_pairs]
            temporal_deltas = [pair["temporalErrorDelta"] for pair in arm_pairs]
            improved = sum(1 for pair in arm_pairs if pair["ssimDelta"] > 0 and pair["temporalErrorDelta"] <= 0)
            numerical_pass = (
                all(pair["pass"] for pair in arm_pairs)
                and sum(ssim_deltas) / len(ssim_deltas) >= 0
                and sum(temporal_deltas) / len(temporal_deltas) <= 0
                and improved >= 3
            )
            decisions.append(
                {
                    "arm": arm["id"],
                    "inputResolution": resolution,
                    "pairs": arm_pairs,
                    "meanSsimDelta": sum(ssim_deltas) / len(ssim_deltas),
                    "meanTemporalErrorDelta": sum(temporal_deltas) / len(temporal_deltas),
                    "improvedPairs": improved,
                    "numericalGate": "pass" if numerical_pass else "fail",
                    "status": "review_required" if numerical_pass else "rejected",
                }
            )

    report = {
        "schema": "temporal_forge.learned_history_matrix_results.v1",
        "matrix": str(output_root / "matrix.json"),
        "binary": str(binary),
        "binarySha256": sha256(binary),
        "results": results,
        "decisions": decisions,
        "overall": "review_required" if any(item["status"] == "review_required" for item in decisions) else "no_numeric_pass",
        "promotion": "blocked_until_human_review",
    }
    write_json(output_root / "results.json", report)
    (output_root / "results.partial.json").unlink(missing_ok=True)
    return 0 if all(item["numericalGate"] == "pass" for item in decisions) else 2


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "build-fast/temporal_forge_player")
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--scene", action="append", help="restrict to one or more real corpus scenes")
    parser.add_argument("--input-resolution", action="append", choices=INPUT_RESOLUTIONS)
    parser.add_argument("--warmup", type=int, default=36)
    parser.add_argument("--frames", type=int, default=24)
    args = parser.parse_args()
    return run(args)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (FileNotFoundError, FileExistsError, RuntimeError, ValueError, AssertionError) as error:
        print(f"learned-history matrix error: {error}", file=sys.stderr)
        raise SystemExit(2)
