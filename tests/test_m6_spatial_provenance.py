"""Failing-first tests for exact M6 spatial execution provenance."""

from __future__ import annotations

import copy
import json
import hashlib
import unittest
from pathlib import Path

from benchmarks.quality_sweeps.campaign_provenance import (
    ProvenanceError,
    validate_execution_provenance,
)


ROOT = Path(__file__).resolve().parents[1]
CAMPAIGN_PATH = ROOT / "benchmarks/quality_sweeps/m6_schema2_spatial_campaign.json"
RESULTS_PATH = Path("/tmp/tforge-m6-spatial/retry-20260823T113531Z/campaign-results.json")


def _load() -> tuple[dict, list]:
    return (
        json.loads(CAMPAIGN_PATH.read_text(encoding="utf-8")),
        json.loads(RESULTS_PATH.read_text(encoding="utf-8")),
    )


class M6SpatialProvenanceTests(unittest.TestCase):
    def test_retry_results_equal_campaign_and_on_disk_binary_config_provenance(self) -> None:
        campaign, results = _load()
        validate_execution_provenance(campaign, results, ROOT)

    def test_stale_manifest_binary_is_rejected_even_when_recorded_hashes_look_plausible(self) -> None:
        campaign, results = _load()
        stale = copy.deepcopy(campaign)
        stale["executionProvenance"]["executedBinary"] = str(ROOT / "build-fast/temporal_forge_player")
        stale["executionProvenance"]["executedBinarySha256"] = hashlib.sha256(
            (ROOT / "build-fast/temporal_forge_player").read_bytes()
        ).hexdigest()
        with self.assertRaisesRegex(ProvenanceError, "different binary|binary provenance"):
            validate_execution_provenance(stale, results, ROOT)

    def test_ambiguous_mixed_binary_results_are_rejected(self) -> None:
        campaign, results = _load()
        ambiguous = copy.deepcopy(results)
        ambiguous[1]["binary"] = str(ROOT / "build-fast/temporal_forge_player")
        with self.assertRaisesRegex(ProvenanceError, "different binary"):
            validate_execution_provenance(campaign, ambiguous, ROOT)

    def test_unrecorded_git_is_not_replaced_by_current_head(self) -> None:
        campaign, results = _load()
        ambiguous = copy.deepcopy(campaign)
        ambiguous["executionProvenance"]["gitCommitStatus"] = "recorded"
        ambiguous["executionProvenance"]["gitCommit"] = ambiguous["executionProvenance"]["currentGitHead"]
        with self.assertRaisesRegex(ProvenanceError, "Git provenance|gitCommit"):
            validate_execution_provenance(ambiguous, results, ROOT)


if __name__ == "__main__":
    unittest.main()
