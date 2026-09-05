#!/usr/bin/env python3
"""devil_analyze.py — pair-wise comparison of Devil's Advocate capture runs.

Compares final output frames (PPM) and prepass reprojected-color dumps
(RGBA16F) between two run directories, plus a per-run summary of the quality
CSV and dispatch-trace gate decisions.

usage: devil_analyze.py RUN_A RUN_B [RUN_C ...]
"""
from __future__ import annotations

import csv
import re
import sys
from pathlib import Path

import numpy as np


def read_ppm(path: Path) -> np.ndarray:
    data = path.read_bytes()
    # P6 header: magic, width, height, maxval, single whitespace, raster
    match = re.match(rb"P6\s+(\d+)\s+(\d+)\s+(\d+)\s", data)
    if not match:
        raise ValueError(f"not a P6 PPM: {path}")
    w, h, _ = int(match.group(1)), int(match.group(2)), int(match.group(3))
    raster = data[match.end():]
    return np.frombuffer(raster, dtype=np.uint8, count=w * h * 3).reshape(h, w, 3)


def frames(dir_path: Path) -> list[Path]:
    return sorted(dir_path.glob("artifacts/fsr_frames/*.ppm"))


def frame_stats(a: Path, b: Path) -> tuple[float, float]:
    va = read_ppm(a).astype(np.float32)
    vb = read_ppm(b).astype(np.float32)
    if va.shape != vb.shape:
        return float("nan"), float("nan")
    diff = np.abs(va - vb)
    return float(np.sqrt((diff**2).mean())), float(diff.max())


def repro_stats(dir_a: Path, dir_b: Path) -> list[tuple[int, float, float]]:
    out = []
    for fa in sorted(dir_a.glob("artifacts/reprojected_color_*.rgba16f")):
        fb = dir_b / fa.relative_to(dir_a)
        if not fb.is_file():
            continue
        va = np.fromfile(fa, dtype=np.float16).astype(np.float32)
        vb = np.fromfile(fb, dtype=np.float16).astype(np.float32)
        if va.shape != vb.shape:
            continue
        d = np.abs(va - vb)
        out.append((int(fa.stem.split("_")[-1]), float(d.mean()), float(d.max())))
    return out


def gate_summary(dir_path: Path) -> dict:
    log = dir_path / "artifacts" / "player.log"
    if not log.is_file():
        return {}
    text = log.read_text(errors="replace")
    gates = re.findall(r"historyGatePass=(\w+)", text)
    native = len(re.findall(r"native-int8 graph complete", text))
    generic = len(re.findall(r"generic-split graph complete", text))
    confs = [float(x) for x in re.findall(r"confidence=([0-9.]+)", text)]
    return {
        "graph": f"native={native} generic={generic}",
        "gate_pass": sum(1 for g in gates if g == "true"),
        "gate_fail": sum(1 for g in gates if g == "false"),
        "conf_min": min(confs) if confs else None,
        "conf_max": max(confs) if confs else None,
    }


def quality(dir_path: Path) -> dict:
    csvp = dir_path / "quality.csv"
    if not csvp.is_file():
        return {}
    rows = list(csv.DictReader(csvp.open()))
    fsr = [r for r in rows if r.get("label") == "fsr"]
    if not fsr:
        return {}
    f = fsr[0]
    return {
        "ssim": float(f["fsr_ssim_mean"]),
        "ssim_min": float(f["fsr_ssim_min"]),
        "tdae": float(f["fsr_temporal_delta_abs_error"]),
        "lanczos": float(f["lanczos_ssim_mean"]),
    }


def main() -> int:
    runs = [Path(p) for p in sys.argv[1:]]
    print("== per-run summary ==")
    for r in runs:
        q = quality(r)
        g = gate_summary(r)
        n = len(frames(r))
        print(f"{r.name:14s} ssim={q.get('ssim')} min={q.get('ssim_min')} "
              f"tdae={q.get('tdae')} lanczos={q.get('lanczos')} frames={n} "
              f"{g}")
    print("\n== pairwise output-frame diffs (RMSE / max, 8-bit levels) ==")
    for i in range(len(runs)):
        for j in range(i + 1, len(runs)):
            fa, fb = frames(runs[i]), frames(runs[j])
            if not fa or not fb:
                continue
            stats = [frame_stats(a, b) for a, b in zip(fa, fb)]
            rmses = [s[0] for s in stats if s == s]
            mx = max((s[1] for s in stats if s == s), default=float("nan"))
            print(f"{runs[i].name:14s} vs {runs[j].name:14s} "
                  f"meanRMSE={np.mean(rmses):8.4f} maxRMSE={max(rmses):8.4f} "
                  f"absMax={mx:5.0f}")
    print("\n== reprojected-color dump diffs (run[0] vs others) ==")
    for r in runs[1:]:
        rs = repro_stats(runs[0], r)
        if rs:
            mean_of_means = np.mean([x[1] for x in rs])
            print(f"{runs[0].name:14s} vs {r.name:14s} "
                  f"reproMeanAbsDiff={mean_of_means:8.5f} over {len(rs)} frames")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
