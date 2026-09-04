#!/usr/bin/env python3
"""Offline precision/ordering ablation for the P0 reproducer.

This deliberately does not alter Vulkan resources.  It evaluates the same
source-grid samples with float32, an RGB10 quantized carrier, and two
nonlinear transform orderings.  The input is the native stage-A readback, so
the comparison is independent of review-harness scaling.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np


def read_ppm(path: Path) -> np.ndarray:
    data = path.read_bytes()
    head, payload = data.split(b"\n", 1)
    if head.strip() != b"P6":
        raise ValueError(f"{path}: expected binary P6 PPM")
    tokens = []
    while len(tokens) < 3:
        line, payload = payload.split(b"\n", 1)
        line = line.split(b"#", 1)[0]
        tokens.extend(line.split())
    width, height, maximum = map(int, tokens[:3])
    if maximum != 255:
        raise ValueError(f"{path}: expected 8-bit PPM, got max={maximum}")
    expected = width * height * 3
    raw = np.frombuffer(payload[:expected], dtype=np.uint8)
    if raw.size != expected:
        raise ValueError(f"{path}: truncated pixel payload")
    return raw.reshape(height, width, 3).astype(np.float32) / 255.0


def resize_bilinear(image: np.ndarray, width: int, height: int) -> np.ndarray:
    """Center-aligned bilinear resolve matching the diagnostic shader."""
    h, w, _ = image.shape
    xs = (np.arange(width, dtype=np.float32) + 0.5) * w / width - 0.5
    ys = (np.arange(height, dtype=np.float32) + 0.5) * h / height - 0.5
    x0 = np.clip(np.floor(xs).astype(np.int32), 0, w - 1)
    y0 = np.clip(np.floor(ys).astype(np.int32), 0, h - 1)
    x1 = np.clip(x0 + 1, 0, w - 1)
    y1 = np.clip(y0 + 1, 0, h - 1)
    fx = (xs - np.floor(xs))[:, None]
    fy = (ys - np.floor(ys))[None, :]
    top = image[y0[:, None], x0[None, :]]
    right = image[y0[:, None], x1[None, :]]
    bottom = image[y1[:, None], x0[None, :]]
    bottom_right = image[y1[:, None], x1[None, :]]
    fx = fx.reshape(1, width, 1)
    fy = fy.reshape(height, 1, 1)
    return (top * (1 - fx) * (1 - fy)
            + right * fx * (1 - fy)
            + bottom * (1 - fx) * fy
            + bottom_right * fx * fy)


def periodic_energy(image: np.ndarray) -> tuple[float, float, list[dict[str, float]]]:
    gray = image.mean(axis=2)
    residual = gray - gray.mean(axis=0, keepdims=True)
    residual -= residual.mean(axis=1, keepdims=True)
    spectrum = np.abs(np.fft.fftshift(np.fft.fft2(residual))) ** 2
    cy, cx = np.array(spectrum.shape) // 2
    spectrum[cy - 1:cy + 2, cx - 1:cx + 2] = 0
    total = float(spectrum.sum()) or 1.0
    fy_grid = np.abs(np.fft.fftfreq(spectrum.shape[0]))[:, None]
    fx_grid = np.abs(np.fft.fftfreq(spectrum.shape[1]))[None, :]
    short_band = ((fx_grid >= 1 / 32) & (fx_grid <= 1 / 2)) | ((fy_grid >= 1 / 32) & (fy_grid <= 1 / 2))
    short_band_energy = float(spectrum[short_band].sum() / total)
    band_values = spectrum[short_band]
    band_median = float(np.median(band_values)) or 1.0
    narrow_peak_ratio = float(np.max(band_values) / band_median)
    peaks = []
    band_spectrum = np.where(short_band, spectrum, 0.0)
    for y, x in zip(*np.unravel_index(np.argsort(band_spectrum.ravel())[-8:], spectrum.shape)):
        fy = abs((y - cy) / spectrum.shape[0])
        fx = abs((x - cx) / spectrum.shape[1])
        peaks.append({"period_x_px": float(1 / fx) if fx else 0.0,
                      "period_y_px": float(1 / fy) if fy else 0.0,
                      "energy_fraction": float(spectrum[y, x] / total)})
    return float(spectrum.sum() / (gray.size * gray.size)), short_band_energy, narrow_peak_ratio, peaks


def rec709_eotf(encoded: np.ndarray) -> np.ndarray:
    return np.power(np.clip(encoded, 0, 1), 2.4, dtype=np.float32)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("stage_a", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    source = read_ppm(args.stage_a)
    h, w, _ = source.shape
    target_w, target_h = max(1, round(w * 0.75)), max(1, round(h * 0.75))

    float_carrier = source.astype(np.float32)
    rgb10_carrier = np.round(float_carrier * 1023.0) / 1023.0
    float_resolve = resize_bilinear(float_carrier, target_w, target_h)
    rgb10_resolve = resize_bilinear(rgb10_carrier, target_w, target_h)

    # Construct a high-precision encoded signal from the captured transformed
    # values. This avoids pretending an 8-bit display tap is a precision arm.
    encoded = np.power(np.clip(float_carrier, 0, 1), 1 / 2.4, dtype=np.float32)
    transform_before = resize_bilinear(rec709_eotf(encoded), target_w, target_h)
    resolve_before = rec709_eotf(resize_bilinear(encoded, target_w, target_h))

    def summarize(a: np.ndarray, b: np.ndarray) -> dict:
        delta = a - b
        energy, short_band_energy, narrow_peak_ratio, peaks = periodic_energy(delta)
        return {"rmse": float(np.sqrt(np.mean(delta * delta))),
                "max_abs": float(np.max(np.abs(delta))),
                "periodic_energy": energy,
                "short_period_2_to_32_energy_fraction": short_band_energy,
                "short_period_narrow_peak_to_median": narrow_peak_ratio,
                "dominant_periods": peaks}

    result = {
        "schema": "temporal_forge.lattice_precision_order_ablation.v1",
        "input": str(args.stage_a),
        "source_dimensions": [w, h],
        "resolve_dimensions": [target_w, target_h],
        "carrier": {
            "rgb10_a2_vs_float32": summarize(rgb10_resolve, float_resolve),
            "interpretation": "The report includes short-period band energy and narrow-peak-to-median values for comparison; this is an offline ablation, not a production carrier swap.",
        },
        "transform_order": {
            "transform_before_resolve_vs_resolve_before_transform": summarize(transform_before, resolve_before),
            "interpretation": "Ordering is numerically distinguishable; the independent GPU resolve-before-transform arm remains outside the current descriptor contract.",
        },
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
