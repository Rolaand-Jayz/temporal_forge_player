#!/usr/bin/env python3
"""Schedule every declared quality candidate through fresh temporal captures.

This is deliberately a capture orchestrator, not a quality judge.  It creates
one immutable candidate manifest first, then runs each candidate in an isolated
process with a clean ``TFORGE_*`` environment.  ``--mode all`` includes every
single-control arm plus pairwise combinations of the runtime controls.  The
checked-in qualityLab JSON files are also included as independent candidates.

The script never changes the player's defaults.  A candidate is an opt-in
environment/configuration overlay, and failed captures remain visible in the
candidate directory rather than being mistaken for a passing result.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
from typing import Any

from benchmarks.quality_sweeps.trackmania_guard import guarded_worker_count


ROOT = Path(__file__).resolve().parents[3]
RUNNER = ROOT / "benchmarks/video_corpus/run_temporal_quality.sh"
SAFE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]*$")

# These are quality-affecting controls only.  Diagnostic, timing, dump, and
# dispatch-performance switches stay out of this matrix because they do not
# represent a reconstruction candidate.
RUNTIME_DIMENSIONS: tuple[tuple[str, tuple[str, ...]], ...] = (
    ("TFORGE_FSR4_CURRENT_BASE_FILTER", ("bilinear", "mitchell", "catmull_rom", "lanczos2")),
    ("TFORGE_FSR4_CURRENT_BLEND_LINEAR", ("1",)),
    ("TFORGE_FSR4_CURRENT_BASE_JITTERED", ("1",)),
    ("TFORGE_FSR4_EXPERIMENTAL_BASE_UNJITTERED", ("1",)),
    ("TFORGE_FSR4_EXPERIMENTAL_DISABLE_POSTPASS_TAIL", ("1",)),
    ("TFORGE_FSR4_EXPERIMENTAL_RECOVERED_LINEAR_OUTPUT", ("1",)),
    ("TFORGE_FSR4_ENABLE_COLOR_HISTORY", ("1",)),
    ("TFORGE_FSR4_DISABLE_COLOR_HISTORY", ("1",)),
    ("TFORGE_FSR4_ENABLE_RECURRENT", ("1",)),
    ("TFORGE_FSR4_LEARNED_STRENGTH", ("0.25", "0.55", "0.75")),
    ("TFORGE_FSR4_LEARNED_CONFIDENCE_BLEND", ("0.25", "0.5", "0.75")),
    ("TFORGE_FSR4_DISABLE_LEARNED_CONFIDENCE_GATE", ("1",)),
    ("TFORGE_FSR4_CAS_STRENGTH", ("0.02", "0.05", "0.1", "0.2")),
    ("TFORGE_FSR4_LEGACY_RCAS_STRENGTH", ("0.02", "0.05", "0.1")),
    ("TFORGE_FSR4_FORCE_RESET", ("1",)),
    ("TFORGE_FSR4_EXPERIMENTAL_SINGLE_HISTORY_BLEND", ("1",)),
    ("TFORGE_FSR4_EXPERIMENTAL_LEGACY_ROUND", ("1",)),
    ("TFORGE_FSR4_EXPERIMENTAL_LEGACY_RECURRENT_BIAS", ("1",)),
    ("TFORGE_FSR4_LEARNED_KERNEL_RADIUS", ("0.75", "1.25")),
    ("TFORGE_FSR4_LEARNED_KERNEL_SIGMA", ("0.35", "1.25")),
    ("TFORGE_FSR4_LEARNED_KERNEL_EXPONENT", ("wide",)),
    ("TFORGE_FSR4_LEARNED_KERNEL_NORMALIZATION", ("legacy", "raw")),
    ("TFORGE_FSR4_POSTPASS_CURRENT_WEIGHT", ("0.25", "0.5", "0.75")),
    ("TFORGE_FSR4_POSTPASS_TAIL_MAPPING", ("swap",)),
    ("TFORGE_FSR4_POSTPASS_REVERSE_TAIL_CHANNELS", ("1",)),
    ("TFORGE_FSR4_EXPERIMENTAL_MOTION_SIGN", ("invert",)),
    ("TFORGE_FSR4_EXPERIMENTAL_MOTION_SCALE", ("0.5", "1.5")),
    ("TFORGE_FSR4_EXPERIMENTAL_MOTION_ROUNDING", ("floor", "ceil")),
    ("TFORGE_FSR4_EXPERIMENTAL_HISTORY_INTERPOLATION", ("bilinear", "nearest")),
    ("TFORGE_FSR4_MOTION_CONFIDENCE_REACTIVE", ("1",)),
    ("TFORGE_FSR4_EXPERIMENTAL_MOTION_MAX_BLOCKS", ("1", "2", "4", "8")),
    ("TFORGE_FSR4_EXPERIMENTAL_RECURRENT_RESET_ONLY", ("1",)),
    ("TFORGE_FSR4_INPUT_SHARPEN_STRENGTH", ("0.02", "0.05", "0.1")),
    ("TFORGE_FSR4_INPUT_TRANSFER", ("srgb", "rec709", "linear")),
    ("TFORGE_FSR4_CHROMA_FILTER", ("bilinear",)),
    ("TFORGE_FSR4_CHROMA_PHASE", ("center", "left", "top", "top-left")),
    ("TFORGE_FSR4_JITTER_SEQUENCE", ("halton23", "halton32", "alternating", "zero")),
    ("TFORGE_FSR4_JITTER_CADENCE", ("2", "8", "16")),
    ("TFORGE_FSR4_FP8_ROUNDING", ("nearest",)),
)


def slug(name: str, value: str) -> str:
    """Make a stable, filesystem-safe candidate label."""
    value = value.replace(".", "p").replace("-", "_")
    return f"{name.removeprefix('TFORGE_FSR4_').lower()}_{value}"


def candidate(candidate_id: str, environment: dict[str, str], source: str) -> dict[str, Any]:
    """Build one serializable candidate record."""
    if not SAFE.fullmatch(candidate_id):
        raise ValueError(f"unsafe candidate id: {candidate_id}")
    # Quality candidates must exercise the generic reconstructed path. The
    # native INT8 graph returns before the Quality Lab postpass and would make
    # declared composition/filter/postpass controls silently inert. Native
    # playback remains the separate no-overlay application path; this matrix
    # is specifically for measuring the configurable reconstruction path.
    measured_environment = {
        "TFORGE_FSR4_DISABLE_NATIVE_INT8": "1",
        **environment,
    }
    return {
        "id": candidate_id,
        "environment": measured_environment,
        "source": source,
    }


def quality_lab_candidates() -> list[dict[str, Any]]:
    """Discover valid qualityLab files without hardcoding every experiment."""
    results: list[dict[str, Any]] = []
    seen: set[str] = set()
    for path in sorted((ROOT / "benchmarks/quality_sweeps").rglob("*.json")):
        try:
            document = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            continue
        if not isinstance(document, dict) or not isinstance(document.get("qualityLab"), dict):
            continue
        quality_lab = document["qualityLab"]
        if "template" in path.name.lower() or not quality_lab.get("enabled", False):
            continue
        digest = hashlib.sha256(json.dumps(document, sort_keys=True).encode()).hexdigest()
        if digest in seen:
            continue
        seen.add(digest)
        relative = path.relative_to(ROOT).with_suffix("")
        candidate_id = "qualitylab_" + re.sub(r"[^A-Za-z0-9]+", "_", str(relative)).strip("_").lower()
        results.append(candidate(candidate_id, {"TFORGE_QUALITY_LAB_CONFIG": str(path)}, "qualityLab JSON"))
    return results


def runtime_candidates() -> list[dict[str, Any]]:
    """Expand every runtime dimension into isolated one-control arms."""
    results = [candidate("current_control", {}, "default control")]
    for name, values in RUNTIME_DIMENSIONS:
        for value in values:
            results.append(candidate(slug(name, value), {name: value}, "runtime control"))
    return results


def pairwise_candidates() -> list[dict[str, Any]]:
    """Expand valid two-control combinations for fast interaction screening."""
    results: list[dict[str, Any]] = []
    for left_index, (left_name, left_values) in enumerate(RUNTIME_DIMENSIONS):
        for right_name, right_values in RUNTIME_DIMENSIONS[left_index + 1 :]:
            for left_value in left_values:
                for right_value in right_values:
                    environment = {left_name: left_value, right_name: right_value}
                    # Mutually exclusive history switches are not valid assets.
                    if {
                        "TFORGE_FSR4_ENABLE_COLOR_HISTORY",
                        "TFORGE_FSR4_DISABLE_COLOR_HISTORY",
                    }.issubset(environment):
                        continue
                    candidate_id = "pair_" + slug(left_name, left_value) + "__" + slug(right_name, right_value)
                    results.append(candidate(candidate_id, environment, "runtime pairwise"))
    return results


def quality_lab_runtime_pairs() -> list[dict[str, Any]]:
    """Pair every discovered qualityLab asset with every runtime control."""
    results: list[dict[str, Any]] = []
    configs = quality_lab_candidates()
    for config in configs:
        config_environment = config["environment"]
        for name, values in RUNTIME_DIMENSIONS:
            for value in values:
                environment = dict(config_environment)
                environment[name] = value
                candidate_id = f"{config['id']}__{slug(name, value)}"
                results.append(candidate(candidate_id, environment, "qualityLab/runtime pairwise"))
    return results


def build_candidates(mode: str) -> list[dict[str, Any]]:
    """Return deterministic, duplicate-free candidates for the requested mode."""
    records: list[dict[str, Any]] = []
    if mode in {"isolated", "all"}:
        records += runtime_candidates() + quality_lab_candidates()
    if mode in {"pairwise", "all"}:
        records += pairwise_candidates() + quality_lab_runtime_pairs()
    unique: dict[str, dict[str, Any]] = {}
    for record in records:
        unique.setdefault(record["id"], record)
    return [unique[key] for key in sorted(unique)]


def clean_parent_environment(parent: dict[str, str]) -> dict[str, str]:
    """Remove capture-affecting variables so each candidate starts clean."""
    prefixes = ("TFORGE_FSR4_", "TFORGE_QUALITY_LAB_CONFIG", "TFORGE_BENCHMARK_")
    return {key: value for key, value in parent.items() if not key.startswith(prefixes)}


def parse_args() -> argparse.Namespace:
    """Parse planning and optional capture arguments."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=("isolated", "pairwise", "all"), default="all")
    parser.add_argument("--manifest-out", type=Path, required=True)
    parser.add_argument("--run", action="store_true", help="execute captures after writing the manifest")
    parser.add_argument("--player", type=Path)
    parser.add_argument("--input", type=Path)
    parser.add_argument("--reference", type=Path)
    parser.add_argument("--output-root", type=Path)
    parser.add_argument("--frames", type=int, default=8)
    parser.add_argument(
        "--workers",
        type=int,
        default=2,
        help="Maximum concurrent captures; two avoids CPU/GPU contention on the benchmark host.",
    )
    parser.add_argument(
        "--resume",
        action="store_true",
        help="skip candidates with a successful existing quality.csv/status.json",
    )
    parser.add_argument(
        "--retain-artifacts",
        action="store_true",
        help="retain per-frame PPM/MKV artifacts; default keeps only metrics and status",
    )
    return parser.parse_args()


def run_one(record: dict[str, Any], args: argparse.Namespace, parent: dict[str, str]) -> int:
    """Run one candidate in its own artifact directory."""
    destination = args.output_root / record["id"]
    if destination.exists():
        # A failed/interrupted row is disposable capture state.  Successful
        # rows are filtered before submission; this exact directory is the
        # only retry target and never contains source assets.
        shutil.rmtree(destination)
    destination.mkdir(parents=True, exist_ok=False)
    environment = clean_parent_environment(parent)
    environment.update(record["environment"])
    environment.update({
        "TFORGE_TEMPORAL_CANDIDATE_ID": record["id"],
        "TFORGE_QUALITY_TAG": "all_" + record["id"],
    })
    if args.retain_artifacts:
        environment["TFORGE_TEMPORAL_ARTIFACT_DIR"] = str(destination / "artifacts")
    result = destination / "quality.csv"
    command = [str(RUNNER), str(args.player), str(args.input), str(args.reference), str(result), str(args.frames)]
    completed = subprocess.run(command, cwd=ROOT, env=environment, check=False)
    (destination / "status.json").write_text(json.dumps({"id": record["id"], "returncode": completed.returncode}, indent=2) + "\n", encoding="utf-8")
    return completed.returncode


def main() -> int:
    """Write the complete candidate manifest and optionally execute it."""
    args = parse_args()
    if args.frames < 1 or args.workers < 1:
        raise SystemExit("frames and workers must be positive")
    args.workers, trackmania_active = guarded_worker_count(args.workers)
    if trackmania_active:
        print("Trackmania detected; running sample-producing swarm sequentially.", file=sys.stderr)
    records = build_candidates(args.mode)
    args.manifest_out.parent.mkdir(parents=True, exist_ok=True)
    args.manifest_out.write_text(json.dumps({"mode": args.mode, "candidates": records}, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"planned {len(records)} candidates: {args.manifest_out}")
    if not args.run:
        return 0
    if not all((args.player, args.input, args.reference, args.output_root)):
        raise SystemExit("--run requires --player, --input, --reference, and --output-root")
    if not args.player.is_file() or not os.access(args.player, os.X_OK):
        raise SystemExit(f"player is not executable: {args.player}")
    if args.output_root.exists() and any(args.output_root.iterdir()) and not args.resume:
        raise SystemExit(f"output root is not empty: {args.output_root}")
    args.output_root.mkdir(parents=True, exist_ok=True)
    if args.resume:
        records = [
            record for record in records
            if not (
                (args.output_root / record["id"] / "quality.csv").is_file()
                and (args.output_root / record["id"] / "quality.csv").stat().st_size > 0
                and (args.output_root / record["id"] / "status.json").is_file()
                and json.loads((args.output_root / record["id"] / "status.json").read_text(encoding="utf-8")).get("returncode") == 0
            )
        ]
        print(f"resume: {len(records)} candidates remain")
    parent = dict(os.environ)
    failures = 0
    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        futures = [pool.submit(run_one, record, args, parent) for record in records]
        for record, future in zip(records, futures):
            code = future.result()
            print(f"{record['id']}: {'ok' if code == 0 else 'failed'}")
            failures += code != 0
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
