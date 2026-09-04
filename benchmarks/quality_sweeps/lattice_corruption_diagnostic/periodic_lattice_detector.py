#!/usr/bin/env python3
"""General periodic-lattice analysis for native/delivery campaign frames.

This is intentionally independent of the old 2x2 checker score.  It reports
the strongest narrow spectral peaks and matched-control-relative evidence so a
reviewer can inspect the detector's decision instead of trusting one scalar.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
from pathlib import Path
from typing import Iterable

import numpy as np
from PIL import Image
from scipy.ndimage import gaussian_filter

PERIODS = tuple(range(2, 33))
ORIENTATIONS = ((1, 0), (0, 1), (1, 1), (1, -1))
CAMPAIGN_ID = "quality-campaign-20260904-canonical-v1"


def _luma(path: Path, size: tuple[int, int] = (256, 144)) -> np.ndarray:
    image = Image.open(path).convert("RGB").resize(size, Image.Resampling.BILINEAR)
    rgb = np.asarray(image, dtype=np.float32) / 255.0
    return 0.2126 * rgb[..., 0] + 0.7152 * rgb[..., 1] + 0.0722 * rgb[..., 2]


def _peak_metrics(image: np.ndarray) -> tuple[list[dict[str, float]], float]:
    residual = image - gaussian_filter(image, sigma=2.0, mode="reflect")
    residual -= residual.mean()
    spectrum = np.fft.fftshift(np.fft.fft2(residual))
    power = np.abs(spectrum) ** 2
    height, width = residual.shape
    cy, cx = height // 2, width // 2
    peaks: list[dict[str, float]] = []
    for period in PERIODS:
        fx, fy = width / period, height / period
        for ox, oy in ORIENTATIONS:
            x = int(round(cx + ox * fx))
            y = int(round(cy + oy * fy))
            if not (4 <= x < width - 4 and 4 <= y < height - 4):
                continue
            value = float(power[y, x])
            neighborhood = np.concatenate(
                (
                    power[y - 4 : y - 1, x - 5 : x + 6].ravel(),
                    power[y + 2 : y + 5, x - 5 : x + 6].ravel(),
                    power[y - 1 : y + 2, x - 5 : x - 2].ravel(),
                    power[y - 1 : y + 2, x + 3 : x + 6].ravel(),
                )
            )
            background = float(np.median(neighborhood)) + 1e-12
            peaks.append(
                {
                    "period_px": float(period),
                    "orientation_x": float(ox),
                    "orientation_y": float(oy),
                    "prominence": value / background,
                    "relative_energy": value / (float(power.mean()) + 1e-12),
                }
            )
    peaks.sort(key=lambda item: item["prominence"], reverse=True)
    # A second, independent spatial measure: repeated residual energy at each
    # lag, normalized by total residual energy.
    lag_scores = []
    variance = float(np.mean(residual * residual)) + 1e-12
    for lag in PERIODS:
        horizontal = float(np.mean(residual[:, lag:] * residual[:, :-lag])) / variance
        vertical = float(np.mean(residual[lag:, :] * residual[:-lag, :])) / variance
        lag_scores.append(max(abs(horizontal), abs(vertical)))
    return peaks[:8], float(max(lag_scores))


def analyze(path: Path, controls: Iterable[Path] = ()) -> dict[str, object]:
    peaks, autocorrelation = _peak_metrics(_luma(path))
    result: dict[str, object] = {
        "path": str(path),
        "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
        "width": Image.open(path).size[0],
        "height": Image.open(path).size[1],
        "dominant_peaks": peaks,
        "max_abs_autocorrelation": autocorrelation,
        "detector_score": float(math.log1p(peaks[0]["prominence"]) * (1.0 + autocorrelation)) if peaks else 0.0,
    }
    # Absolute spectral energy is intentionally diagnostic only: scene texture
    # can produce stronger peaks than a lattice.  A matched-control residual
    # suppresses that confounder and is the value used for triage.
    control_rows = []
    for control in controls:
        if control.exists() and Image.open(control).size == Image.open(path).size:
            cpeaks, cac = _peak_metrics(_luma(path) - _luma(control))
            control_rows.append({
                "path": str(control),
                "max_abs_autocorrelation": cac,
                "dominant_peaks": cpeaks,
                "residual_score": float(math.log1p(cpeaks[0]["prominence"]) * (1.0 + cac)) if cpeaks else 0.0,
            })
    result["matched_control_residuals"] = control_rows
    result["classification"] = "uncertain"
    return result


def _metadata(path: Path) -> dict[str, object]:
    parts = path.parts
    route = next((part for part in parts if part.startswith("resolution_")), "unknown")
    arm_index = parts.index(route) + 1 if route in parts else 0
    arm = parts[arm_index] if arm_index < len(parts) else "unknown"
    scale = parts[arm_index + 1].removeprefix("scale_") if arm_index + 1 < len(parts) else "unknown"
    scene = parts[arm_index + 2] if arm_index + 2 < len(parts) else "unknown"
    match = re.match(r"resolution_(\d+)_to_(\d+)", route)
    return {
        "route": route,
        "input_resolution": int(match.group(1)) if match else None,
        "output_resolution": int(match.group(2)) if match else None,
        "arm": arm,
        "supersampling_scale": scale,
        "scene": scene,
        "view": "delivery",
    }


def run(root: Path, output: Path, catalog_path: Path | None = None) -> None:
    catalog_by_name: dict[str, dict[str, object]] = {}
    if catalog_path and catalog_path.exists():
        catalog = json.loads(catalog_path.read_text())
        for asset in catalog.get("assets", []):
            if isinstance(asset, dict) and asset.get("view") == "delivery":
                catalog_by_name[Path(str(asset.get("path", ""))).name] = asset
    rows = []
    for path in sorted(root.glob("resolution_*/**/candidate_final.png")):
        asset = catalog_by_name.get(path.name, {})
        frame_dir = path.parent / "quality_frames"
        controls = sorted(frame_dir.glob("*_bicubic.png")) + sorted(frame_dir.glob("*_lanczos.png"))
        method = asset.get("method", "")
        if not method:
            arm = str(_metadata(path)["arm"])
            method = {"fsr_pre": "temporal_fsr", "fsr_post": "temporal_fsr", "fsr_none": "temporal_fsr",
                      "nativeaa_pre": "native_aa", "nativeaa_post": "native_aa", "nativeaa_none": "native_aa"}.get(arm, "unknown")
        row = {**_metadata(path), **analyze(path, controls), "method": method}
        # Controls are explicit negative references; FSR/native outputs are
        # compared against them within the same route, scene, and scale.
        row["control_class"] = "negative_control" if str(row["method"]).startswith("conventional_") or row["method"] == "base_only_bilinear_cas20" else "candidate"
        rows.append(row)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps({"schema": "temporal_forge.periodic_lattice.v1", "campaign_id": CAMPAIGN_ID, "period_range_px": [2, 32], "rows": rows}, indent=2) + "\n")
    print(f"analysed {len(rows)} native candidate frames -> {output}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("root", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--catalog", type=Path)
    args = parser.parse_args()
    run(args.root, args.output, args.catalog)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
