"""Regression tests for unified campaign/harness capture identity and safety."""

from contextlib import redirect_stdout
from io import StringIO
from pathlib import Path
from unittest import mock
import json
import sys
import unittest


ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / "benchmarks/quality_sweeps/run_harness_campaign.py"
NEW_ENTRY = ROOT / "benchmarks/quality_sweeps/run_quality_campaign_capture.py"
PLAN = ROOT / "benchmarks/quality_sweeps/quality_campaign_capture_plan.json"


class HarnessCaptureContractTests(unittest.TestCase):
    def test_capture_plan_is_exact_and_has_two_consumers(self) -> None:
        data = json.loads(PLAN.read_text(encoding="utf-8"))
        self.assertEqual(data["schema"], "temporal_forge.quality_campaign_capture_plan.v2")
        self.assertEqual(data["campaignGeneration"], "post_lattice_fix_canonical_v1")
        self.assertEqual(data["pipelineRevision"], "temporal_lattice_fix_closeout")
        pairs = {(item["inputHeight"], item["outputHeight"]) for item in data["pairs"]}
        self.assertEqual(pairs, {
            (360, 480), (360, 720), (360, 1080),
            (480, 720), (480, 1080), (480, 1440),
            (720, 1080), (720, 1440), (720, 2160),
            (1080, 1440), (1080, 2160),
        })
        self.assertEqual(len(data["scenes"]), 4)
        self.assertTrue(all(item["uses"] == ["quality_campaign", "review_harness"] for item in data["pairs"]))
        self.assertEqual(data["downsamplingArms"], [
            {"id": "resolve_cas20", "rendererCas": 0.2, "afterDownsamplingCas": 0.0,
             "label": "CAS 0.20 before downsampling"},
            {"id": "external_post_cas20", "rendererCas": 0.0, "afterDownsamplingCas": 0.2,
             "label": "CAS 0.20 after downsampling"},
            {"id": "no_cas", "rendererCas": 0.0, "afterDownsamplingCas": 0.0,
             "label": "No CAS sharpening"},
        ])
        self.assertNotIn("540", PLAN.read_text(encoding="utf-8"))

    def test_new_entry_defaults_to_plan_only_without_starting_capture(self) -> None:
        from benchmarks.quality_sweeps import run_harness_campaign

        output = StringIO()
        with mock.patch.object(sys, "argv", [str(NEW_ENTRY)]), \
             mock.patch.object(run_harness_campaign.subprocess, "Popen", side_effect=AssertionError("capture started")), \
             redirect_stdout(output):
            self.assertEqual(run_harness_campaign.main(), 0)
        preview = json.loads(output.getvalue())
        self.assertEqual(preview["mode"], "plan_only")
        self.assertFalse(preview["capture_started"])
        self.assertEqual(preview["pair_count"], 11)

    def test_execution_has_explicit_gate_and_clean_tree_preflight(self) -> None:
        source = RUNNER.read_text(encoding="utf-8")
        self.assertIn('parser.add_argument("--execute"', source)
        self.assertLess(source.index("if not args.execute:"), source.index("runner = PausingRunner"))
        self.assertIn("live capture requires a committed tracked worktree", source)
        self.assertIn('"capture_started": False', source)

    def test_nativeaa_is_one_real_scale_per_placement(self) -> None:
        supersampling = (ROOT / "benchmarks/quality_sweeps/run_fsr_supersampling.py").read_text(encoding="utf-8")
        source = RUNNER.read_text(encoding="utf-8")
        self.assertIn('elif args.preset == "NativeAA"', supersampling)
        self.assertIn("requested_scales = (2.0,)", supersampling)
        self.assertIn('arm.startswith("nativeaa_")', source)
        self.assertIn("expected_player_launches_per_pair", source)

    def test_downsampling_cas_arms_are_independent_and_runtime_verified(self) -> None:
        source = (ROOT / "benchmarks/quality_sweeps/run_fsr_supersampling.py").read_text(encoding="utf-8")
        self.assertIn('args.cas_placement in ("post", "none")', source)
        self.assertIn('env["TFORGE_FSR4_CAS_STRENGTH"] = "0.00"', source)
        self.assertIn('if args.cas_placement == "post":', source)
        self.assertIn('f"cas=strength={args.cas_strength}"', source)
        self.assertIn('expected_cas_stage = "integrated_post_reconstruction" if cas_enabled else "none"', source)
        self.assertIn('args.cas_strength if args.cas_placement != "none" else "0.00"', source)

    def test_one_capture_publishes_campaign_and_harness_assets(self) -> None:
        source = RUNNER.read_text(encoding="utf-8")
        self.assertIn("export_review_image(", source)
        self.assertIn("write_catalog(harness, asset_records + native_records)", source)
        self.assertIn('"campaign_id": CAPTURE_PLAN["campaignId"]', source)
        self.assertIn('"quality_campaign", "review_harness"', PLAN.read_text(encoding="utf-8"))

    def test_capture_provenance_ignores_only_its_generated_harness_outputs(self) -> None:
        source = (ROOT / "benchmarks/quality_sweeps/run_fsr_supersampling.py").read_text(encoding="utf-8")
        self.assertIn('":[exclude)review_harness/catalog.js"'.replace("[", "("), source)
        self.assertIn('":[exclude)review_harness/images/**"'.replace("[", "("), source)
        self.assertIn('"diff", "--cached", "--quiet"', source)

    def test_export_and_reference_work_avoid_redundant_processes(self) -> None:
        source = RUNNER.read_text(encoding="utf-8")
        supersampling = (ROOT / "benchmarks/quality_sweeps/run_fsr_supersampling.py").read_text(encoding="utf-8")
        self.assertIn('"--reference-cache"', source)
        self.assertIn("reference_cache", supersampling)
        self.assertIn("shutil.copyfile(source, output)", source)
        self.assertIn("split=3", source)

    def test_resume_requires_current_marker_and_provenance(self) -> None:
        source = RUNNER.read_text(encoding="utf-8")
        self.assertIn('data.get("schemaVersion") != 3', source)
        self.assertIn('data.get("runnerVersion") != 3', source)
        self.assertIn('record = trace.parent / "experiment.json"', source)
        self.assertIn('"runtime_trace_path"', source)
        self.assertIn("source-size scale clamp", (
            ROOT / "benchmarks/quality_sweeps/run_fsr_supersampling.py"
        ).read_text(encoding="utf-8"))

    def test_historical_roots_are_explicitly_noncanonical(self) -> None:
        for relative in (
            "benchmarks/quality_sweeps/quality_campaign_capture/HISTORICAL_ONLY.json",
            "review_harness/HISTORICAL_ONLY.json",
        ):
            marker = json.loads((ROOT / relative).read_text(encoding="utf-8"))
            self.assertFalse(marker["canonical"])
            self.assertTrue(marker["historical_only"])


if __name__ == "__main__":
    unittest.main()
