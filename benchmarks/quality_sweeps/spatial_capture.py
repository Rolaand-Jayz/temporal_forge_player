#!/usr/bin/env python3
"""Validate and materialize the M6 class-region capture input.

The capture runner accepts one JSON input with this contract::

    {
      "schema": "temporal_forge.spatial_capture.v1",
      "annotationsPath": "benchmarks/quality_sweeps/m6_quality_class_annotations.json",
      "classSelections": {"scene": ["class"]},
      "frame": 48,
      "outputDimensions": "1920x1080"
    }

``annotationsPath`` is the source of truth for each selected class region.
The current slice intentionally supports bounded output-space rectangles only;
there is no implicit whole-frame or scene-name-to-class fallback.
"""

from __future__ import annotations

import argparse
import json
import re
from collections.abc import Mapping
from pathlib import Path
from typing import Any


class SpatialCaptureError(ValueError):
    """Raised when class-region capture evidence cannot be resolved safely."""


_DIMENSION = re.compile(r"^(?P<width>[1-9][0-9]*)x(?P<height>[1-9][0-9]*)$")
_SYNTHETIC = re.compile(
    r"^(?:synthetic(?:_|\.)|source(?:_|\.)|supersampled_aa)", re.IGNORECASE
)
_CLASS = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]*$")


def _read_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise SpatialCaptureError(f"cannot read spatial capture JSON {path}: {error}") from error


def _resolve(value: Any, *, base: Path, name: str) -> Path:
    if not isinstance(value, str) or not value:
        raise SpatialCaptureError(f"{name} must be a non-empty path")
    path = Path(value)
    return path if path.is_absolute() else (base / path).resolve()


def _dimensions(value: Any, name: str) -> tuple[int, int]:
    if not isinstance(value, str):
        raise SpatialCaptureError(f"{name} must be WIDTHxHEIGHT")
    match = _DIMENSION.fullmatch(value)
    if match is None:
        raise SpatialCaptureError(f"{name} must be WIDTHxHEIGHT")
    return int(match.group("width")), int(match.group("height"))


def _integer(value: Any, name: str, *, minimum: int = 0) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < minimum:
        raise SpatialCaptureError(f"{name} must be an integer >= {minimum}")
    return value


def _selected_classes(class_selections: Any) -> dict[str, list[str]]:
    if not isinstance(class_selections, Mapping) or not class_selections:
        raise SpatialCaptureError("classSelections must be a non-empty object")
    selected: dict[str, list[str]] = {}
    for scene, values in class_selections.items():
        if not isinstance(scene, str) or not scene:
            raise SpatialCaptureError("classSelections scene names must be non-empty strings")
        if _SYNTHETIC.match(scene):
            raise SpatialCaptureError(f"synthetic scene is not allowed: {scene}")
        if not isinstance(values, list) or any(
            not isinstance(value, str) or not value or _CLASS.fullmatch(value) is None
            for value in values
        ):
            raise SpatialCaptureError(
                f"classSelections[{scene}] must contain safe class names"
            )
        if len(values) != len(set(values)):
            raise SpatialCaptureError(f"classSelections[{scene}] contains duplicate classes")
        selected[scene] = list(values)
    return selected


def load_region_annotations(
    annotations_path: Path,
    *,
    class_selections: Mapping[str, list[str]],
    frame: int,
    output_dimensions: str,
    repo_root: Path,
) -> dict[tuple[str, str], dict[str, Any]]:
    """Load exactly one source-backed output-space region for each selection."""
    selected = _selected_classes(class_selections)
    output_width, output_height = _dimensions(output_dimensions, "outputDimensions")
    if isinstance(frame, bool) or not isinstance(frame, int) or frame < 0:
        raise SpatialCaptureError("frame must be a non-negative integer")
    document = _read_json(Path(annotations_path))
    if not isinstance(document, Mapping):
        raise SpatialCaptureError("annotation document must be an object")
    annotations = document.get("annotations")
    if not isinstance(annotations, list):
        raise SpatialCaptureError("annotation document.annotations must be a list")

    resolved: dict[tuple[str, str], dict[str, Any]] = {}
    annotation_base = Path(annotations_path).resolve().parent
    selected_scenes = set(selected)
    selected_pairs = {
        (scene, quality_class)
        for scene, classes in selected.items()
        for quality_class in classes
    }
    for index, raw in enumerate(annotations):
        if not isinstance(raw, Mapping):
            raise SpatialCaptureError(f"annotation {index} must be an object")
        scene = raw.get("scene")
        quality_class = raw.get("qualityClass")
        if not isinstance(scene, str) or not scene:
            raise SpatialCaptureError(f"annotation {index}.scene must be a non-empty string")
        if _SYNTHETIC.match(scene):
            raise SpatialCaptureError(f"synthetic annotation scene is not allowed: {scene}")
        if scene not in selected_scenes:
            raise SpatialCaptureError(f"annotation scene is not selected: {scene}")
        if not isinstance(quality_class, str) or not quality_class:
            raise SpatialCaptureError(
                f"annotation {index}.qualityClass must be a non-empty string"
            )
        key = (scene, quality_class)
        if key not in selected_pairs:
            raise SpatialCaptureError(
                f"annotation class is not selected for scene: {scene}/{quality_class}"
            )
        if key in resolved:
            raise SpatialCaptureError(
                f"ambiguous class-region mapping for {scene}/{quality_class}"
            )
        if raw.get("frame") != frame:
            raise SpatialCaptureError(
                f"annotation frame does not match capture for {scene}/{quality_class}"
            )
        asset_path = _resolve(raw.get("assetPath"), base=repo_root, name="assetPath")
        if not asset_path.is_file():
            raise SpatialCaptureError(f"annotation asset does not exist: {asset_path}")
        region = raw.get("staticRegion")
        if not isinstance(region, Mapping):
            raise SpatialCaptureError(
                f"missing staticRegion for {scene}/{quality_class}; whole-scene capture is forbidden"
            )
        region_width = _integer(region.get("width"), f"{key}.staticRegion.width", minimum=1)
        region_height = _integer(region.get("height"), f"{key}.staticRegion.height", minimum=1)
        region_x = _integer(region.get("x"), f"{key}.staticRegion.x")
        region_y = _integer(region.get("y"), f"{key}.staticRegion.y")
        if region.get("imageWidth") != output_width or region.get("imageHeight") != output_height:
            raise SpatialCaptureError(
                f"region dimensions do not match outputDimensions for {scene}/{quality_class}"
            )
        if region_x + region_width > output_width or region_y + region_height > output_height:
            raise SpatialCaptureError(
                f"staticRegion is outside outputDimensions for {scene}/{quality_class}"
            )
        resolved[key] = {
            "scene": scene,
            "class": quality_class,
            "x": region_x,
            "y": region_y,
            "width": region_width,
            "height": region_height,
            "annotationPath": str(Path(annotations_path).resolve()),
            "assetPath": str(asset_path),
            "annotationIndex": index,
        }

    missing = sorted(selected_pairs - set(resolved))
    if missing:
        raise SpatialCaptureError(f"missing class-region mapping: {missing}")
    return resolved


def load_capture_input(path: Path, *, repo_root: Path) -> dict[tuple[str, str], dict[str, Any]]:
    """Validate the runner input JSON and return its class-region mapping."""
    document = _read_json(Path(path))
    if not isinstance(document, Mapping):
        raise SpatialCaptureError("spatial capture input must be an object")
    if document.get("schema") != "temporal_forge.spatial_capture.v1":
        raise SpatialCaptureError(
            "spatial capture input schema must be temporal_forge.spatial_capture.v1"
        )
    selected = _selected_classes(document.get("classSelections"))
    annotations_path = _resolve(
        document.get("annotationsPath"),
        base=Path(path).resolve().parent,
        name="annotationsPath",
    )
    return load_region_annotations(
        annotations_path,
        class_selections=selected,
        frame=document.get("frame"),
        output_dimensions=document.get("outputDimensions"),
        repo_root=repo_root,
    )


def write_tsv(mapping: Mapping[tuple[str, str], Mapping[str, Any]], output: Path) -> None:
    """Write a shell-safe tab-separated map: scene, class, x, y, width, height."""
    lines = [
        "\t".join(
            str(item[field])
            for field in ("scene", "class", "x", "y", "width", "height")
        )
        for _, item in sorted(mapping.items())
    ]
    Path(output).write_text("\n".join(lines) + ("\n" if lines else ""), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    repo_root = Path(__file__).resolve().parents[2]
    mapping = load_capture_input(args.input, repo_root=repo_root)
    write_tsv(mapping, args.output)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except SpatialCaptureError as error:
        print(f"spatial capture error: {error}")
        raise SystemExit(2)
