"""Regression tests for review-harness capture identity and resume safety."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / "benchmarks/quality_sweeps/run_harness_campaign.py"
AUDIT = ROOT / "tools/quality_campaign_evidence_audit.py"


class HarnessCaptureContractTests(unittest.TestCase):
    def test_historical_audit_is_data_only(self) -> None:
        source = (ROOT / "tools/audit_quality_campaign_evidence.py").read_text(encoding="utf-8")
        self.assertIn('"image_payloads_required": False', source)
        self.assertIn('INCOMPLETE PROVENANCE', source)
        self.assertIn('RECAPTURE REQUIRED', source)
        self.assertNotIn("png_size", source)

    def test_nativeaa_cas_arms_are_independent_runner_invocations(self) -> None:
        source = RUNNER.read_text(encoding="utf-8")
        self.assertIn('native_root = pair_root / f"nativeaa_{placement}"', source)
        self.assertIn('"--cas-placement", placement', source)
        self.assertIn('native_roots[placement] = native_root', source)

    def test_native_assets_use_recorded_fsr_output_not_decoded_source(self) -> None:
        source = RUNNER.read_text(encoding="utf-8")
        self.assertIn("def native_output(scene_root: Path)", source)
        self.assertIn("export_native(runner, native_output(scene_root)", source)
        self.assertIn('"-i", str(source), "-frames:v", "1"', source)
        self.assertNotIn('runner.run(["cp", str(source), str(output)]', source)

    def test_resume_requires_versioned_marker_and_experiment_records(self) -> None:
        source = RUNNER.read_text(encoding="utf-8")
        self.assertIn('data.get("schemaVersion") != 2', source)
        self.assertIn('data.get("runnerVersion") != 2', source)
        self.assertIn('record = trace.parent / "experiment.json"', source)
        self.assertIn('"runtime_trace_path"', source)
        self.assertIn('configuration provenance mismatch', source)
        self.assertIn('quality profile mismatch', source)
        self.assertIn('alias_of=direct_record.get("experiment_id")', source)


if __name__ == "__main__":
    unittest.main()
