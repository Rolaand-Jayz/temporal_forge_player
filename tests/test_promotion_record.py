"""Test-first contract for the M7.5 promotion/rollback record.

Promotion is a release decision, not a side effect of a green build. The
record must preserve the prior control, hashes, evidence, and limitations; a
failed quality/equivalence gate must produce an explicit non-promotion record.
"""

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools" / "record_quality_promotion.py"


class PromotionRecordTests(unittest.TestCase):
    def run_tool(self, evidence):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            evidence_path = directory / "evidence.json"
            output_path = directory / "promotion.json"
            evidence_path.write_text(json.dumps(evidence))
            result = subprocess.run(
                [sys.executable, str(TOOL), "--evidence", str(evidence_path),
                 "--output", str(output_path)],
                text=True, capture_output=True, check=False,
            )
            record = json.loads(output_path.read_text()) if output_path.exists() else None
            return result, record

    def base_evidence(self):
        return {
            "schema": "temporal_forge.m7_promotion_evidence.v1",
            "campaignId": "m7-final",
            "candidateId": "candidate-a",
            "referenceCandidateId": "base-only-bilinear",
            "quality": {"passed": True, "artifact": "quality.json"},
            "equivalence": {"passed": True, "artifact": "equivalence.json"},
            "performance": {"passed": True, "artifact": "performance.csv"},
            "diagnostics": {"disabledByDefault": True},
            "rollback": {"configPath": "known-good-config.json", "candidateId": "base-only-bilinear"},
            "binary": {"path": "player", "sha256": "abc"},
            "artifacts": ["quality.json", "equivalence.json", "performance.csv"],
            "limitations": ["AMD/Vulkan validated only"],
        }

    def test_failed_gate_is_explicitly_not_promoted(self):
        evidence = self.base_evidence()
        evidence["quality"]["passed"] = False
        evidence["quality"]["reason"] = "candidate trails baseline"
        result, record = self.run_tool(evidence)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(record["status"], "not_promoted")
        self.assertIn("candidate trails baseline", record["reasons"])
        self.assertEqual(record["rollback"]["candidateId"], "base-only-bilinear")

    def test_promotion_requires_every_gate_and_rollback(self):
        evidence = self.base_evidence()
        evidence["rollback"] = {}
        result, record = self.run_tool(evidence)
        self.assertEqual(result.returncode, 2)
        self.assertIsNone(record)


if __name__ == "__main__":
    unittest.main()
