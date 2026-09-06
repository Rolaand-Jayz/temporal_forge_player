#!/usr/bin/env python3
"""Run and validate the five-case AMD semantic baseline qualification.

The runner is deliberately serial.  It keeps the renderer's temporary image
inputs only for the duration of measurement and retains the resulting CSV,
event trace, runtime trace, hashes, and provenance manifest.
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import subprocess
import tempfile
import uuid
from pathlib import Path

import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from benchmarks.quality_sweeps.run_fsr_supersampling import validate_runtime_trace

ROOT = Path(__file__).resolve().parents[1]
FRAMES = 8
OUTPUT = "852x480"
CASES = {
    "static_high_detail": ("synthetic_edges_text_426x240_high_crf12.mp4", False),
    "smooth_camera_pan": ("tos_daylight_426x240_high_crf12.mp4", False),
    "independent_motion": ("synthetic_motion_426x240_high_crf12.mp4", False),
    "occlusion_disocclusion": ("tos_debris_426x240_high_crf12.mp4", False),
    "hard_scene_cut": (None, True),
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def identity(root: Path, player: Path) -> tuple[str, bool, str, str]:
    head = subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"],
                          check=True, text=True, stdout=subprocess.PIPE).stdout.strip()
    dirty = bool(subprocess.run(["git", "-C", str(root), "status", "--porcelain"],
                                check=True, text=True, stdout=subprocess.PIPE).stdout.strip())
    config = root / "benchmarks/video_corpus/benchmark_settings.json"
    return head, dirty, sha256(player), sha256(config)


def make_cut(path: Path, first: Path, second: Path) -> None:
    subprocess.run([
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
        "-i", str(first), "-i", str(second),
        "-filter_complex",
        "[0:v]select='lte(n,3)',setpts=N/24/TB[a];"
        "[1:v]select='lte(n,3)',setpts=N/24/TB[b];"
        "[a][b]concat=n=2:v=1:a=0,format=yuv420p[v]",
        "-map", "[v]", "-frames:v", str(FRAMES), "-r", "24",
        "-c:v", "ffv1", "-level", "3", "-g", "1", str(path),
    ], check=True, cwd=ROOT)


def read_events(path: Path) -> list[dict[str, object]]:
    document = json.loads(path.read_text(encoding="utf-8"))
    if document.get("schema") != "temporal_forge.event_trace.v1":
        raise RuntimeError(f"event trace schema mismatch: {path}")
    frames = document.get("frames")
    if not isinstance(frames, list) or len(frames) != FRAMES:
        raise RuntimeError(f"event trace frame count mismatch: {path}")
    return [frame for frame in frames if isinstance(frame, dict)]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--player", type=Path, default=Path("build-fast/temporal_forge_player"))
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--frames", type=int, default=FRAMES)
    args = parser.parse_args()
    if args.frames != FRAMES:
        raise SystemExit(f"qualification requires exactly {FRAMES} frames")

    root = ROOT.resolve()
    player = args.player.resolve()
    if not player.is_file() or not os.access(player, os.X_OK):
        raise SystemExit(f"player is not executable: {player}")
    output_root = args.output_root.resolve()
    if output_root.exists() and any(output_root.iterdir()):
        raise SystemExit(f"refusing to overwrite qualification data: {output_root}")
    output_root.mkdir(parents=True, exist_ok=True)
    head, dirty, binary_sha, config_sha = identity(root, player)
    source_dir = root / "benchmarks/video_corpus/clips"
    reference_dir = root / "benchmarks/video_corpus/references"
    temporal_config = root / (
        "benchmarks/quality_sweeps/swarm/agent_composition_audit/current_control.json"
    )
    if not temporal_config.is_file():
        raise SystemExit(f"missing temporal qualification config: {temporal_config}")

    records: list[dict[str, object]] = []
    with tempfile.TemporaryDirectory(prefix="tforge-qualification-") as temp_name:
        temp = Path(temp_name)
        cut = temp / "hard_scene_cut.mkv"
        cut_reference = temp / "hard_scene_cut_reference.mkv"
        make_cut(cut, source_dir / CASES["static_high_detail"][0],
                 source_dir / "synthetic_dark_426x240_high_crf12.mp4")
        make_cut(
            cut_reference,
            reference_dir / "synthetic_edges_text_2160p_lossless.mkv",
            reference_dir / "synthetic_dark_2160p_lossless.mkv",
        )
        for case, (filename, is_cut) in CASES.items():
            case_root = output_root / case
            case_root.mkdir()
            source = cut if is_cut else source_dir / str(filename)
            event_path = case_root / "event_trace.json"
            runtime_path = case_root / "runtime_pipeline.json"
            csv_path = case_root / "quality.csv"
            run_id = uuid.uuid4().hex
            if is_cut:
                reference = cut_reference
            else:
                source_id = str(filename).split("_426x240_", 1)[0]
                reference = reference_dir / f"{source_id}_2160p_lossless.mkv"
            if not reference.is_file():
                raise SystemExit(f"missing matched reference: {reference}")
            env = os.environ.copy()
            env.update({
                "TFORGE_TEMPORAL_ARTIFACT_DIR": str(case_root / "artifacts"),
                "TFORGE_TEMPORAL_EVENTS_JSON": str(event_path),
                "TFORGE_RUNTIME_TRACE_PATH": str(runtime_path),
                "TFORGE_QUALITY_PROFILE": "AMD_SEMANTIC_BASELINE",
                "TFORGE_EXPERIMENT_ID": run_id,
                "TFORGE_GIT_HEAD": head,
                "TFORGE_GIT_DIRTY": "1" if dirty else "0",
                "TFORGE_CONFIG_SHA256": config_sha,
                "TFORGE_FSR4_JITTER_MODE": "synthetic",
                "TFORGE_FSR4_FORCE_VIEWPORT": OUTPUT,
                "TFORGE_FSR4_FORCE_SCALE": "2.00",
                "TFORGE_FSR4_CAS_STRENGTH": "0.20",
                "TFORGE_FSR4_INTEGRATED_BEST_FINDINGS": "1",
                "TFORGE_FSR4_ENABLE_COLOR_HISTORY": "1",
                "TFORGE_FSR4_ENABLE_RECURRENT": "1",
                "TFORGE_FSR4_EXPERIMENTAL_SOURCE_TAP_MULAW": "1",
                "TFORGE_TEMPORAL_CANDIDATE_ID": "AMD_SEMANTIC_BASELINE",
                "TFORGE_TEMPORAL_SCENE": case,
                "TFORGE_TEMPORAL_CLASS": case,
                "TFORGE_TEMPORAL_CONFIG_ID": "AMD_SEMANTIC_BASELINE",
                "TFORGE_TEMPORAL_START_FRAME": "0",
                "TFORGE_TEMPORAL_ANALYSIS_FRAME_INDICES": "0,1,2,3,4,5,6,7",
                "TFORGE_TEMPORAL_GHOST_THRESHOLD": "0.10",
                "TFORGE_TEMPORAL_RESET_THRESHOLD": "0.10",
                "TFORGE_TEMPORAL_ALLOW_NO_EVENT": "1" if not is_cut else "",
                "TFORGE_PRESERVE_IMAGE_ARTIFACTS": "0",
                "TFORGE_DISABLE_HW_DECODE": "1",
                "TFORGE_QUALITY_LAB_CONFIG": str(temporal_config),
            })
            subprocess.run([
                str(root / "benchmarks/video_corpus/run_temporal_quality.sh"),
                str(player), str(source), str(reference), str(csv_path), str(FRAMES),
            ], cwd=root, env=env, check=True)
            trace = validate_runtime_trace(
                runtime_path, run_id=run_id, source="426x240", output=OUTPUT,
                scale=2.0, cas_enabled=True, binary_sha256=binary_sha,
                git_head=head, git_dirty=dirty, profile="AMD_SEMANTIC_BASELINE",
                config_sha256=config_sha,
            )
            events = read_events(event_path)
            if not any(bool(frame.get("jitterApplied")) for frame in events[1:]):
                raise RuntimeError(f"qualification case has no changing jitter evidence: {case}")
            if is_cut and not any(bool(frame.get("event")) and bool(frame.get("reset"))
                                   for frame in events[1:]):
                raise RuntimeError("hard scene cut did not produce a recorded reset")
            records.append({
                "case": case, "status": "passed", "experiment_id": run_id,
                "source": str(source), "source_sha256": sha256(source),
                "reference": str(reference), "reference_sha256": sha256(reference),
                "runtime_trace": str(runtime_path), "runtime_trace_sha256": sha256(runtime_path),
                "event_trace": str(event_path), "event_trace_sha256": sha256(event_path),
                "metrics": str(csv_path), "metrics_sha256": sha256(csv_path),
                "frames": FRAMES, "output_resolution": OUTPUT,
                "quality_profile": trace["quality_profile"],
            })
    manifest = {
        "schema": "temporal_forge.baseline_qualification.v1",
        "created_at": dt.datetime.now(dt.timezone.utc).isoformat(),
        "binary": str(player), "binary_sha256": binary_sha,
        "git_head": head, "git_dirty": dirty, "config_sha256": config_sha,
        "image_retention": "data_only",
        "cases": records,
    }
    (output_root / "qualification_manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(output_root)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
