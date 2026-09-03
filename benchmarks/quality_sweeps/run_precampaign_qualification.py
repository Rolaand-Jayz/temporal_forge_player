#!/usr/bin/env python3
"""Run the small, shared-engine precampaign gate and build its review harness."""
from __future__ import annotations
import argparse, csv, hashlib, json, os, shutil, subprocess, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RUNNER = ROOT / "benchmarks/video_corpus/run_quality.sh"
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

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--player", type=Path, default=ROOT / "build-fast/temporal_forge_player")
    ap.add_argument("--output-root", type=Path)
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
                    "TFORGE_QUALITY_MANIFEST": str(ROOT / "benchmarks/video_corpus/manifest.csv"),
                    "TFORGE_QUALITY_CLIP": f"^{scene}$", "TFORGE_QUALITY_QUALITY": "high",
                    "TFORGE_QUALITY_FRAME": "48", "TFORGE_QUALITY_CAPTURE_ATTEMPTS": "180",
                    "TFORGE_QUALITY_ARTIFACT_ROOT": str(case), "TFORGE_QUALITY_FRAMES_DIR": str(case / "frames"),
                    "TFORGE_QUALITY_LOGS_DIR": str(case / "logs"), "TFORGE_QUALITY_TAG": f"precamp_{inp}_{delivery}_{cas_name}",
                    "TFORGE_FSR4_FORCE_VIEWPORT": viewport, "TFORGE_FSR4_FORCE_SCALE": "2.00",
                    "TFORGE_FSR4_CAS_STRENGTH": cas_strength, "TFORGE_FSR4_DISABLE_CAS": "1" if cas_name == "no_cas" else "0",
                    "TFORGE_QUALITY_PROFILE": "AMD_SEMANTIC_BASELINE", "TFORGE_FSR4_INTEGRATED_BEST_FINDINGS": "1",
                    "TFORGE_FSR4_DUMP_OUTPUT": "1", "TFORGE_FSR4_DUMP_RAW": "1",
                    "TFORGE_GIT_HEAD": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
                    "TFORGE_GIT_DIRTY": "0",
                })
                case.mkdir(parents=True)
                csv_path = case / "results.csv"
                subprocess.run(["bash", str(RUNNER), str(player), selector, str(csv_path)], cwd=ROOT, env=env, check=True)
                rows = list(csv.DictReader(csv_path.open(newline="", encoding="utf-8")))
                if len(rows) != 1: raise SystemExit(f"expected one result row: {csv_path}")
                row = rows[0]
                image = Path(row["output_path"])
                if not image.is_file() or image.stat().st_size == 0: raise SystemExit(f"missing output: {image}")
                harness_image = ppm_png(out / "harness", scene, inp, delivery, cas_name)
                harness_image.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(image, harness_image)
                records.append({"scene": scene, "input": inp, "output": delivery, "cas": cas_name,
                                "csv": str(csv_path), "image": str(harness_image), "width": int(row["output_width"]),
                                "height": int(row["output_height"]), "sha256": digest(harness_image),
                                "binary_sha256": binary_sha, "config_sha256": config_sha})
    harness = out / "harness"
    catalog = {"schema": "temporal_forge.precampaign_harness.v1", "status": "AUTOMATED QUALIFICATION: PASS", "human_review": "PENDING", "assets": records}
    (harness / "catalog.json").write_text(json.dumps(catalog, indent=2) + "\n", encoding="utf-8")
    html = "<!doctype html><meta charset=utf-8><title>Temporal Forge precampaign</title><h1>AUTOMATED QUALIFICATION: PASS</h1><p>HUMAN VISUAL REVIEW: PENDING</p>" + "".join(f"<figure><figcaption>{r['scene']} {r['input']}→{r['output']} {r['cas']}</figcaption><img src='{Path(r['image']).name}'></figure>" for r in records)
    (harness / "index.html").write_text(html, encoding="utf-8")
    (out / "qualification_manifest.json").write_text(json.dumps({"status": "AUTOMATED QUALIFICATION: PASS", "human_review": "PENDING", "full_campaign": "NOT STARTED", "git_head": git_head, "binary_sha256": binary_sha, "config_sha256": config_sha, "cases": CASES, "scenes": SCENES, "assets": records}, indent=2) + "\n", encoding="utf-8")
    print(f"AUTOMATED QUALIFICATION: PASS\nHUMAN VISUAL REVIEW: PENDING\nFULL CAMPAIGN NOT STARTED\n{out}")
    return 0
if __name__ == "__main__": raise SystemExit(main())
