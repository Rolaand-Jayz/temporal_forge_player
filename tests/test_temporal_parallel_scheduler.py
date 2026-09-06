"""Contract tests for bounded, retry-safe temporal capture scheduling."""

from pathlib import Path
import unittest


SCRIPT = Path(__file__).parents[1] / ".m6-captures/m6-authoritative-29339fc-20260823/run_parallel_temporal.py"


class TemporalParallelSchedulerContractTests(unittest.TestCase):
    def test_scheduler_defaults_to_two_bounded_workers_and_allows_serial_override(self) -> None:
        source = SCRIPT.read_text(encoding="utf-8")
        self.assertIn('os.environ.get("TFORGE_CAPTURE_WORKERS", "2")', source)
        self.assertIn("ThreadPoolExecutor(max_workers=MAX_WORKERS)", source)

    def test_retry_uses_a_distinct_root_and_revalidates_before_acceptance(self) -> None:
        source = SCRIPT.read_text(encoding="utf-8")
        self.assertIn("RETRY_ROOT / job_id(candidate, row)", source)
        self.assertIn("retry-discover", source)
        self.assertIn("if not valid(root, candidate, row)[0]", source)

    def test_workers_enable_capture_only_fence_timeout(self) -> None:
        source = SCRIPT.read_text(encoding="utf-8")
        self.assertIn('"TFORGE_FSR4_FENCE_TIMEOUT_MS": "5000"', source)

    def test_temporal_evidence_jobs_disable_hw_decode_by_default_for_motion_sidecars(self) -> None:
        source = SCRIPT.read_text(encoding="utf-8")
        self.assertIn('TFORGE_CAPTURE_DISABLE_HW_DECODE', source)
        self.assertIn('"TFORGE_DISABLE_HW_DECODE":', source)


if __name__ == "__main__":
    unittest.main()
