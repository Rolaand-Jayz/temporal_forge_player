#!/usr/bin/env python3
"""Run the small, shared-engine precampaign gate and build its review harness."""
from __future__ import annotations
import argparse, csv, hashlib, json, os, shutil, subprocess, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(ROOT))
from benchmarks.quality_sweeps.capture_engine import run_renderer
RUNNER = ROOT / "benchmarks/video_corpus/run_quality.sh"
CAMPAIGN_GENERATION = "post_lattice_fix_canonical_v1"
PIPELINE_REVISION = "temporal_lattice_fix_closeout"
SCENES = ("sintel_cave", "tos_daylight")
CASES = ((360, 720), (360, 1080), (720, 1080))

def digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()

def ppm_png(root: Path, scene: str, inp: int, out: int, cas: str) -> Path:
    return root / f"{scene}_{inp}to{out}_{cas}.png"

def checker_score(path: Path) -> float:
    """Periodic 2x2 energy ratio used as a conservative lattice tripwire."""
    from PIL import Image
    import numpy as np
    image = np.asarray(Image.open(path).convert("L"), dtype=np.float32)
    if image.shape[0] < 2 or image.shape[1] < 2 or image.std() < 1e-6:
        return 0.0
    residual = image[:-1, :-1] - image[:-1, 1:] - image[1:, :-1] + image[1:, 1:]
    return float(np.mean(np.abs(residual)) / image.std())

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--player", type=Path, default=ROOT / "build-fast/temporal_forge_player")
    ap.add_argument("--output-root", type=Path)
    ap.add_argument("--manifest", type=Path, default=ROOT / "benchmarks/video_corpus/manifest.csv")
    args = ap.parse_args()
    player = args.player.resolve()
    if not player.is_file() or not os.access(player, os.X_OK):
        raise SystemExit(f"missing executable: {player}")
    if subprocess.run(["git", "diff", "--quiet"], cwd=ROOT).returncode != 0 or subprocess.run(["git", "diff", "--cached", "--quiet"], cwd=ROOT).returncode != 0:
        raise SystemExit("precampaign requires a committed tracked worktree")
    stamp = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
    out = (args.output_root or ROOT / "benchmarks/quality_sweeps" / f"precampaign_{stamp}").resolve()
    if out.exists():
        raise SystemExit(f"refusing to overwrite {out}")
    out.mkdir(parents=True)
    binary_sha = digest(player)
    git_head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    config = ROOT / "benchmarks/video_corpus/benchmark_settings.json"
    config_sha = digest(config)
    records = []
    for inp, delivery in CASES:
        selector = f"{inp * 16 // 9}x{inp}" if inp != 360 else "640x360"
        if inp == 720: selector = "1280x720"
        viewport = {720: "1280x720", 1080: "1920x1080"}[delivery]
        for scene in SCENES:
            for cas_name, cas_strength in (("no_cas", "0.00"), ("cas20", "0.20")):
                case = out / f"{scene}_{inp}to{delivery}_{cas_name}"
                env = os.environ.copy()
                env.update({
                    "TFORGE_QUALITY_MANIFEST": str(args.manifest.resolve()),
                    "TFORGE_QUALITY_CLIP": f"^{scene}$", "TFORGE_QUALITY_QUALITY": "high",
                    "TFORGE_QUALITY_FRAME": "48", "TFORGE_QUALITY_CAPTURE_ATTEMPTS": "180",
                    "TFORGE_QUALITY_ARTIFACT_ROOT": str(case), "TFORGE_QUALITY_FRAMES_DIR": str(case / "frames"),
                    "TFORGE_QUALITY_LOGS_DIR": str(case / "logs"), "TFORGE_QUALITY_TAG": f"precamp_{inp}_{delivery}_{cas_name}",
                    "TFORGE_FSR4_DUMP_STAGE_DIR": str(case / "stages"), "TFORGE_FSR4_DUMP_MODEL_INPUT": "1",
                    "TFORGE_FSR4_DUMP_MODEL_INPUT_FRAME": "48",
                    "TFORGE_FSR4_FORCE_VIEWPORT": viewport, "TFORGE_FSR4_FORCE_SCALE": "2.00",
                    "TFORGE_FSR4_CAS_STRENGTH": cas_strength, "TFORGE_FSR4_DISABLE_CAS": "1" if cas_name == "no_cas" else "0",
                    "TFORGE_QUALITY_PROFILE": "AMD_SEMANTIC_BASELINE", "TFORGE_FSR4_INTEGRATED_BEST_FINDINGS": "1",
                    "TFORGE_FSR4_ENABLE_RECURRENT": "1", "TFORGE_FSR4_EXPERIMENTAL_PREPASS_JITTER_ORDERING": "1",
                    "TFORGE_FSR4_EXPERIMENTAL_SOURCE_TAP_MULAW": "1", "TFORGE_FSR4_JITTER_SEQUENCE": "halton23",
                    "TFORGE_FSR4_DUMP_OUTPUT": "1", "TFORGE_FSR4_DUMP_RAW": "1",
                    "TFORGE_RUNTIME_TRACE_PATH": str(case / "runtime_pipeline.json"),
                    "TFORGE_EXPERIMENT_ID": f"canonical_precamp_{inp}_{delivery}_{scene}_{cas_name}",
                    "TFORGE_CONFIG_SHA256": config_sha,
                    "TFORGE_GIT_HEAD": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
                    "TFORGE_GIT_DIRTY": "0",
                })
                case.mkdir(parents=True)
                csv_path = case / "results.csv"
                run_renderer(["bash", str(RUNNER), str(player), selector, str(csv_path)], cwd=ROOT, env=env)
                rows = list(csv.DictReader(csv_path.open(newline="", encoding="utf-8")))
                if len(rows) != 1: raise SystemExit(f"expected one result row: {csv_path}")
                row = rows[0]
                trace_path = case / "runtime_pipeline.json"
                if not trace_path.is_file():
                    raise SystemExit(f"missing runtime semantic trace: {trace_path}")
                trace = json.loads(trace_path.read_text(encoding="utf-8"))
                semantic = {
                    "prepass_resolve_source": "model_color",
                    "prepass_resolve_stage": "prepass_input_resolve",
                    "model_color_transfer": "eotf_mulaw_pretransformed",
                    "model_color_format": "rgb10_a2",
                    "mulaw_application_stage": "yuv_to_model_color",
                    "mulaw_sampling_semantics": "pretransformed_before_resolve",
                    "jitter_stage": "prepass_input_resolve",
                    "jitter_sequence": "halton23",
                    "source_display_format": "rgba8",
                    "source_display_used_for_current_resolve": False,
                }
                if any(trace.get(key) != value for key, value in semantic.items()):
                    raise SystemExit(f"runtime semantic provenance mismatch: {trace_path}")
                image = Path(row["output_path"])
                if not image.is_file() or image.stat().st_size == 0: raise SystemExit(f"missing output: {image}")
                expected = (int(viewport.split("x")[0]), int(viewport.split("x")[1]))
                if (int(row["output_width"]), int(row["output_height"])) != expected:
                    raise SystemExit(f"geometry mismatch for {csv_path}: {row['output_width']}x{row['output_height']} != {viewport}")
                controls = list(case.glob("frames/*_bicubic.png")) + list(case.glob("frames/*_lanczos.png"))
                if len(controls) != 2 or any(p.stat().st_size == 0 for p in controls):
                    raise SystemExit(f"missing conventional controls for {csv_path}")
                stage_b = case / "stages" / "stage-B-color.ppm"
                if not stage_b.is_file():
                    raise SystemExit(f"missing Stage-B capture for {csv_path}")
                # The gate must score the native/final Temporal Forge output,
                # not Stage B. Stage B remains a provenance artifact, while
                # this final-output tripwire catches temporal feedback
                # recurrence downstream of the resize handoff.
                score = checker_score(image)
                if score >= 0.20:
                    raise SystemExit(f"final-output periodic lattice tripwire exceeded for {csv_path}: {score:.4f}")
                harness_image = ppm_png(out / "harness", scene, inp, delivery, cas_name)
                harness_image.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(image, harness_image)
                records.append({"scene": scene, "input": inp, "output": delivery, "cas": cas_name,
                                "csv": str(csv_path), "image": str(harness_image), "width": int(row["output_width"]),
                                "height": int(row["output_height"]), "sha256": digest(harness_image),
                                "final_checker_score": score,
                                "stage_b_checker_score": checker_score(stage_b),
                                "binary_sha256": binary_sha, "config_sha256": config_sha})
    harness = out / "harness"
    hashes = [item["sha256"] for item in records]
    if len(hashes) != len(set(hashes)):
        raise SystemExit("duplicate/stale precampaign harness image hashes")
    catalog = {"schema": "temporal_forge.precampaign_harness.v1", "status": "AUTOMATED QUALIFICATION: PASS", "human_review": "PENDING", "assets": records}
    (harness / "catalog.json").write_text(json.dumps(catalog, indent=2) + "\n", encoding="utf-8")
    html = "<!doctype html><meta charset=utf-8><title>Temporal Forge precampaign</title><h1>AUTOMATED QUALIFICATION: PASS</h1><p>HUMAN VISUAL REVIEW: PENDING</p>" + "".join(f"<figure><figcaption>{r['scene']} {r['input']}→{r['output']} {r['cas']}</figcaption><img src='{Path(r['image']).name}'></figure>" for r in records)
    (harness / "index.html").write_text(html, encoding="utf-8")
    (out / "qualification_manifest.json").write_text(json.dumps({"status": "AUTOMATED QUALIFICATION: PASS", "human_review": "PENDING", "full_campaign": "NOT STARTED", "campaign_generation": CAMPAIGN_GENERATION, "pipeline_revision": PIPELINE_REVISION, "git_head": git_head, "git_dirty": False, "binary_sha256": binary_sha, "config_sha256": config_sha, "quality_profile": "AMD_SEMANTIC_BASELINE", "cases": CASES, "scenes": SCENES, "assets": records}, indent=2) + "\n", encoding="utf-8")
    print(f"AUTOMATED QUALIFICATION: PASS\nHUMAN VISUAL REVIEW: PENDING\nFULL CAMPAIGN NOT STARTED\n{out}")
    return 0
if __name__ == "__main__": raise SystemExit(main())
