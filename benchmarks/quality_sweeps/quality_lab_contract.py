#!/usr/bin/env python3
"""Validate and materialize auditable quality-lab capture contracts.

Upstream: a checked-in quality candidate, the player binary, corpus metadata,
and explicitly recorded runtime settings. Downstream: immutable run folders
that can be audited without guessing which source, dimensions, or settings
produced an image. This module does not launch the player or alter its output.
"""

from __future__ import annotations

import hashlib
import json
import os
import re
from pathlib import Path
from typing import Any, Mapping


class ManifestError(ValueError):
    """Raised when a baseline manifest cannot support an auditable capture."""


class ArtifactExistsError(FileExistsError):
    """Raised instead of replacing an existing quality artifact directory."""


_SHA256 = re.compile(r"^[0-9a-fA-F]{64}$")
_SAFE_COMPONENT = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]*$")
_STAGES = {f"D{number}" for number in range(9)}
_DIMENSION_NAMES = ("source", "model", "history", "display")
_JITTER_MODES = {"off", "current", "reduced", "controlled"}

# These prefixes identify settings that can change pixels or temporal state.
# Ordinary process settings such as PATH are deliberately outside this list.
IMAGE_AFFECTING_ENV_PREFIXES = (
    "TFORGE_QUALITY_",
    "TFORGE_FSR4_",
    "TFORGE_UPSCALE_",
    "TFORGE_JITTER_",
    "TFORGE_REVIEW_",
    "TFORGE_BENCHMARK_",
)
IMAGE_AFFECTING_ENV_KEYS = {"TFORGE_DISABLE_HW_DECODE"}


def _require_mapping(value: Any, name: str) -> Mapping[str, Any]:
    """Require an object-shaped value and return it with a useful error."""
    if not isinstance(value, Mapping):
        raise ManifestError(f"{name} must be a JSON object")
    return value


def _require_string(value: Any, name: str) -> str:
    """Require a non-empty string for identifiers and paths."""
    if not isinstance(value, str) or not value:
        raise ManifestError(f"{name} must be a non-empty string")
    return value


def _require_positive_int(value: Any, name: str) -> int:
    """Require a positive integer for a pixel dimension."""
    if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
        raise ManifestError(f"{name} must be a positive integer")
    return value


def _validate_dimensions(value: Any, name: str) -> None:
    """Validate width and height without allowing the old ambiguous target."""
    dimensions = _require_mapping(value, name)
    width = _require_positive_int(dimensions.get("width"), f"{name}.width")
    height = _require_positive_int(dimensions.get("height"), f"{name}.height")
    if (width, height) == (1278, 720):
        raise ManifestError(f"{name} uses the forbidden ambiguous 1278x720 target")


def _validate_hashed_file(value: Any, name: str) -> None:
    """Validate a path/hash pair; hashing is recorded even when the file is external."""
    record = _require_mapping(value, name)
    _require_string(record.get("path"), f"{name}.path")
    digest = _require_string(record.get("sha256"), f"{name}.sha256")
    if not _SHA256.fullmatch(digest):
        raise ManifestError(f"{name}.sha256 must be a 64-character hexadecimal digest")


def _validate_artifact(value: Any, index: int) -> None:
    """Validate one D0-D8 stage artifact declaration."""
    artifact = _require_mapping(value, f"artifacts[{index}]")
    stage = _require_string(artifact.get("stage"), f"artifacts[{index}].stage")
    if stage not in _STAGES:
        raise ManifestError(f"artifacts[{index}].stage must be one of D0-D8")
    _require_string(artifact.get("path"), f"artifacts[{index}].path")
    _validate_dimensions(artifact, f"artifacts[{index}]")
    _require_string(artifact.get("format"), f"artifacts[{index}].format")
    if not isinstance(artifact.get("finiteChecked"), bool):
        raise ManifestError(f"artifacts[{index}].finiteChecked must be boolean")
    if not artifact["finiteChecked"]:
        raise ManifestError(f"artifacts[{index}] has not passed finite-value validation")


def _validate_jitter(value: Any) -> None:
    """Validate optional decoded-video jitter provenance for M5 sequences."""
    jitter = _require_mapping(value, "manifest.jitter")
    mode = _require_string(jitter.get("mode"), "manifest.jitter.mode")
    if mode not in _JITTER_MODES:
        raise ManifestError(
            f"manifest.jitter.mode must be one of {sorted(_JITTER_MODES)}"
        )
    strength = jitter.get("controlledStrength", 1.0)
    if not isinstance(strength, (int, float)) or isinstance(strength, bool):
        raise ManifestError("manifest.jitter.controlledStrength must be numeric")
    if not 0.0 <= float(strength) <= 1.5:
        raise ManifestError("manifest.jitter.controlledStrength must be in [0, 1.5]")


def validate_manifest(manifest: Any) -> None:
    """Validate the complete M0 baseline schema without touching any files."""
    root = _require_mapping(manifest, "manifest")
    if root.get("schemaVersion") != 1:
        raise ManifestError("manifest.schemaVersion must be 1")
    _require_string(root.get("runId"), "manifest.runId")
    _require_string(root.get("candidateId"), "manifest.candidateId")
    _require_string(root.get("gitCommit"), "manifest.gitCommit")
    _validate_hashed_file(root.get("binary"), "manifest.binary")
    _validate_hashed_file(root.get("configuration"), "manifest.configuration")

    settings = _require_mapping(root.get("settings"), "manifest.settings")
    if not settings:
        raise ManifestError("manifest.settings must record image-affecting settings")
    if "jitter" in root:
        _validate_jitter(root["jitter"])

    dimensions = _require_mapping(root.get("dimensions"), "manifest.dimensions")
    for name in _DIMENSION_NAMES:
        _validate_dimensions(dimensions.get(name), f"manifest.dimensions.{name}")

    corpus = _require_mapping(root.get("corpus"), "manifest.corpus")
    _require_string(corpus.get("manifestPath"), "manifest.corpus.manifestPath")
    selection = corpus.get("selection")
    if not isinstance(selection, list) or not selection or not all(
        isinstance(item, str) and item for item in selection
    ):
        raise ManifestError("manifest.corpus.selection must be a non-empty string list")
    _require_string(corpus.get("inputPath"), "manifest.corpus.inputPath")
    _require_string(corpus.get("referencePath"), "manifest.corpus.referencePath")

    timing = _require_mapping(root.get("timing"), "manifest.timing")
    _require_string(timing.get("path"), "manifest.timing.path")
    for name in ("pipelineCpuMs", "dispatchCpuMs", "gpuMs"):
        value = timing.get(name)
        if not isinstance(value, (int, float)) or isinstance(value, bool) or value < 0:
            raise ManifestError(f"manifest.timing.{name} must be a non-negative number")

    artifacts = root.get("artifacts")
    if not isinstance(artifacts, list) or not artifacts:
        raise ManifestError("manifest.artifacts must contain at least one stage")
    for index, artifact in enumerate(artifacts):
        _validate_artifact(artifact, index)


def unrecorded_image_settings(
    environment: Mapping[str, str], recorded: Mapping[str, str]
) -> list[str]:
    """Return image-affecting environment keys absent or mismatched in metadata."""
    missing: list[str] = []
    for key, value in sorted(environment.items()):
        if key not in IMAGE_AFFECTING_ENV_KEYS and not key.startswith(IMAGE_AFFECTING_ENV_PREFIXES):
            continue
        if recorded.get(key) != value:
            missing.append(key)
    return missing


def artifact_directory(run_id: str, candidate_id: str, index: int) -> str:
    """Return a stable relative artifact path for one candidate attempt."""
    for name, value in (("run_id", run_id), ("candidate_id", candidate_id)):
        if not isinstance(value, str) or not _SAFE_COMPONENT.fullmatch(value):
            raise ValueError(f"{name} contains unsafe artifact-path characters")
    if isinstance(index, bool) or not isinstance(index, int) or index < 0 or index > 99:
        raise ValueError("artifact index must be an integer from 0 through 99")
    return f"{run_id}/{candidate_id}/{index:02d}"


def create_artifact_directory(
    root: Path, run_id: str, candidate_id: str, index: int
) -> Path:
    """Create one audit directory and refuse to overwrite an earlier capture."""
    destination = root / artifact_directory(run_id, candidate_id, index)
    try:
        destination.mkdir(parents=True, exist_ok=False)
    except FileExistsError as error:
        raise ArtifactExistsError(f"quality artifact already exists: {destination}") from error
    return destination


def sha256_file(path: Path) -> str:
    """Hash a binary or config in chunks for reproducible provenance recording."""
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def inherited_image_settings(environment: Mapping[str, str] | None = None) -> dict[str, str]:
    """Collect image-affecting process settings for explicit manifest recording."""
    source = os.environ if environment is None else environment
    return {
        key: value
        for key, value in sorted(source.items())
        if key in IMAGE_AFFECTING_ENV_KEYS or key.startswith(IMAGE_AFFECTING_ENV_PREFIXES)
    }
