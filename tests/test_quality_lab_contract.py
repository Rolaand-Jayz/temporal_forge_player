#!/usr/bin/env python3
"""M0 contract tests for auditable Temporal Forge quality artifacts.

These tests are intentionally written before the M0 implementation. They define
the evidence required by the baseline gate: provenance, independent dimensions,
stage declarations, explicit settings, and non-overwriting artifact identity.
"""

from __future__ import annotations

import json
import os
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tests" / "fixtures" / "m0_baseline_valid.json"


class QualityLabContractTests(unittest.TestCase):
    """Pin the M0 acceptance contract independently of its implementation."""

    @classmethod
    def setUpClass(cls) -> None:
        """Load the immutable fixture shared by the schema tests."""
        with FIXTURE.open("r", encoding="utf-8") as stream:
            cls.valid_manifest = json.load(stream)

    def test_valid_baseline_manifest_contains_auditable_provenance(self) -> None:
        """A baseline must identify code, binary, config, corpus, and timing."""
        from benchmarks.quality_sweeps.quality_lab_contract import validate_manifest

        validate_manifest(self.valid_manifest)

    def test_dimensions_are_independent_and_reject_ambiguous_relabeling(self) -> None:
        """Source/model/history/display dimensions cannot be collapsed or relabeled."""
        from benchmarks.quality_sweeps.quality_lab_contract import ManifestError, validate_manifest

        invalid = json.loads(json.dumps(self.valid_manifest))
        invalid["dimensions"]["display"] = {"width": 1278, "height": 720}
        invalid["dimensions"]["model"] = {"width": 1278, "height": 720}
        with self.assertRaises(ManifestError):
            validate_manifest(invalid)

    def test_stage_contract_requires_declared_dimensions_format_and_finite_check(self) -> None:
        """Every recorded stage must be inspectable or fail schema validation."""
        from benchmarks.quality_sweeps.quality_lab_contract import ManifestError, validate_manifest

        invalid = json.loads(json.dumps(self.valid_manifest))
        del invalid["artifacts"][0]["finiteChecked"]
        with self.assertRaises(ManifestError):
            validate_manifest(invalid)

    def test_image_affecting_environment_must_be_recorded(self) -> None:
        """A launch cannot inherit image-affecting settings silently."""
        from benchmarks.quality_sweeps.quality_lab_contract import unrecorded_image_settings

        env = {
            "TFORGE_QUALITY_FRAME": "48",
            "TFORGE_FSR4_POSTPASS_EXPOSURE": "0.25",
            "PATH": os.environ.get("PATH", ""),
        }
        recorded = {"TFORGE_QUALITY_FRAME": "48"}
        self.assertEqual(
            unrecorded_image_settings(env, recorded),
            ["TFORGE_FSR4_POSTPASS_EXPOSURE"],
        )

    def test_artifact_names_are_unique_and_immutable(self) -> None:
        """Repeated captures receive distinct stable paths instead of overwriting."""
        from benchmarks.quality_sweeps.quality_lab_contract import artifact_directory

        first = artifact_directory("m0-control-20260822", "current", 0)
        second = artifact_directory("m0-control-20260822", "current", 1)
        self.assertNotEqual(first, second)
        self.assertEqual(first, "m0-control-20260822/current/00")
        self.assertEqual(second, "m0-control-20260822/current/01")

    def test_existing_artifact_is_never_overwritten(self) -> None:
        """The materializer must fail when an audit directory already exists."""
        from benchmarks.quality_sweeps.quality_lab_contract import ArtifactExistsError, create_artifact_directory

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            create_artifact_directory(root, "m0-control-20260822", "current", 0)
            with self.assertRaises(ArtifactExistsError):
                create_artifact_directory(root, "m0-control-20260822", "current", 0)


if __name__ == "__main__":
    unittest.main()
