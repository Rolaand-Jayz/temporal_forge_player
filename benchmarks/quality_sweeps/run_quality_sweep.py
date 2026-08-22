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
from pathlib import Path
from statistics import fmean
from typing import Any


ROOT = Path(__file__).resolve().parents[2]
QUALITY_RUNNER = ROOT / "benchmarks" / "video_corpus" / "run_quality.sh"
TIMING_RE = re.compile(
    r"pipelineCPU=(?P<cpu>[0-9.]+)ms\s+"
    r"dispatchCPU\(all 1 passes\)=(?P<dispatch>[0-9.]+)ms\s+"
    r"GPU\(all 1 passes\)=(?P<gpu>[0-9.]+)ms"
)
SAFE_ID = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]*$")


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
    return parser.parse_args()


def load_json(path: Path) -> dict[str, Any]:
    """Load and validate a manifest object before any candidate is run."""
    with path.open("r", encoding="utf-8") as stream:
        value = json.load(stream)
    if not isinstance(value, dict):
        raise ValueError(f"manifest must contain a JSON object: {path}")
    return value


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
    if not isinstance(dimensions, str) or not isinstance(frame, int):
        raise ValueError("manifest dimensions must be a string and frame must be an integer")
    if not isinstance(quality, str) or not isinstance(clip_regex, str):
        raise ValueError("manifest quality and clipRegex must be strings")

    tag = f"{run_id}-{index:02d}-{candidate_id}"
    candidate_root = run_root / candidate_id
    candidate_root.mkdir()
    runtime_config_path, config_source = materialize_config(
        candidate, manifest_path, candidate_root / "quality_lab.json"
    )
    exact_manifest = {
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
        "binary": str(binary),
    }
    write_json(candidate_root / "experiment.json", exact_manifest)

    csv_path = candidate_root / "quality.csv"
    env = os.environ.copy()
    env.update(
        {
            "TFORGE_BENCHMARK_PRESET": preset,
            "TFORGE_QUALITY_LAB_CONFIG": str(runtime_config_path),
            "TFORGE_QUALITY_FRAME": str(frame),
            "TFORGE_QUALITY_TAG": tag,
            "TFORGE_QUALITY_QUALITY": quality,
            "TFORGE_QUALITY_CLIP": clip_regex,
        }
    )
    corpus_manifest = manifest.get("corpusManifest")
    if corpus_manifest is not None:
        if not isinstance(corpus_manifest, str):
            raise ValueError("manifest corpusManifest must be a path string")
        env["TFORGE_QUALITY_MANIFEST"] = str(
            resolve_from_root(corpus_manifest, manifest_path.parent)
        )
    command = [str(QUALITY_RUNNER), str(binary), dimensions, str(csv_path)]
    completed = subprocess.run(
        command,
        cwd=ROOT,
        env=env,
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
        result["metrics"] = summarize_csv(csv_path)
        result["timings"] = summarize_timings(tag)
        result["representativeStillPaths"] = [
            row["output_path"]
            for row in result["metrics"]["clips"]
            if row.get("output_path")
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

    results: list[dict[str, Any]] = []
    for index, candidate in enumerate(candidates, start=1):
        if not isinstance(candidate, dict):
            raise ValueError(f"experiment {index} is not an object")
        try:
            result = run_candidate(
                candidate,
                manifest_path=manifest_path,
                manifest=manifest,
                binary=binary,
                run_root=run_root,
                run_id=run_id,
                index=index,
            )
        except Exception as error:  # noqa: BLE001 - preserve candidate failure evidence.
            candidate_id = str(candidate.get("id", f"experiment-{index}"))
            result = {
                "runId": run_id,
                "candidateId": candidate_id,
                "exitCode": -1,
                "error": str(error),
            }
            candidate_root = run_root / candidate_id
            candidate_root.mkdir(exist_ok=True)
            write_json(candidate_root / "result.json", result)
            if not args.continue_on_error:
                write_json(run_root / "results.json", results + [result])
                raise
        results.append(result)
    write_json(run_root / "results.json", results)
    write_rankings(run_root, results)
    print(run_root)
    return 0 if all(result.get("exitCode") == 0 for result in results) else 2


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (FileNotFoundError, FileExistsError, ValueError) as error:
        print(f"quality sweep error: {error}", file=sys.stderr)
        raise SystemExit(2)
