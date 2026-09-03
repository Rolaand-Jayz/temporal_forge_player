#!/usr/bin/env python3
"""Run the FSR reconstruction-scale experiment at one common final size.

The player is asked for the candidate's intermediate viewport.  Every
supersampled output is then reduced with the fixed first-pass filter before
metrics are computed, so intermediate pixels are never compared directly to
the final-resolution reference. ``--scale`` is a multiplier of the nominal 2x
delivery grid, not a source-relative scale; recorded intermediate dimensions
are authoritative.
"""
from __future__ import annotations

import argparse
import csv
import datetime as dt
import os
import re
import struct
import subprocess
import time
import uuid
import json
import hashlib
from pathlib import Path


SCALES = (2.00, 2.25, 2.50, 2.75, 3.00)
SCENES = ("tos_daylight", "tos_debris", "sintel_rooftop", "sintel_cave")


def even_dimension(value: int) -> int:
    """Return the nearest requested dimension accepted by the image path."""
    return value if value % 2 == 0 else value + 1


def fitted_dimensions(view_w: int, view_h: int, source_w: int, source_h: int) -> tuple[int, int]:
    """Match PlaybackEngine's source-aspect fit and even-pixel alignment."""
    fit = min(view_w / source_w, view_h / source_h)
    width = int(source_w * fit + 0.5)
    height = int(source_h * fit + 0.5)
    return even_dimension(width), even_dimension(height)


def run(command: list[str], *, env: dict[str, str], cwd: Path) -> None:
    subprocess.run(command, env=env, cwd=cwd, check=True)


def vram_used() -> int | None:
    for path in sorted(Path("/sys/class/drm").glob("card*/device/mem_info_vram_used")):
        try:
            total = int(path.with_name("mem_info_vram_total").read_text())
            if total > 1_000_000_000:
                return int(path.read_text())
        except (OSError, ValueError):
            pass
    return None


def run_player(command: list[str], *, env: dict[str, str], cwd: Path) -> tuple[int | None, int | None]:
    before = vram_used()
    process = subprocess.Popen(command, env=env, cwd=cwd)
    peak = before
    while process.poll() is None:
        used = vram_used()
        if used is not None and (peak is None or used > peak):
            peak = used
        time.sleep(0.05)
    used = vram_used()
    if used is not None and (peak is None or used > peak):
        peak = used
    if process.returncode:
        raise subprocess.CalledProcessError(process.returncode, command)
    return before, peak


def metric(label: str, path: Path) -> str:
    values = re.findall(rf"{re.escape(label)}:([-0-9.]+)", path.read_text(errors="replace"))
    if not values:
        raise RuntimeError(f"missing {label} in {path}")
    return values[-1]


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def png_dimensions(path: Path) -> tuple[int, int]:
    with path.open("rb") as stream:
        if stream.read(8) != b"\x89PNG\r\n\x1a\n":
            raise RuntimeError(f"reference cache entry is not a PNG: {path}")
        stream.seek(16)
        return struct.unpack(">II", stream.read(8))


def prune_image_payloads(root: Path) -> None:
    """Remove derived raster/video payloads while retaining campaign data."""
    suffixes = {".png", ".ppm", ".pgm", ".jpg", ".jpeg", ".mkv", ".mp4", ".webm"}
    for path in root.rglob("*"):
        if path.is_file() and path.suffix.lower() in suffixes:
            path.unlink()


def git_identity(root: Path) -> tuple[str, bool]:
    head = subprocess.run(
        ["git", "-C", str(root), "rev-parse", "HEAD"],
        cwd=root, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        check=True,
    ).stdout.strip()
    # The unified campaign publishes its own generated catalog and images as
    # routes finish. Exclude only those known outputs from subsequent arm
    # provenance checks; the top-level runner requires a clean tree before
    # the first route starts, and every source/config change remains visible.
    generated_pathspecs = (
        ":(exclude)review_harness/catalog.js",
        ":(exclude)review_harness/images/**",
    )
    worktree = subprocess.run(
        ["git", "-C", str(root), "diff", "--quiet", "--", ".", *generated_pathspecs],
        cwd=root, check=False,
    )
    staged = subprocess.run(
        ["git", "-C", str(root), "diff", "--cached", "--quiet"],
        cwd=root, check=False,
    )
    if worktree.returncode not in (0, 1) or staged.returncode not in (0, 1):
        raise RuntimeError("could not determine campaign git state")
    dirty = worktree.returncode == 1 or staged.returncode == 1
    return head, dirty


def validate_runtime_trace(path: Path, *, run_id: str, source: str,
                           output: str, scale: float,
                           cas_enabled: bool, binary_sha256: str,
                           git_head: str, git_dirty: bool,
                           profile: str, config_sha256: str,
                           expected_cas_strength: float | None = None) -> dict[str, object]:
    """Require the player to prove the effective state of this arm."""
    if not path.is_file():
        raise RuntimeError(f"Temporal Forge did not emit runtime trace: {path}")
    try:
        trace = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise RuntimeError(f"invalid runtime trace {path}: {error}") from error
    if not isinstance(trace, dict) or trace.get("schema") != "temporal_forge.runtime_pipeline.v1":
        raise RuntimeError(f"runtime trace schema mismatch: {path}")
    output_w, output_h = (int(value) for value in output.split("x"))
    source_w, source_h = (int(value) for value in source.split("x"))
    nominal_model_w = even_dimension(round(output_w / max(1.0, scale)))
    nominal_model_h = even_dimension(round(output_h / max(1.0, scale)))
    # Supersampling arms intentionally reconstruct on the requested nominal
    # grid even when it is larger than the decoded source. The player must not
    # clamp the model dimensions back to the decoder dimensions.
    expected_model_w = nominal_model_w
    expected_model_h = nominal_model_h
    expected_jitter = profile != "JITTER_OFF_DIAGNOSTIC"
    expected_jitter_mode = (
        "prepass_input_resolve" if profile == "AMD_SEMANTIC_BASELINE"
        else "off"
    )
    expected = {
        "run_id": run_id,
        "quality_profile": profile,
        "config_sha256": config_sha256,
        "source_resolution": source,
        "presentation_resolution": output,
        "requested_force_viewport": output,
        "requested_force_scale": f"{scale:.2f}",
        "jitter_mode": expected_jitter_mode,
        "jitter_enabled": expected_jitter,
        "cas_enabled": cas_enabled,
        "binary_sha256": binary_sha256,
        "git_head": git_head,
        "git_dirty": git_dirty,
    }
    for key, value in expected.items():
        if trace.get(key) != value:
            raise RuntimeError(
                f"runtime state mismatch for {key}: requested={value!r}, "
                f"effective={trace.get(key)!r}, trace={path}"
            )
    if expected_cas_strength is None:
        expected_cas_strength = 0.2 if cas_enabled else 0.0
    expected_cas_stage = "integrated_post_reconstruction" if cas_enabled else "none"
    if trace.get("cas_stage") != expected_cas_stage:
        raise RuntimeError(
            f"CAS stage mismatch: expected={expected_cas_stage!r}, "
            f"effective={trace.get('cas_stage')!r}, trace={path}"
        )
    trace_cas_strength = trace.get("cas_strength")
    if (not isinstance(trace_cas_strength, (int, float)) or
            abs(float(trace_cas_strength) - expected_cas_strength) > 0.001):
        raise RuntimeError(
            f"CAS strength mismatch: expected={expected_cas_strength:.2f}, "
            f"effective={trace_cas_strength!r}, trace={path}"
        )
    if profile == "AMD_SEMANTIC_BASELINE":
        required_baseline_state = {
            "prepass_source_tap_mulaw": True,
            "history_enabled": True,
            "recurrent_enabled": True,
            "motion_lookup": "unjittered_source_coordinates",
            "motion_validity_enabled": True,
        }
        for key, value in required_baseline_state.items():
            if trace.get(key) != value:
                raise RuntimeError(
                    f"AMD semantic baseline state mismatch for {key}: "
                    f"expected={value!r}, effective={trace.get(key)!r}, trace={path}"
                )
    reconstruction = trace.get("reconstruction_resolution")
    if not isinstance(reconstruction, str) or not re.fullmatch(r"[1-9][0-9]*x[1-9][0-9]*", reconstruction):
        raise RuntimeError(f"runtime trace has invalid reconstruction resolution: {path}")
    if trace.get("requested_model_resolution") != f"{nominal_model_w}x{nominal_model_h}":
        raise RuntimeError(f"requested model grid mismatch: {path}")
    if trace.get("scale_clamped_to_source") is not False:
        raise RuntimeError(f"unreported source-size scale clamp: {path}")
    if reconstruction != f"{expected_model_w}x{expected_model_h}":
        raise RuntimeError(
            f"effective model resolution mismatch: expected={expected_model_w}x{expected_model_h}, "
            f"effective={reconstruction!r}, trace={path}"
        )
    expected_motion_scale_x = expected_model_w / source_w
    expected_motion_scale_y = expected_model_h / source_h
    required_motion_state = {
        "motion_texture_resolution": f"{expected_model_w}x{expected_model_h}",
        "motion_units": "source_pixels",
        "motion_direction": "current_to_previous",
        "motion_coordinate_convention":
            "current_destination_plus_motion_previous_reference",
        "motion_sample_domain": "unjittered_source_coordinates",
        "motion_validity_distinct_from_zero": True,
        "invalid_history_policy": "reject_invalid_history",
    }
    for key, value in required_motion_state.items():
        if trace.get(key) != value:
            raise RuntimeError(
                f"runtime motion semantics mismatch for {key}: "
                f"expected={value!r}, effective={trace.get(key)!r}, trace={path}"
            )
    for key, expected_value in (("source_to_motion_scale_x", expected_motion_scale_x),
                                ("source_to_motion_scale_y", expected_motion_scale_y)):
        actual_value = trace.get(key)
        if not isinstance(actual_value, (int, float)) or abs(float(actual_value) - expected_value) > 0.001:
            raise RuntimeError(
                f"runtime motion transform mismatch for {key}: "
                f"expected={expected_value!r}, effective={actual_value!r}, trace={path}"
            )
    if trace.get("post_fsr_reducer") != "none":
        raise RuntimeError(f"unexpected post-FSR reducer: {path}")
    if trace.get("history_reset_policy") != "seek_resize_cut_or_invalid_correspondence":
        raise RuntimeError(f"runtime history reset policy mismatch: {path}")
    effective_scale = trace.get("effective_scale")
    expected_effective_scale = output_w / expected_model_w
    if not isinstance(effective_scale, (int, float)) or abs(float(effective_scale) - expected_effective_scale) > 0.01:
        raise RuntimeError(
            f"effective scale mismatch: expected={expected_effective_scale:.2f}, "
            f"effective={effective_scale!r}, trace={path}"
        )
    return trace


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--player", type=Path, default=Path("build-fast/temporal_forge_player"))
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--manifest", type=Path,
                        help="override the video-corpus manifest for controlled fixtures")
    parser.add_argument("--artifact-root", type=Path, required=True)
    parser.add_argument("--output-csv", type=Path, required=True)
    parser.add_argument("--reference-cache", type=Path,
                        help="pair-local cache for common final-resolution reference frames")
    parser.add_argument("--final", default="2560x1440")
    parser.add_argument("--scene", action="append", choices=SCENES + ("synthetic_edges_text", "bbb_branches"))
    parser.add_argument("--source", default="1280x720")
    parser.add_argument("--frame", type=int, default=48)
    parser.add_argument("--cas-strength", required=True,
                        help="explicit renderer-integrated CAS strength")
    parser.add_argument("--cas-placement", choices=("pre", "post", "none"), default="pre",
                        help="place CAS before reduction, after reduction, or disable it")
    parser.add_argument("--preset", default="saved",
                        help="benchmark preset forwarded to run_quality.sh")
    parser.add_argument("--profile", choices=("AMD_SEMANTIC_BASELINE", "JITTER_OFF_DIAGNOSTIC"),
                        default="AMD_SEMANTIC_BASELINE",
                        help="explicit semantic profile; diagnostics never become baseline evidence")
    parser.add_argument("--scale", type=float, action="append", choices=SCALES,
                        help="limit the run to selected reconstruction scales")
    args = parser.parse_args()
    final_w, final_h = (int(x) for x in args.final.split("x"))
    root = args.repo.resolve()
    player = args.player.resolve()
    artifact_root = args.artifact_root.resolve()
    artifact_root.mkdir(parents=True, exist_ok=True)
    reference_cache = args.reference_cache.resolve() if args.reference_cache else None
    if reference_cache:
        reference_cache.mkdir(parents=True, exist_ok=True)
    output_rows: list[dict[str, str]] = []
    git_head, git_dirty = git_identity(root)
    binary_sha256 = sha256_file(player)
    config_sha256 = sha256_file(root / "benchmarks/video_corpus/benchmark_settings.json")
    if args.scale:
        requested_scales = tuple(args.scale)
    elif args.preset == "NativeAA":
        # NativeAA is a fixed native-shape control, not five independent
        # multiplier arms. Repeating it under 2.25x..3.00x would create fake
        # identities for the same native output and fail effective-state
        # validation.
        requested_scales = (2.0,)
    else:
        requested_scales = SCALES
    preserve_images = os.environ.get("TFORGE_PRESERVE_SPATIAL_IMAGES", "1") == "1"

    manifest = (args.manifest or (root / "benchmarks/video_corpus/manifest.csv")).resolve()
    with manifest.open(newline="") as handle:
        rows = list(csv.DictReader(handle))
    source_w, source_h = (int(x) for x in args.source.split("x"))
    requested_scenes = tuple(args.scene) if args.scene else SCENES
    selected = {
        row["clip_id"]: row
        for row in rows
        if row["clip_id"] in requested_scenes and row["quality"] == "high"
        and row["width"] == str(source_w) and row["height"] == str(source_h)
    }
    if set(selected) != set(requested_scenes):
        raise RuntimeError(f"missing required scenes: {sorted(set(requested_scenes)-set(selected))}")

    for scale in requested_scales:
        requested_w = even_dimension(int(round(final_w * scale / 2.0)))
        requested_h = even_dimension(int(round(final_h * scale / 2.0)))
        candidate = f"scale_{scale:.2f}".replace(".", "_")
        candidate_root = artifact_root / candidate
        candidate_root.mkdir(parents=True, exist_ok=True)
        for scene in requested_scenes:
            row = selected[scene]
            intermediate_w, intermediate_h = fitted_dimensions(
                requested_w, requested_h, int(row["width"]), int(row["height"])
            )
            scene_root = candidate_root / scene
            scene_root.mkdir(parents=True, exist_ok=True)
            raw_csv = scene_root / "raw.csv"
            run_id = uuid.uuid4().hex
            runtime_trace = scene_root / "runtime_pipeline.json"
            env = os.environ.copy()
            env.update({
                "TFORGE_QUALITY_MANIFEST": str(manifest),
                "TFORGE_QUALITY_CLIP": rf"^{re.escape(scene)}$",
                "TFORGE_QUALITY_QUALITY": "high",
                "TFORGE_QUALITY_FRAME": str(args.frame),
                "TFORGE_QUALITY_TAG": candidate,
                "TFORGE_QUALITY_ARTIFACT_ROOT": str(scene_root),
                "TFORGE_QUALITY_OUTPUT_DIMENSIONS": f"{intermediate_w}x{intermediate_h}",
                "TFORGE_FSR4_FORCE_VIEWPORT": f"{intermediate_w}x{intermediate_h}",
                "TFORGE_FSR4_FORCE_SCALE": f"{scale:.2f}",
                "TFORGE_FSR4_JITTER_MODE": "synthetic" if args.profile == "AMD_SEMANTIC_BASELINE" else "off",
                "TFORGE_REVIEW_FSR_CAS": args.cas_strength,
                "TFORGE_FSR4_CAS_STRENGTH": args.cas_strength,
                "TFORGE_BENCHMARK_PRESET": args.preset,
                "TFORGE_DISABLE_HW_DECODE": "1",
                "TFORGE_FSR4_PROFILE_TIMINGS": "1",
                "TFORGE_EXPERIMENT_ID": run_id,
                "TFORGE_RUNTIME_TRACE_PATH": str(runtime_trace),
                "TFORGE_GIT_HEAD": git_head,
                "TFORGE_GIT_DIRTY": "1" if git_dirty else "0",
                "TFORGE_CONFIG_SHA256": config_sha256,
            })
            if args.profile == "AMD_SEMANTIC_BASELINE":
                env.update({
                    "TFORGE_QUALITY_PROFILE": args.profile,
                    "TFORGE_FSR4_INTEGRATED_BEST_FINDINGS": "1",
                    "TFORGE_FSR4_ENABLE_COLOR_HISTORY": "1",
                    "TFORGE_FSR4_ENABLE_RECURRENT": "1",
                    "TFORGE_FSR4_EXPERIMENTAL_SOURCE_TAP_MULAW": "1",
                })
            else:
                env["TFORGE_QUALITY_PROFILE"] = args.profile
            if args.cas_placement in ("post", "none"):
                env["TFORGE_FSR4_DISABLE_CAS"] = "1"
                env["TFORGE_REVIEW_FSR_CAS"] = ""
                env["TFORGE_FSR4_CAS_STRENGTH"] = "0.00"
            vram_before, vram_peak = run_player(
                [str(root / "benchmarks/video_corpus/run_quality.sh"), str(player),
                args.source, str(raw_csv)], env=env, cwd=root)
            with raw_csv.open(newline="") as handle:
                raw = next(csv.DictReader(handle))
            source = Path(raw["output_path"])
            try:
                trace = validate_runtime_trace(
                    runtime_trace,
                    run_id=run_id,
                    source=f"{row['width']}x{row['height']}",
                    output=f"{intermediate_w}x{intermediate_h}",
                    scale=scale,
                    cas_enabled=args.cas_placement == "pre",
                    expected_cas_strength=(float(args.cas_strength)
                                           if args.cas_placement == "pre" else 0.0),
                    binary_sha256=binary_sha256,
                    git_head=git_head,
                    git_dirty=git_dirty,
                    profile=args.profile,
                    config_sha256=config_sha256,
                )
            except Exception as error:
                # Persist rejected-arm provenance before propagating the
                # failure.  A failed experiment is still evidence; losing its
                # reason would make later audits indistinguishable from a
                # process that never ran.
                failed_record = {
                    "schema": "temporal_forge.quality_experiment.v2",
                    "experiment_id": run_id,
                    "run_id": run_id,
                    "arm_id": f"{args.preset.lower()}_{scale:.2f}x_{args.cas_placement}",
                    "profile": args.profile,
                    "status": "failed",
                    "failure_reason": str(error),
                    "timestamp_complete_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
                    "git_head": git_head,
                    "git_dirty_at_capture_start": git_dirty,
                    "binary": str(player),
                    "binary_sha256": binary_sha256,
                    "config_sha256": config_sha256,
                    "scene": scene,
                    "frame": args.frame,
                    "input_resolution": f"{row['width']}x{row['height']}",
                    "delivery_resolution": args.final,
                    "nominal_scale": f"{scale:.2f}",
                    "cas_strength": args.cas_strength if args.cas_placement != "none" else "0.00",
                    "cas_placement": args.cas_placement,
                    "runtime_trace_path": str(runtime_trace),
                }
                temporary_record = scene_root / f".experiment.{run_id}.tmp"
                temporary_record.write_text(json.dumps(failed_record, indent=2) + "\n", encoding="utf-8")
                os.replace(temporary_record, scene_root / "experiment.json")
                raise
            final_candidate = scene_root / "candidate_final.png"
            reference_final = (
                reference_cache / f"{scene}__frame-{args.frame:04d}__{final_w}x{final_h}.png"
                if reference_cache else scene_root / "reference_final.png"
            )
            reference = Path(raw["control_source_path"]).parent / (
                f"{scene}_reference_{intermediate_w}x{intermediate_h}_f{args.frame}_{candidate}.png"
            )
            # The quality runner's reference is a scaled version of the
            # lossless 2160p master. Regenerate the common final reference
            # directly from that master for an identical reference across arms.
            if reference_final.is_file():
                if png_dimensions(reference_final) != (final_w, final_h):
                    raise RuntimeError(f"reference cache dimensions mismatch: {reference_final}")
            else:
                temporary_reference = reference_final.with_name(
                    f".{reference_final.name}.{os.getpid()}.tmp.png"
                )
                run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
                     "-i", row["reference_path"],
                     "-vf", f"select=eq(n\\,{args.frame}),scale={final_w}:{final_h}:flags=lanczos",
                     "-frames:v", "1", str(temporary_reference)], env=env, cwd=root)
                os.replace(temporary_reference, reference_final)
            reduction = "lanczos" if scale > 2.0 else "fit"
            source_dimensions = (int(row["width"]), int(row["height"]))
            needs_final_fit = source_dimensions[0] * final_h != source_dimensions[1] * final_w
            if scale > 2.0 or needs_final_fit:
                run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", str(source),
                     "-vf", f"scale={final_w}:{final_h}:flags=lanczos", "-frames:v", "1",
                     str(final_candidate)], env=env, cwd=root)
            else:
                run(["cp", str(source), str(final_candidate)], env=env, cwd=root)
            if args.cas_placement == "post":
                post_cas = scene_root / "candidate_post_cas.png"
                run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", str(final_candidate),
                     "-vf", f"cas=strength={args.cas_strength}", "-frames:v", "1", str(post_cas)],
                    env=env, cwd=root)
                run(["cp", str(post_cas), str(final_candidate)], env=env, cwd=root)
            metric_log = scene_root / "final_metrics.log"
            edge_log = scene_root / "final_edge_metrics.log"
            with metric_log.open("w") as out:
                subprocess.run(["ffmpeg", "-hide_banner", "-i", str(final_candidate), "-i", str(reference_final),
                                "-lavfi", "[0:v][1:v]psnr;[0:v][1:v]ssim", "-f", "null", "-"],
                               env={**env, "LC_ALL": "C"}, cwd=root, stdout=subprocess.DEVNULL,
                               stderr=out, check=True)
            with edge_log.open("w") as out:
                subprocess.run(["ffmpeg", "-hide_banner", "-i", str(final_candidate), "-i", str(reference_final),
                                "-filter_complex", "[0:v]format=gray,edgedetect=low=0.05:high=0.15[a];[1:v]format=gray,edgedetect=low=0.05:high=0.15[b];[a][b]ssim", "-f", "null", "-"],
                               env={**env, "LC_ALL": "C"}, cwd=root, stdout=subprocess.DEVNULL,
                               stderr=out, check=True)
            timings = list((scene_root / "quality_logs").glob("*.log"))
            stage = next((p for p in timings if "stage-timing" in p.read_text(errors="replace")), None)
            stage_text = stage.read_text(errors="replace") if stage else ""
            gpu = re.findall(r"GPU=([0-9.]+)ms", stage_text)
            output_rows.append({
                "scene": scene, "scale": f"{scale:.2f}",
                "cas_strength": args.cas_strength if args.cas_placement != "none" else "0.00",
                "source_resolution": f"{row['width']}x{row['height']}",
                "intermediate_resolution": f"{intermediate_w}x{intermediate_h}",
                "final_resolution": args.final, "downsample": reduction,
                "psnr_db": metric("average", metric_log), "ssim": metric("All", metric_log),
                "edge_ssim": metric("All", edge_log), "gpu_ms_mean": gpu[-1] if gpu else "",
                "source_sha256": raw["control_source_sha256"],
                "binary": str(player), "frame": str(args.frame),
                "vram_before_bytes": str(vram_before or ""),
                "vram_peak_bytes": str(vram_peak or ""),
                "run_id": run_id,
                "runtime_trace_path": str(runtime_trace),
                "runtime_trace_sha256": hashlib.sha256(runtime_trace.read_bytes()).hexdigest(),
                "output_sha256": sha256_file(final_candidate),
                "runtime_effective_backend": str(trace.get("backend", "")),
                "runtime_effective_model_resolution": str(trace.get("reconstruction_resolution", "")),
            })
            experiment_record = {
                "schema": "temporal_forge.quality_experiment.v2",
                "experiment_id": run_id,
                "run_id": run_id,
                "arm_id": (
                    f"{args.preset.lower()}_{scale:.2f}x_"
                    f"{('cas20_' + args.cas_placement) if args.cas_placement != 'none' else 'no_cas'}"
                ),
                "profile": args.profile,
                "status": "complete",
                "timestamp_complete_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
                "git_head": git_head,
                "git_dirty_at_capture_start": git_dirty,
                "binary": str(player),
                "binary_sha256": binary_sha256,
                "config_sha256": config_sha256,
                "scene": scene,
                "frame": args.frame,
                "input_resolution": f"{row['width']}x{row['height']}",
                "reconstruction_resolution": f"{intermediate_w}x{intermediate_h}",
                "delivery_resolution": args.final,
                "nominal_scale": f"{scale:.2f}",
                "cas_strength": args.cas_strength if args.cas_placement != "none" else "0.00",
                "cas_placement": args.cas_placement,
                "source_artifact": raw.get("control_source_path", ""),
                "source_sha256": raw.get("control_source_sha256", ""),
                "output_artifact": str(final_candidate),
                "output_sha256": sha256_file(final_candidate),
                "output_retained": preserve_images,
                "runtime_trace": trace,
                "runtime_trace_path": str(runtime_trace),
                "runtime_trace_sha256": sha256_file(runtime_trace),
                "metrics": {
                    "psnr_db": metric("average", metric_log),
                    "ssim": metric("All", metric_log),
                    "edge_ssim": metric("All", edge_log),
                },
                "command": [str(root / "benchmarks/video_corpus/run_quality.sh"), str(player), args.source, str(raw_csv)],
                "requested_environment": {
                    key: env[key] for key in env
                    if key.startswith("TFORGE_") and key in {
                        "TFORGE_QUALITY_MANIFEST", "TFORGE_QUALITY_CLIP",
                        "TFORGE_QUALITY_FRAME", "TFORGE_QUALITY_TAG",
                        "TFORGE_QUALITY_OUTPUT_DIMENSIONS", "TFORGE_FSR4_FORCE_VIEWPORT",
                        "TFORGE_QUALITY_PROFILE",
                        "TFORGE_FSR4_FORCE_SCALE", "TFORGE_FSR4_JITTER_MODE",
                        "TFORGE_REVIEW_FSR_CAS", "TFORGE_FSR4_CAS_STRENGTH",
                        "TFORGE_FSR4_DISABLE_CAS", "TFORGE_EXPERIMENT_ID",
                        "TFORGE_RUNTIME_TRACE_PATH",
                        "TFORGE_GIT_HEAD", "TFORGE_GIT_DIRTY",
                        "TFORGE_CONFIG_SHA256",
                    }
                },
            }
            temporary_record = scene_root / f".experiment.{run_id}.tmp"
            temporary_record.write_text(json.dumps(experiment_record, indent=2) + "\n", encoding="utf-8")
            os.replace(temporary_record, scene_root / "experiment.json")
            if not preserve_images:
                # Metrics, hashes, runtime traces, logs, and experiment.json
                # are the durable campaign evidence. The rendered payloads
                # are disposable measurement intermediates in data-only mode.
                prune_image_payloads(scene_root)

    args.output_csv.parent.mkdir(parents=True, exist_ok=True)
    fields = list(output_rows[0])
    with args.output_csv.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(output_rows)
    print(args.output_csv)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
