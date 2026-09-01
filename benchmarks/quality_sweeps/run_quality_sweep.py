#!/usr/bin/env python3
# run_quality_sweep.py — orchestrate auditable, non-overwriting quality runs.
#
# Upstream: a checked-in candidate manifest and a build-fast player binary.
# Downstream: one run directory containing copied configs, capture CSVs, timing
# summaries, and deterministic rankings. It coordinates experiments only; the
# actual frame production remains in video_corpus/run_quality.sh.
"""Run reproducible, non-overwriting Temporal Forge quality sweeps.

The corpus runner owns frame capture and reference metrics. This wrapper adds
the experiment bookkeeping that makes a sweep auditable: a copied manifest,
the exact runtime config for every candidate, tagged corpus artifacts, parsed
GPU/pipeline timings, per-candidate CSV rows, and a deterministic ranking.

Example:
  ./benchmarks/quality_sweeps/run_quality_sweep.py \
      --manifest benchmarks/quality_sweeps/stage_a_manifest.json \
      --binary ./build-fast/temporal_forge_player \
      --output-root /tmp/tforge-quality-stageA-20260821/full
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import json
import os
import re
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
from statistics import fmean
from typing import Any

try:
    # Package import is used by the test suite and by callers importing the
    # sweep as a module.
    from .quality_lab_contract import (
        create_artifact_directory,
        inherited_image_settings,
        sha256_file,
    )
    from .paired_spatial_metrics import write_paired_reports
except ImportError:
    # Direct script execution puts this directory on sys.path instead of the
    # repository root, so retain a script-mode import path as well.
    from quality_lab_contract import (
        create_artifact_directory,
        inherited_image_settings,
        sha256_file,
    )
    from paired_spatial_metrics import write_paired_reports


try:
    from .trackmania_guard import guarded_worker_count
except ImportError:
    from trackmania_guard import guarded_worker_count


ROOT = Path(__file__).resolve().parents[2]
QUALITY_RUNNER = ROOT / "benchmarks" / "video_corpus" / "run_quality.sh"
TIMING_RE = re.compile(
    r"pipelineCPU=(?P<cpu>[0-9.]+)ms\s+"
    r"dispatchCPU\(all 1 passes\)=(?P<dispatch>[0-9.]+)ms\s+"
    r"GPU\(all 1 passes\)=(?P<gpu>[0-9.]+)ms"
)
SAFE_ID = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]*$")
CAPTURE_ENV_PREFIXES = (
    "TFORGE_QUALITY_",
    "TFORGE_FSR4_",
    "TFORGE_UPSCALE_",
    "TFORGE_JITTER_",
    "TFORGE_REVIEW_",
    "TFORGE_BENCHMARK_",
)
CAPTURE_ENV_KEYS = {
    "TFORGE_ALLOW_SPATIAL_TEMPORAL_CONTROL",
    "TFORGE_DISABLE_HW_DECODE",
}


def parse_args() -> argparse.Namespace:
    """Parse the sweep contract used by the command-line workflow."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--output-root", required=True, type=Path)
    parser.add_argument(
        "--tag-prefix",
        default="",
        help="Optional stable prefix; a UTC run id is appended to keep artifacts unique.",
    )
    parser.add_argument(
        "--continue-on-error",
        action="store_true",
        help="Record a failed candidate and continue with the remaining candidates.",
    )
    parser.add_argument(
        "--workers",
        type=int,
        default=max(1, int(os.environ.get("TFORGE_CAPTURE_WORKERS", "2"))),
        help="Maximum isolated capture processes to run at once (default: 2).",
    )
    parser.add_argument(
        "--retries",
        type=int,
        default=1,
        help="Number of isolated retry attempts for failed candidates (default: 1).",
    )
    return parser.parse_args()


def load_json(path: Path) -> dict[str, Any]:
    """Load and validate a manifest object before any candidate is run."""
    with path.open("r", encoding="utf-8") as stream:
        value = json.load(stream)
    if not isinstance(value, dict):
        raise ValueError(f"manifest must contain a JSON object: {path}")
    return value


def _is_capture_environment_key(name: str) -> bool:
    """Return whether one variable can alter capture pixels or identity."""
    return name in CAPTURE_ENV_KEYS or name.startswith(CAPTURE_ENV_PREFIXES)


def build_candidate_environment(
    parent: dict[str, str], manifest: dict[str, Any], candidate: dict[str, Any]
) -> dict[str, str]:
    """Build an isolated runtime environment from declared capture settings.

    Desktop, Vulkan, locale, and executable-path variables remain available,
    while every image/capture-affecting Temporal Forge variable is removed
    unless the campaign or candidate declares it explicitly.
    """
    environment = {
        key: value for key, value in parent.items() if not _is_capture_environment_key(key)
    }
    declared: dict[str, str] = {}
    for owner, source in (("campaign", manifest), ("candidate", candidate)):
        values = source.get("environment", {})
        if not isinstance(values, dict):
            raise ValueError(f"{owner} environment must be an object")
        for key, value in values.items():
            if not isinstance(key, str) or not _is_capture_environment_key(key):
                raise ValueError(f"unsupported candidate environment key: {key!r}")
            if not isinstance(value, str):
                raise ValueError(f"candidate environment value for {key} must be a string")
            declared[key] = value
    environment.update(declared)
    return environment


def resolve_from_root(path_value: str, base: Path) -> Path:
    """Resolve manifest-relative paths so runs are independent of cwd."""
    path = Path(path_value)
    return path if path.is_absolute() else (base / path).resolve()


def mean(values: list[float]) -> float | None:
    """Return a stable arithmetic mean while preserving empty-data semantics."""
    return fmean(values) if values else None


def summarize_csv(path: Path) -> dict[str, Any]:
    """Summarize captured quality metrics for one candidate's CSV."""
    with path.open("r", encoding="utf-8", newline="") as stream:
        rows = list(csv.DictReader(stream))
    numeric_fields = (
        "fsr_psnr_db",
        "fsr_ssim",
        "fsr_edge_ssim",
        "lanczos_ssim",
        "bicubic_ssim",
        "fsr_vs_lanczos_ssim_delta",
        "fsr_vs_bicubic_ssim_delta",
        "fsr_lowfreq_luma_mae",
        "fsr_lowfreq_luma_bias",
    )
    summary: dict[str, Any] = {"rowCount": len(rows), "clips": rows}
    for field in numeric_fields:
        values = [float(row[field]) for row in rows if row.get(field)]
        summary[f"mean_{field}"] = mean(values)
    return summary


def load_review_assets(path: Path) -> list[dict[str, Any]]:
    """Load the runner's full-frame inventory into campaign asset metadata."""
    if not path.is_file():
        raise FileNotFoundError(f"quality runner did not create asset manifest: {path}")
    with path.open("r", encoding="utf-8", newline="") as stream:
        rows = list(csv.DictReader(stream))
    assets: list[dict[str, Any]] = []
    for row in rows:
        try:
            asset = {
                "scene": row["scene"],
                "frame": int(row["frame"]),
                "path": row["path"],
                "width": int(row["width"]),
                "height": int(row["height"]),
            }
        except (KeyError, TypeError, ValueError) as error:
            raise ValueError(f"invalid review asset row in {path}: {row}") from error
        asset_path = Path(asset["path"])
        if not asset["scene"] or not asset_path.is_file() or asset_path.stat().st_size == 0:
            raise ValueError(f"missing full-frame review asset in {path}: {row}")
        if asset["width"] <= 0 or asset["height"] <= 0:
            raise ValueError(f"invalid review asset dimensions in {path}: {row}")
        assets.append(asset)
    if not assets:
        raise ValueError(f"quality runner produced no full-frame review assets: {path}")
    return assets


def summarize_timings(tag: str) -> dict[str, Any]:
    """Parse the last timing line for each tagged corpus log."""
    values: list[dict[str, float]] = []
    pattern = f"*_{tag}.log"
    for log_path in sorted(
        (ROOT / "benchmarks" / "video_corpus" / "results" / "quality_logs").glob(pattern)
    ):
        text = log_path.read_text(encoding="utf-8", errors="replace")
        matches = list(TIMING_RE.finditer(text))
        if not matches:
            continue
        match = matches[-1]
        values.append(
            {
                "log": str(log_path),
                "pipelineCpuMs": float(match.group("cpu")),
                "dispatchCpuMs": float(match.group("dispatch")),
                "gpuMs": float(match.group("gpu")),
            }
        )
    return {
        "rows": values,
        "meanPipelineCpuMs": mean([row["pipelineCpuMs"] for row in values]),
        "meanDispatchCpuMs": mean([row["dispatchCpuMs"] for row in values]),
        "meanGpuMs": mean([row["gpuMs"] for row in values]),
    }


def copy_config(config_path: Path, destination: Path) -> None:
    """Copy the exact config used by a candidate into its audit directory."""
    if not config_path.is_file():
        raise FileNotFoundError(f"quality config does not exist: {config_path}")
    destination.write_bytes(config_path.read_bytes())


def deep_merge(base: Any, overlay: Any) -> Any:
    """Return a JSON-compatible deep merge without mutating either input."""
    if isinstance(base, dict) and isinstance(overlay, dict):
        merged = dict(base)
        for key, value in overlay.items():
            merged[key] = deep_merge(merged[key], value) if key in merged else value
        return merged
    return overlay


def materialize_config(
    candidate: dict[str, Any], manifest_path: Path, destination: Path
) -> tuple[Path, str]:
    """Resolve a checked-in config or generate one from a template/override."""
    config_value = candidate.get("config")
    template_value = candidate.get("baseConfig")
    overrides = candidate.get("overrides", {})
    if config_value is not None and template_value is not None:
        raise ValueError("candidate cannot specify both config and baseConfig")
    if not isinstance(overrides, dict):
        raise ValueError("candidate overrides must be a JSON object")

    if template_value is not None:
        if not isinstance(template_value, str):
            raise ValueError("candidate baseConfig must be a path string")
        template_path = resolve_from_root(template_value, manifest_path.parent)
        generated = deep_merge(load_json(template_path), overrides)
        write_json(destination, generated)
        return destination, str(template_path)

    if not isinstance(config_value, str):
        raise ValueError("candidate is missing a config or baseConfig path")
    config_path = resolve_from_root(config_value, manifest_path.parent)
    copy_config(config_path, destination)
    return destination, str(config_path)


def write_json(path: Path, value: Any) -> None:
    """Write deterministic, human-readable audit metadata."""
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def run_candidate(
    candidate: dict[str, Any],
    *,
    manifest_path: Path,
    manifest: dict[str, Any],
    binary: Path,
    run_root: Path,
    run_id: str,
    index: int,
) -> dict[str, Any]:
    """Materialize, execute, and summarize one manifest candidate."""
    candidate_id = candidate.get("id")
    if not isinstance(candidate_id, str) or not SAFE_ID.fullmatch(candidate_id):
        raise ValueError(f"candidate id must match {SAFE_ID.pattern!r}: {candidate_id!r}")
    dimensions = manifest.get("dimensions", "426x240")
    frame = manifest.get("frame", 48)
    quality = manifest.get("quality", "high")
    clip_regex = manifest.get("clipRegex", "")
    preset = manifest.get("preset", "Quality")
    output_dimensions = candidate.get("outputDimensions", manifest.get("outputDimensions"))
    if output_dimensions is not None:
        if not isinstance(output_dimensions, str) or not re.fullmatch(r"[1-9][0-9]*x[1-9][0-9]*", output_dimensions):
            raise ValueError("candidate outputDimensions must be WIDTHxHEIGHT")
    jitter = manifest.get("jitter", {"mode": "current", "controlledStrength": 1.0})
    if not isinstance(jitter, dict):
        raise ValueError("manifest jitter must be an object")
    jitter_mode = jitter.get("mode", "current")
    jitter_strength = jitter.get("controlledStrength", 1.0)
    if jitter_mode not in {"off", "current", "reduced", "controlled"}:
        raise ValueError(f"unsupported manifest jitter mode: {jitter_mode!r}")
    if not isinstance(jitter_strength, (int, float)) or not 0.0 <= float(jitter_strength) <= 1.5:
        raise ValueError("manifest jitter controlledStrength must be in [0, 1.5]")
    if not isinstance(dimensions, str) or not isinstance(frame, int):
        raise ValueError("manifest dimensions must be a string and frame must be an integer")
    if not isinstance(quality, str) or not isinstance(clip_regex, str):
        raise ValueError("manifest quality and clipRegex must be strings")

    tag = f"{run_id}-{index:02d}-{candidate_id}"
    # Each attempt gets a numbered, non-overwriting directory.  This keeps a
    # rerun from replacing the evidence that explains an earlier result.
    # `run_root` already names the run; pass its parent so the helper creates
    # exactly `<output>/<run>/<candidate>/<attempt>` rather than duplicating
    # the run component.
    candidate_root = create_artifact_directory(run_root.parent, run_id, candidate_id, index)
    runtime_config_path, config_source = materialize_config(
        candidate, manifest_path, candidate_root / "quality_lab.json"
    )
    environment = build_candidate_environment(os.environ.copy(), manifest, candidate)
    environment.update(
        {
            "TFORGE_BENCHMARK_PRESET": preset,
            "TFORGE_QUALITY_LAB_CONFIG": str(runtime_config_path),
            "TFORGE_QUALITY_FRAME": str(frame),
            "TFORGE_QUALITY_TAG": tag,
            "TFORGE_QUALITY_QUALITY": quality,
            "TFORGE_QUALITY_CLIP": clip_regex,
            "TFORGE_FSR4_JITTER_MODE": str(jitter_mode),
            "TFORGE_FSR4_CONTROLLED_JITTER": str(jitter_strength),
        }
    )
    class_selections = manifest.get("classSelections")
    annotations_value = manifest.get("qualityClassAnnotationsPath")
    if class_selections is not None or annotations_value is not None:
        if not isinstance(class_selections, dict) or not isinstance(annotations_value, str) or not annotations_value:
            raise ValueError(
                "classSelections and qualityClassAnnotationsPath are both required for spatial capture"
            )
        annotation_path = resolve_from_root(annotations_value, ROOT)
        spatial_input_path = candidate_root / "spatial_capture_input.json"
        write_json(
            spatial_input_path,
            {
                "schema": "temporal_forge.spatial_capture.v1",
                "annotationsPath": str(annotation_path),
                "classSelections": class_selections,
                "frame": frame,
                "outputDimensions": output_dimensions,
                "evidenceMode": manifest.get("evidenceMode", "visual_and_metrics"),
            },
        )
        environment["TFORGE_QUALITY_SPATIAL_INPUT"] = str(spatial_input_path)
        if output_dimensions is None:
            raise ValueError("spatial capture requires outputDimensions")
        environment["TFORGE_QUALITY_OUTPUT_DIMENSIONS"] = output_dimensions
    if output_dimensions is not None:
        environment["TFORGE_FSR4_FORCE_VIEWPORT"] = output_dimensions
    exact_manifest = {
        "schemaVersion": 1,
        "runId": run_id,
        "candidateId": candidate_id,
        "tag": tag,
        "manifest": str(manifest_path),
        "configSource": config_source,
        "runtimeConfig": str(runtime_config_path),
        "overrides": candidate.get("overrides", {}),
        "dimensions": dimensions,
        "frame": frame,
        "quality": quality,
        "preset": preset,
        "clipRegex": clip_regex,
        "outputDimensions": output_dimensions,
        "jitter": {
            "mode": jitter_mode,
            "controlledStrength": float(jitter_strength),
        },
        "binary": str(binary),
        "binarySha256": sha256_file(binary),
        "configSha256": sha256_file(runtime_config_path),
        "imageAffectingEnvironment": inherited_image_settings(environment),
    }
    write_json(candidate_root / "experiment.json", exact_manifest)

    csv_path = candidate_root / "quality.csv"
    asset_manifest_path = candidate_root / "review_assets.csv"
    environment["TFORGE_QUALITY_ASSET_MANIFEST"] = str(asset_manifest_path)
    corpus_manifest = manifest.get("corpusManifest")
    if corpus_manifest is not None:
        if not isinstance(corpus_manifest, str):
            raise ValueError("manifest corpusManifest must be a path string")
        manifest_candidate = resolve_from_root(corpus_manifest, manifest_path.parent)
        if not manifest_candidate.is_file():
            # Campaign plans retain repository-relative evidence paths even
            # after the plan is copied beneath the non-overwriting run root.
            manifest_candidate = resolve_from_root(corpus_manifest, ROOT)
        if not manifest_candidate.is_file():
            raise FileNotFoundError(f"quality corpus manifest does not exist: {corpus_manifest}")
        environment["TFORGE_QUALITY_MANIFEST"] = str(manifest_candidate)
        exact_manifest["imageAffectingEnvironment"] = inherited_image_settings(environment)
        write_json(candidate_root / "experiment.json", exact_manifest)
    command = [str(QUALITY_RUNNER), str(binary), dimensions, str(csv_path)]
    completed = subprocess.run(
        command,
        cwd=ROOT,
        env=environment,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )
    (candidate_root / "runner.log").write_text(completed.stdout, encoding="utf-8")
    result: dict[str, Any] = dict(exact_manifest)
    result["command"] = command
    result["exitCode"] = completed.returncode
    result["csv"] = str(csv_path)
    if completed.returncode == 0 and csv_path.is_file():
        metrics = summarize_csv(csv_path)
        result["metrics"] = metrics
        if metrics["rowCount"] == 0:
            # A child can exit zero after swallowing a failed capture.  Empty
            # metrics are never a valid candidate result, so surface this as
            # a distinct failure instead of allowing a false green ranking.
            result["exitCode"] = 3
            result["error"] = "quality runner returned success but produced zero metric rows"
        else:
            result["timings"] = summarize_timings(tag)
            result["reviewAssets"] = load_review_assets(asset_manifest_path)
            result["representativeStillPaths"] = [
                asset["path"] for asset in result["reviewAssets"]
            ]
    else:
        result["error"] = "quality runner failed; see runner.log"
    write_json(candidate_root / "result.json", result)
    return result


def write_rankings(run_root: Path, results: list[dict[str, Any]]) -> None:
    """Write deterministic candidate rankings beside raw run artifacts."""
    rows: list[dict[str, Any]] = []
    for result in results:
        metrics = result.get("metrics", {})
        timings = result.get("timings", {})
        rows.append(
            {
                "candidateId": result["candidateId"],
                "exitCode": result["exitCode"],
                "rowCount": metrics.get("rowCount"),
                "meanFsrPsnrDb": metrics.get("mean_fsr_psnr_db"),
                "meanFsrSsim": metrics.get("mean_fsr_ssim"),
                "meanFsrEdgeSsim": metrics.get("mean_fsr_edge_ssim"),
                "meanLanczosSsim": metrics.get("mean_lanczos_ssim"),
                "meanBicubicSsim": metrics.get("mean_bicubic_ssim"),
                "meanLowfreqLumaMae": metrics.get("mean_fsr_lowfreq_luma_mae"),
                "meanLowfreqLumaBias": metrics.get("mean_fsr_lowfreq_luma_bias"),
                "meanGpuMs": timings.get("meanGpuMs"),
                "meanPipelineCpuMs": timings.get("meanPipelineCpuMs"),
            }
        )
    rows.sort(
        key=lambda row: (
            row["meanFsrSsim"] is not None,
            row["meanFsrSsim"] if row["meanFsrSsim"] is not None else float("-inf"),
            row["meanFsrEdgeSsim"] if row["meanFsrEdgeSsim"] is not None else float("-inf"),
        ),
        reverse=True,
    )
    with (run_root / "rankings.csv").open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]) if rows else ["candidateId"])
        writer.writeheader()
        writer.writerows(rows)
    write_json(run_root / "rankings.json", rows)


def main() -> int:
    """Run every manifest candidate and return a shell-friendly status code."""
    args = parse_args()
    if args.workers < 1 or args.retries < 0:
        raise ValueError("workers must be >= 1 and retries must be >= 0")
    args.workers, trackmania_active = guarded_worker_count(args.workers)
    if trackmania_active:
        print("Trackmania detected; running sample-producing sweep sequentially.", file=sys.stderr)
    manifest_path = args.manifest.resolve()
    binary = resolve_from_root(str(args.binary), ROOT)
    if not manifest_path.is_file():
        raise FileNotFoundError(manifest_path)
    if not binary.is_file():
        raise FileNotFoundError(binary)
    if not QUALITY_RUNNER.is_file():
        raise FileNotFoundError(QUALITY_RUNNER)

    manifest = load_json(manifest_path)
    candidates = manifest.get("experiments")
    if not isinstance(candidates, list) or not candidates:
        raise ValueError("manifest experiments must be a non-empty array")
    if args.output_root.exists():
        if any(args.output_root.iterdir()):
            raise FileExistsError(
                f"output root is not empty; refusing to overwrite prior sweep: {args.output_root}"
            )
    else:
        args.output_root.mkdir(parents=True)

    now = dt.datetime.now(dt.timezone.utc)
    timestamp = now.strftime("%Y%m%dT%H%M%SZ")
    prefix = args.tag_prefix.strip("-_")
    run_id = f"{prefix}-{timestamp}" if prefix else timestamp
    run_root = args.output_root / run_id
    run_root.mkdir()
    write_json(run_root / "manifest.json", manifest)
    shutil.copy2(manifest_path, run_root / "manifest.source.json")

    indexed_candidates = []
    for index, candidate in enumerate(candidates, start=1):
        if not isinstance(candidate, dict):
            raise ValueError(f"experiment {index} is not an object")
        indexed_candidates.append((index, candidate))

    def execute(item: tuple[int, dict[str, Any]], attempt: int = 0) -> dict[str, Any]:
        index, candidate = item
        # Retry indices are disjoint from first attempts, so a retry can never
        # overwrite evidence from the original capture.
        retry_index = index + attempt * len(indexed_candidates)
        try:
            return run_candidate(
                candidate,
                manifest_path=manifest_path,
                manifest=manifest,
                binary=binary,
                run_root=run_root,
                run_id=run_id,
                index=retry_index,
            )
        except Exception as error:  # noqa: BLE001 - preserve candidate failure evidence.
            candidate_id = str(candidate.get("id", f"experiment-{index}"))
            return {
                "runId": run_id,
                "candidateId": candidate_id,
                "exitCode": -1,
                "error": str(error),
                "attempt": attempt,
            }

    def run_batch(items: list[tuple[int, dict[str, Any]]], attempt: int) -> list[dict[str, Any]]:
        # Each candidate owns its config, output directory, tag, and runner
        # process. The pool only overlaps independent captures; it never
        # merges or reuses their evidence.
        with ThreadPoolExecutor(max_workers=args.workers) as pool:
            futures = {pool.submit(execute, item, attempt): item[0] for item in items}
            ordered = []
            for future in as_completed(futures):
                ordered.append((futures[future], future.result()))
        return [result for _, result in sorted(ordered)]

    results = run_batch(indexed_candidates, 0)
    for attempt in range(1, args.retries + 1):
        failed_ids = {result.get("candidateId") for result in results if result.get("exitCode") != 0}
        if not failed_ids:
            break
        retry_items = [item for item in indexed_candidates if item[1].get("id") in failed_ids]
        retry_results = run_batch(retry_items, attempt)
        retry_by_id = {result.get("candidateId"): result for result in retry_results}
        results = [retry_by_id.get(result.get("candidateId"), result) for result in results]
    write_json(run_root / "results.json", results)
    write_rankings(run_root, results)
    baseline_id = manifest.get("baselineCandidateId")
    if baseline_id is not None:
        if not isinstance(baseline_id, str) or not baseline_id:
            raise ValueError("manifest baselineCandidateId must be a non-empty string")
        write_paired_reports(run_root, results, baseline_id=baseline_id)
    print(run_root)
    return 0 if all(result.get("exitCode") == 0 for result in results) else 2


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (FileNotFoundError, FileExistsError, ValueError) as error:
        print(f"quality sweep error: {error}", file=sys.stderr)
        raise SystemExit(2)
