"""M5 tests for explicit jitter policy provenance."""

from __future__ import annotations

import json
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tests" / "fixtures" / "m0_baseline_valid.json"


class JitterManifestContractTests(unittest.TestCase):
    """A sequence capture must identify the jitter policy that produced it."""

    @classmethod
    def setUpClass(cls) -> None:
        with FIXTURE.open("r", encoding="utf-8") as stream:
            cls.manifest = json.load(stream)

    def test_supported_modes_and_controlled_strength_are_valid(self) -> None:
        from benchmarks.quality_sweeps.quality_lab_contract import validate_manifest

        manifest = json.loads(json.dumps(self.manifest))
        manifest["jitter"] = {"mode": "controlled", "controlledStrength": 0.25}
        validate_manifest(manifest)

    def test_unknown_mode_is_rejected(self) -> None:
        from benchmarks.quality_sweeps.quality_lab_contract import ManifestError, validate_manifest

        manifest = json.loads(json.dumps(self.manifest))
        manifest["jitter"] = {"mode": "invented"}
        with self.assertRaises(ManifestError):
            validate_manifest(manifest)


if __name__ == "__main__":
    unittest.main()
