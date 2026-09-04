#!/usr/bin/env python3
"""Run the complete Temporal Forge motion-input evidence matrix.

The runner is intentionally data/manifest driven.  It never edits campaign
evidence and defaults to a dry run; use --run to start captures.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CORPUS = ROOT / "benchmarks" / "video_corpus"
TIERS = {"426x240": "240", "640x360": "360", "1280x720": "720", "1920x1080": "1080"}
SCENES = ("tos_daylight", "tos_debris", "sintel_rooftop", "sintel_cave")
ARMS = {
    "zero": {"TFORGE_FSR4_MOTION_ABLATION": "zero"},
    "autocheap": {},
    "codec": {"TFORGE_FSR4_MOTION_ABLATION": "codec"},
    "refined": {"TFORGE_FSR4_MOTION_ABLATION": "refined"},
    "dense_grid_edge": {"TFORGE_FSR4_MOTION_DENSE_GRID": "1", "TFORGE_FSR4_MOTION_EDGE_AWARE": "1"},
    "offline_dense": {},  # sidecar is supplied per capture below
}
# Keep the sweep small enough to answer the causal question while covering
# both normal and deliberately strict history consumption.
CONFIDENCE_SWEEP = (None, "0.35", "0.75")


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def reference(scene: str) -> tuple[Path, str]:
    p = CORPUS / "references" / f"{scene}_2160p_lossless.mkv"
    if p.stat().st_size < 4096:
        # The checked-in cave reference is a known invalid placeholder.  Use
        # the highest-quality matching decode and record that fact explicitly.
        p = CORPUS / "clips" / f"{scene}_1920x1080_high_crf12.mp4"
        return p, "matching_1920x1080_high_crf12_fallback"
    return p, "2160p_lossless"


def plans(args: argparse.Namespace):
    scenes = args.scenes or SCENES
    tiers = args.tiers or tuple(TIERS)
    arms = args.arms or tuple(ARMS)
    for scene in scenes:
        for tier in tiers:
            inp = CORPUS / "clips" / f"{scene}_{tier}_high_crf12.mp4"
            ref, ref_kind = reference(scene)
            if not inp.is_file() or not ref.is_file():
                raise FileNotFoundError(f"missing corpus input/reference: {inp} / {ref}")
            for arm in arms:
                confs = CONFIDENCE_SWEEP if args.confidence else (None,)
                for conf in confs:
                    yield scene, tier, arm, conf, inp, ref, ref_kind


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--run", action="store_true", help="execute captures (default is dry-run)")
    ap.add_argument("--output-root", type=Path, default=ROOT / "benchmarks" / "quality_sweeps" / "motion_campaign")
    ap.add_argument("--player", type=Path, default=ROOT / "build-fast" / "temporal_forge_player")
    ap.add_argument("--config", type=Path, default=ROOT / "benchmarks" / "quality_sweeps" / "swarm" / "agent_recheck_current" / "config.json")
    ap.add_argument("--frames", type=int, default=8)
    ap.add_argument("--warmup", type=int, default=8)
    # Full-resolution decode/readback can exceed three minutes on this
    # workstation; keep the timeout a guard, not a hidden resolution filter.
    ap.add_argument("--timeout", type=int, default=600)
    ap.add_argument("--no-confidence", dest="confidence", action="store_false", help="only run default confidence")
    ap.set_defaults(confidence=True)
    ap.add_argument("--scenes", nargs="*", choices=SCENES)
    ap.add_argument("--tiers", nargs="*", choices=tuple(TIERS))
    ap.add_argument("--arms", nargs="*", choices=tuple(ARMS))
    args = ap.parse_args()
    if not args.player.is_file():
        ap.error(f"player not found: {args.player}")
    if not args.config.is_file():
        ap.error(f"quality config not found: {args.config}")
    args.output_root.mkdir(parents=True, exist_ok=True)
    manifest = args.output_root / "manifest.jsonl"
    count = 0
    for scene, tier, arm, conf, inp, ref, ref_kind in plans(args):
        count += 1
        key = f"{scene}/{tier}/{arm}/confidence-{conf or 'default'}"
        run_dir = args.output_root / scene / tier / arm / f"confidence-{conf or 'default'}"
        row = {"key": key, "scene": scene, "input_resolution": tier, "output_resolution": "1920x1080",
               "arm": arm, "confidence_threshold": conf, "input": str(inp), "reference": str(ref),
               "reference_kind": ref_kind, "input_sha256": sha256(inp), "reference_sha256": sha256(ref),
               "status": "planned"}
        if args.run:
            run_dir.mkdir(parents=True, exist_ok=True)
            csv = run_dir / "quality.csv"
            env = os.environ.copy()
            env.update({"TFORGE_QUALITY_LAB_CONFIG": str(args.config), "TFORGE_QUALITY_PROFILE": "AMD_SEMANTIC_BASELINE",
                        "TFORGE_BENCHMARK_PRESET": "Quality", "TFORGE_TEMPORAL_WARMUP_FRAMES": str(args.warmup),
                        # AMD VAAPI did not export codec vectors and stalled
                        # during the 1080p diagnostic path. Software decode is
                        # deterministic and keeps the codec arm legitimate.
                        "TFORGE_DISABLE_HW_DECODE": "1",
                        "TFORGE_TEMPORAL_CAPTURE_TIMEOUT": str(args.timeout), "TFORGE_FSR4_FORCE_VIEWPORT": "1920x1080",
                        # Bound the Vulkan fence wait so a bad 1080p dispatch
                        # becomes an attributable capture failure, not a
                        # process that occupies the GPU for ten minutes.
                        "TFORGE_FSR4_FENCE_TIMEOUT_MS": "15000",
                        "TFORGE_FSR4_ENABLE_COLOR_HISTORY": "1", "TFORGE_FSR4_ENABLE_RECURRENT": "1",
                        "TFORGE_PRESERVE_IMAGE_ARTIFACTS": "1", "TFORGE_TEMPORAL_ARTIFACT_DIR": str(run_dir / "artifacts"),
                        "TFORGE_TEMPORAL_FAILURE_ARTIFACT_DIR": str(run_dir / "failure_artifacts"),
                        "TFORGE_FSR4_DUMP_MOTION_TEXTURE": "1", "TFORGE_FSR4_DUMP_MOTION_SIDECAR": "1",
                        "TFORGE_FSR4_DUMP_REPROJECTED_COLOR": "1", "TFORGE_FSR4_DUMP_EVENT_TRACE": "1",
                        "TFORGE_TEMPORAL_CLASS": "motion_campaign", "TFORGE_TEMPORAL_SCENE": scene,
                        "TFORGE_TEMPORAL_CONFIG_ID": "motion_campaign", "TFORGE_EXPERIMENT_ID": key,
                        "TFORGE_RUNTIME_TRACE_PATH": str(run_dir / "runtime.json")})
            env.update(ARMS[arm])
            if arm == "offline_dense":
                sidecar = run_dir / "offline_dense_replay.json"
                flow = run_dir / "offline_dense_flow.npz"
                if not sidecar.exists():
                    validator = ROOT / "tools" / "validate_dense_motion.py"
                    subprocess.run([sys.executable, str(validator), "--input", str(inp),
                                    "--output", str(run_dir / "offline_dense_report.json"),
                                    "--flow-output", str(flow), "--replay-output", str(sidecar),
                                    "--method", "farneback", "--frames", str(args.frames)],
                                   cwd=ROOT, check=True)
                env["TFORGE_FSR4_EXPERIMENTAL_DENSE_MOTION"] = str(sidecar)
            if conf is not None:
                env["TFORGE_FSR4_HISTORY_CONFIDENCE_THRESHOLD"] = conf
                env["TFORGE_FSR4_EXPERIMENTAL_EMPTY_MOTION_CONFIDENCE"] = conf
            run_dir.joinpath("artifacts").mkdir(parents=True, exist_ok=True)
            cmd = [str(ROOT / "benchmarks" / "video_corpus" / "run_temporal_quality.sh"), str(args.player), str(inp), str(ref), str(csv), str(args.frames)]
            row["command"] = cmd
            proc = subprocess.run(cmd, env=env, cwd=ROOT)
            row["status"] = "complete" if proc.returncode == 0 else f"failed:{proc.returncode}"
        with manifest.open("a", encoding="utf-8") as f:
            f.write(json.dumps(row, sort_keys=True) + "\n")
        print(f"[{count}] {row['status']} {key}", flush=True)
    print(f"planned={count} manifest={manifest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
