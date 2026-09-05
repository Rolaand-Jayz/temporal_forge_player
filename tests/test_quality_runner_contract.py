"""M0 capture-isolation contract tests.

These tests are written before the runner change.  A quality capture must use
the checked-in neutral benchmark settings and must not inherit a reviewer or
developer's persisted UI settings through XDG_CONFIG_HOME.
"""

from __future__ import annotations

import json
import hashlib
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / "benchmarks" / "video_corpus" / "run_quality.sh"
BENCHMARK_SETTINGS = ROOT / "benchmarks" / "video_corpus" / "benchmark_settings.json"
SPATIAL_MANIFEST = ROOT / "benchmarks" / "quality_sweeps" / "m6_6_1_real_spatial_controls.json"


def _write_candidate_artifacts(
    candidate_root: Path, candidate_id: str, result_paths: list[str]
) -> None:
    """Create the non-empty sweep files needed by campaign validation."""
    run_root = candidate_root / "run"
    run_root.mkdir(parents=True)
    (run_root / "results.json").write_text("[]\n", encoding="utf-8")
    (run_root / "rankings.csv").write_text("candidateId\ncurrent\n", encoding="utf-8")
    (run_root / "rankings.json").write_text("[]\n", encoding="utf-8")
    csv_path = run_root / "quality.csv"
    csv_path.write_text("clip_id\nclip\n", encoding="utf-8")
    still_path = run_root / "still.png"
    still_path.write_bytes(b"still")
    result = {
        "candidateId": candidate_id,
        "exitCode": 0,
        "csv": str(csv_path),
        "metrics": {"rowCount": 1},
        "representativeStillPaths": [str(still_path)],
        "reviewAssets": [
            {
                "scene": "tos_daylight",
                "frame": 48,
                "path": str(still_path),
                "width": 1920,
                "height": 1080,
            }
        ],
    }
    for relative_path in result_paths:
        result_path = run_root / relative_path
        result_path.parent.mkdir(parents=True, exist_ok=True)
        result_path.write_text(json.dumps(result), encoding="utf-8")


class QualityRunnerContractTests(unittest.TestCase):
    def test_runtime_trace_rejects_jitter_state_mismatch(self) -> None:
        from benchmarks.quality_sweeps.run_fsr_supersampling import validate_runtime_trace

        trace = {
            "schema": "temporal_forge.runtime_pipeline.v1",
            "run_id": "run-1", "quality_profile": "AMD_SEMANTIC_BASELINE",
            "source_resolution": "640x360", "presentation_resolution": "1280x720",
            "requested_force_viewport": "1280x720", "requested_force_scale": "2.00",
            "jitter_mode": "off", "jitter_enabled": False,
            "cas_enabled": True, "binary_sha256": "bin", "config_sha256": "config",
            "git_head": "head", "git_dirty": False,
            "reconstruction_resolution": "640x360", "requested_model_resolution": "640x360",
            "scale_clamped_to_source": False, "effective_scale": 2.0,
        }
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "trace.json"
            path.write_text(json.dumps(trace), encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "jitter_mode"):
                validate_runtime_trace(path, run_id="run-1", source="640x360",
                                       output="1280x720", scale=2.0, cas_enabled=True,
                                       binary_sha256="bin", git_head="head", git_dirty=False,
                                       config_sha256="config",
                                       profile="AMD_SEMANTIC_BASELINE")

    def test_runtime_trace_rejects_contradictory_requested_source_tap_profile(self) -> None:
        """A legacy request flag cannot masquerade as the effective resolve source."""
        from benchmarks.quality_sweeps.run_fsr_supersampling import validate_runtime_trace

        evidence = ROOT / "benchmarks/quality_sweeps/lattice_p0_recurrent_qualification_20260903/candidate_cave720.runtime_pipeline.json"
        trace = json.loads(evidence.read_text(encoding="utf-8"))
        trace["prepass_resolve_source"] = "source_display"
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "contradictory.json"
            path.write_text(json.dumps(trace), encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "prepass_resolve_source"):
                validate_runtime_trace(
                    path,
                    run_id=trace["run_id"], source="1280x720", output="1920x1080",
                    scale=2.0, cas_enabled=False,
                    binary_sha256=trace["binary_sha256"], git_head=trace["git_head"],
                    git_dirty=trace["git_dirty"], profile="AMD_SEMANTIC_BASELINE",
                    config_sha256=trace["config_sha256"], expected_cas_strength=0.0,
                )

    def test_candidate_environment_is_declared_and_not_inherited(self) -> None:
        """Ambient quality switches must not silently change a candidate."""
        from benchmarks.quality_sweeps.run_quality_sweep import build_candidate_environment

        environment = build_candidate_environment(
            {
                "PATH": "/usr/bin",
                "DISPLAY": ":0",
                "TFORGE_FSR4_DISABLE_CAS": "1",
                "TFORGE_REVIEW_FSR_CAS": "0.90",
                "TFORGE_DISABLE_HW_DECODE": "ambient",
            },
            {
                "environment": {
                    "TFORGE_FSR4_ENABLE_COLOR_HISTORY": "1",
                    "TFORGE_FSR4_ENABLE_RECURRENT": "1",
                    "TFORGE_FSR4_CAS_STRENGTH": "0.04",
                    "TFORGE_DISABLE_HW_DECODE": "1",
                    "TFORGE_REVIEW_PIPELINE_CONFIG": "/dev/null",
                }
            },
            {"environment": {"TFORGE_FSR4_LEARNED_STRENGTH": "0.25"}},
        )

        self.assertEqual(environment["PATH"], "/usr/bin")
        self.assertEqual(environment["DISPLAY"], ":0")
        self.assertNotIn("TFORGE_FSR4_DISABLE_CAS", environment)
        self.assertNotIn("TFORGE_REVIEW_FSR_CAS", environment)
        self.assertEqual(environment["TFORGE_DISABLE_HW_DECODE"], "1")
        self.assertEqual(environment["TFORGE_FSR4_LEARNED_STRENGTH"], "0.25")

    def test_candidate_environment_rejects_unscoped_process_settings(self) -> None:
        """A campaign cannot smuggle arbitrary process state into the player."""
        from benchmarks.quality_sweeps.run_quality_sweep import build_candidate_environment

        with self.assertRaisesRegex(ValueError, "unsupported candidate environment"):
            build_candidate_environment(
                {"PATH": "/usr/bin"},
                {"environment": {"LD_PRELOAD": "/tmp/invented.so"}},
                {},
            )

    def test_sweep_exposes_bounded_parallel_workers_and_retry_pass(self) -> None:
        """The capture runner must support isolated parallel work and retries."""
        source = (ROOT / "benchmarks/quality_sweeps/run_quality_sweep.py").read_text(
            encoding="utf-8"
        )
        self.assertIn("ThreadPoolExecutor", source)
        self.assertIn("TFORGE_CAPTURE_WORKERS", source)
        self.assertIn("retry", source.lower())

    def test_campaign_wrapper_parallelizes_independent_candidate_sweeps(self) -> None:
        """The campaign layer must overlap one-candidate sweep processes."""
        source = (ROOT / "benchmarks/quality_sweeps/run_quality_campaign.py").read_text(
            encoding="utf-8"
        )
        self.assertIn("ThreadPoolExecutor", source)
        self.assertIn("TFORGE_CAPTURE_WORKERS", source)
        self.assertIn("as_completed", source)
        self.assertIn('"--retries"', source)
        self.assertIn('"--retries", str(args.retries)', source)

    def test_quality_runner_isolates_generated_reference_images(self) -> None:
        """Parallel candidates must never share a reference file while writing it."""
        source = (ROOT / "benchmarks/video_corpus/run_quality.sh").read_text(
            encoding="utf-8"
        )
        self.assertIn("reference_${output_width}x${output_height}_f${frame_index}${tag_suffix}.png", source)
        self.assertIn("f${frame_index}${tag_suffix}_${class_suffix}.png", source)

    def test_runner_forwards_capture_identity_environment(self) -> None:
        """Capture controls must survive the runner's environment whitelist."""
        source = (ROOT / "benchmarks/video_corpus/run_quality.sh").read_text(
            encoding="utf-8"
        )
        for name in (
            "TFORGE_DISABLE_HW_DECODE",
            "TFORGE_FSR4_JITTER_MODE",
            "TFORGE_FSR4_CONTROLLED_JITTER",
            "TFORGE_FSR4_ENABLE_COLOR_HISTORY",
            "TFORGE_FSR4_ENABLE_RECURRENT",
            "TFORGE_FSR4_CAS_STRENGTH",
            "TFORGE_EXPERIMENT_ID",
            "TFORGE_RUNTIME_TRACE_PATH",
            "TFORGE_GIT_HEAD",
            "TFORGE_GIT_DIRTY",
            "TFORGE_CONFIG_SHA256",
        ):
            self.assertIn(name, source)

    def test_explicit_cas_survives_review_config_defaults(self) -> None:
        """An arm's CAS value must win over review-only config defaults."""
        source = RUNNER.read_text(encoding="utf-8")
        caller_capture = source.index('caller_review_fsr_cas="${TFORGE_REVIEW_FSR_CAS:-}"')
        source_config = source.index('source "$review_pipeline_config"')
        override = source.index('benchmark_env+=("TFORGE_FSR4_CAS_STRENGTH=$caller_review_fsr_cas")')
        config_fallback = source.index('benchmark_env+=("TFORGE_FSR4_CAS_STRENGTH=${TFORGE_REVIEW_FSR_CAS}")')
        self.assertLess(caller_capture, source_config)
        self.assertLess(source_config, override)
        self.assertLess(override, config_fallback)
        self.assertIn('elif [[ -n "$caller_fsr4_cas_strength" ]]', source)

    def test_runner_separates_semantic_baseline_from_jitter_diagnostic(self) -> None:
        source = (ROOT / "benchmarks/quality_sweeps/run_fsr_supersampling.py").read_text(
            encoding="utf-8"
        )
        self.assertIn('"AMD_SEMANTIC_BASELINE", "JITTER_OFF_DIAGNOSTIC"', source)
        self.assertIn('default="AMD_SEMANTIC_BASELINE"', source)
        self.assertIn('"TFORGE_FSR4_INTEGRATED_BEST_FINDINGS": "1"', source)
        self.assertIn('"TFORGE_FSR4_ENABLE_COLOR_HISTORY": "1"', source)
        self.assertIn('"TFORGE_FSR4_ENABLE_RECURRENT": "1"', source)
        self.assertIn('"quality_profile"', (ROOT / "src/core/PlaybackEngine.cpp").read_text(encoding="utf-8"))
        self.assertIn('"config_sha256"', (ROOT / "src/core/PlaybackEngine.cpp").read_text(encoding="utf-8"))

    def test_semantic_validator_requires_effective_color_and_jitter_contract(self) -> None:
        source = (ROOT / "benchmarks/quality_sweeps/run_fsr_supersampling.py").read_text(
            encoding="utf-8"
        )
        for field in (
            '"prepass_resolve_source": "model_color"',
            '"prepass_resolve_stage": "prepass_input_resolve"',
            '"model_color_transfer": "eotf_mulaw_pretransformed"',
            '"model_color_format": "rgb10_a2"',
            '"mulaw_application_stage": "yuv_to_model_color"',
            '"source_display_used_for_current_resolve": False',
            '"jitter_stage": "prepass_input_resolve"',
        ):
            self.assertIn(field, source)

        playback = (ROOT / "src/core/PlaybackEngine.cpp").read_text(encoding="utf-8")
        for field in (
            'trace["prepass_resolve_source"]',
            'trace["model_color_format"]',
            'trace["mulaw_application_stage"]',
            'trace["source_display_used_for_current_resolve"]',
            'trace["requested_source_tap_mulaw_profile"]',
        ):
            self.assertIn(field, playback)

    def test_runtime_rejection_persists_failed_arm_provenance(self) -> None:
        source = (ROOT / "benchmarks/quality_sweeps/run_fsr_supersampling.py").read_text(
            encoding="utf-8"
        )
        self.assertIn('"status": "failed"', source)
        self.assertIn('"failure_reason": str(error)', source)
        self.assertIn('os.replace(temporary_record, scene_root / "experiment.json")', source)

    def test_spatial_controls_use_the_captured_gpu_decode_at_the_target_frame(self) -> None:
        """FSR and standalone controls must start from identical decoded pixels."""
        source = RUNNER.read_text(encoding="utf-8")
        self.assertIn("TFORGE_FSR4_DUMP_RAW=1", source)
        self.assertIn("TFORGE_FSR4_DUMP_RAW_PATH=", source)
        self.assertIn('source_raw_ppm="$frames/', source)
        self.assertIn('source_raw_png="$frames/', source)
        self.assertIn('if (( output_complete && source_complete )); then', source)
        self.assertIn('-i "$source_raw_ppm"', source)
        self.assertIn('control_source_path', source)
        self.assertIn('full_output_path', source)
        self.assertIn('magick "$source_raw_png" -alpha off -depth 8 RGB:-', source)
        # A separately decoded source frame creates a mixed-decoder metric and
        # is not an acceptable Lanczos/Bicubic control for the GPU path.
        self.assertNotIn(
            '-i "$path" \\\n+        -vf "select=eq(n\\,${frame_index}),scale=${output_width}:${output_height}:flags=lanczos"',
            source,
        )

    def test_player_raw_dump_is_frame_and_path_addressable(self) -> None:
        """The runner must be able to capture raw input matching its FSR frame."""
        playback = (ROOT / "src/core/PlaybackEngine.cpp").read_text(encoding="utf-8")
        self.assertIn("TFORGE_FSR4_DUMP_RAW_PATH", playback)
        self.assertIn("df.frameIndex >= dumpOutputFrame", playback)
        self.assertIn("std::ofstream dump(dumpRawPath", playback)
        self.assertIn("sourceFrameIndex = fsrFrame->frameIndex", playback)
        self.assertIn("firstUploader->readbackRaw", playback)
        self.assertIn("!dumpRawEnv", playback)

    def test_runner_requires_exact_complete_p6_payloads(self) -> None:
        """A partial, malformed, or overlong dump cannot become metric input."""
        source = RUNNER.read_text(encoding="utf-8")
        self.assertIn('$output_magic" == "P6"', source)
        self.assertIn('$output_max_value" == "255"', source)
        self.assertIn('output_bytes == expected_output_bytes', source)
        self.assertIn('$source_magic" == "P6"', source)
        self.assertIn('$source_max_value" == "255"', source)
        self.assertIn('source_bytes == expected_source_bytes', source)

    """Keep benchmark launches independent from the interactive app state."""

    def test_runner_installs_neutral_settings_before_launch(self) -> None:
        """The shell runner must replace, not inherit, the user's settings file."""
        source = RUNNER.read_text(encoding="utf-8")
        self.assertIn("benchmark_config_home", source)
        self.assertIn("XDG_CONFIG_HOME=", source)
        self.assertIn("benchmark_settings.json", source)
        self.assertIn("temporal-forge-player/settings.json", source)

    def test_neutral_settings_are_pixel_neutral(self) -> None:
        """The checked-in benchmark settings must not contain UI color tuning."""
        settings = json.loads(BENCHMARK_SETTINGS.read_text(encoding="utf-8"))
        self.assertEqual(settings["brightness"], 0.0)
        self.assertEqual(settings["contrast"], 0.0)
        self.assertEqual(settings["saturation"], 0.0)
        self.assertEqual(settings["hue"], 0.0)
        self.assertEqual(settings["gamma"], 1.0)
        self.assertEqual(settings["backend"], "Fsr4ReExperimental")
        self.assertEqual(settings["preset"], "Quality")
        self.assertFalse(settings["fullscreen"])

    def test_m6_spatial_slice_is_real_only_and_dimensioned(self) -> None:
        """The first corrected campaign slice must exclude synthetic review families."""
        manifest = json.loads(SPATIAL_MANIFEST.read_text(encoding="utf-8"))
        self.assertEqual(manifest["baselineCandidateId"], "base_only_bilinear")
        self.assertEqual(manifest["dimensions"], "426x240")
        self.assertEqual(manifest["outputDimensions"], "1920x1080")
        self.assertEqual(
            manifest["clipRegex"],
            "^(tos_daylight|tos_debris|sintel_rooftop|sintel_cave)$",
        )
        self.assertNotIn("synthetic", manifest["clipRegex"])
        self.assertGreaterEqual(len(manifest["experiments"]), 5)

    def test_campaign_accepts_nested_attempt_result_without_a_guessed_path(self) -> None:
        """The child sweep's exact nested attempt result is selected recursively."""
        with __import__("tempfile").TemporaryDirectory() as directory:
            from benchmarks.quality_sweeps import run_quality_campaign

            candidate_root = Path(directory) / "candidate"
            _write_candidate_artifacts(candidate_root, "current", ["current/01/result.json"])

            result_path = run_quality_campaign.validate_candidate_artifacts(candidate_root, "current")

            self.assertEqual(result_path, candidate_root / "run/current/01/result.json")

    def test_campaign_rejects_ambiguous_result_artifacts(self) -> None:
        """Two result.json files cannot be resolved by choosing the first path."""
        with __import__("tempfile").TemporaryDirectory() as directory:
            from benchmarks.quality_sweeps import run_quality_campaign
            from benchmarks.quality_sweeps.quality_campaign_contract import CampaignError

            candidate_root = Path(directory) / "candidate"
            _write_candidate_artifacts(
                candidate_root,
                "current",
                ["current/result.json", "current/01/result.json"],
            )

            with self.assertRaisesRegex(CampaignError, "exactly one result.json"):
                run_quality_campaign.validate_candidate_artifacts(candidate_root, "current")

    def test_campaign_rejects_missing_result_artifact(self) -> None:
        """A complete-looking run without result.json is not campaign success."""
        with __import__("tempfile").TemporaryDirectory() as directory:
            from benchmarks.quality_sweeps import run_quality_campaign
            from benchmarks.quality_sweeps.quality_campaign_contract import CampaignError

            candidate_root = Path(directory) / "candidate"
            _write_candidate_artifacts(candidate_root, "current", [])

            with self.assertRaisesRegex(CampaignError, "exactly one result.json"):
                run_quality_campaign.validate_candidate_artifacts(candidate_root, "current")

    def test_shell_runner_fails_when_every_player_capture_fails(self) -> None:
        """A failed player cannot be reported as a successful empty capture."""
        with __import__("tempfile").TemporaryDirectory() as directory:
            root = Path(directory)
            failing_player = root / "failing-player"
            failing_player.write_text("#!/usr/bin/env bash\nexit 134\n", encoding="utf-8")
            failing_player.chmod(0o755)
            source = root / "sample_426x240.mp4"
            reference = root / "reference.mkv"
            source.touch()
            reference.touch()
            manifest = root / "manifest.csv"
            manifest.write_text(
                "clip_id,title,source_url,license,start_seconds,duration,width,height,quality,crf,path,reference_path\n"
                f"contract_clip,Contract clip,,,0,1,426,240,high,12,{source},{reference}\n",
                encoding="utf-8",
            )
            results = root / "quality.csv"
            tag = "contract-failed-player"
            log = ROOT / "benchmarks" / "video_corpus" / "results" / "quality_logs" / (
                f"{source.stem}_{tag}.log"
            )
            environment = os.environ.copy()
            environment.update(
                {
                    "TFORGE_QUALITY_MANIFEST": str(manifest),
                    "TFORGE_QUALITY_TAG": tag,
                    "TFORGE_REVIEW_PIPELINE_CONFIG": "/dev/null",
                    "TFORGE_QUALITY_CLIP": "^contract_clip$",
                }
            )
            try:
                result = subprocess.run(
                    [str(RUNNER), str(failing_player), "426x240", str(results)],
                    cwd=ROOT,
                    env=environment,
                    capture_output=True,
                    text=True,
                    check=False,
                )
            finally:
                log.unlink(missing_ok=True)
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_sweep_rejects_a_successful_zero_row_runner(self) -> None:
        """A zero-row child result must fail the sweep even when the child exits 0."""
        with __import__("tempfile").TemporaryDirectory() as directory:
            root = Path(directory)
            fake_binary = root / "fake-player"
            fake_binary.write_text("#!/usr/bin/env bash\nexit 0\n", encoding="utf-8")
            fake_binary.chmod(0o755)
            config = root / "quality.json"
            config.write_text("{}\n", encoding="utf-8")
            manifest = root / "manifest.json"
            manifest.write_text(
                json.dumps(
                    {
                        "dimensions": "426x240",
                        "outputDimensions": "1920x1080",
                        "frame": 48,
                        "quality": "high",
                        "experiments": [{"id": "empty-capture", "config": "quality.json"}],
                    }
                ),
                encoding="utf-8",
            )
            fake_runner = root / "fake-quality-runner"
            fake_runner.write_text(
                "#!/usr/bin/env bash\n"
                "printf 'clip_id,fsr_psnr_db,fsr_ssim,fsr_edge_ssim\\n' > \"$3\"\n"
                "exit 0\n",
                encoding="utf-8",
            )
            fake_runner.chmod(0o755)
            output_root = root / "output"
            argv = [
                "run_quality_sweep.py",
                "--manifest",
                str(manifest),
                "--binary",
                str(fake_binary),
                "--output-root",
                str(output_root),
                "--tag-prefix",
                "contract-empty-capture",
            ]
            quality_sweeps_path = str(ROOT / "benchmarks" / "quality_sweeps")
            with mock.patch.object(sys, "path", [quality_sweeps_path, *sys.path]):
                from benchmarks.quality_sweeps import run_quality_sweep

                with mock.patch.object(run_quality_sweep, "QUALITY_RUNNER", fake_runner):
                    with mock.patch.object(sys, "argv", argv):
                        self.assertEqual(run_quality_sweep.main(), 2)

    def test_campaign_rejects_successful_child_without_artifacts(self) -> None:
        """The campaign wrapper must not turn a zero-exit empty child into success."""
        with __import__("tempfile").TemporaryDirectory() as directory:
            root = Path(directory)
            binary_digest = hashlib.sha256(b"player\n").hexdigest()
            config_digest = hashlib.sha256(b"{}\n").hexdigest()
            campaign = root / "campaign.json"
            campaign.write_text(
                json.dumps(
                    {
                        "schemaVersion": 2,
                        "campaignId": "empty-campaign",
                        "baselineCandidateId": "candidate",
                        "corpus": {
                            "manifestPath": "manifest.csv",
                            "selection": ["tos_daylight"],
                        },
                        "classes": ["daylight"],
                        "requiredMetrics": [
                            "static_flicker",
                            "edge_variance",
                            "motion_compensated_error",
                            "ghost_duration_frames",
                            "reset_recovery_frames",
                        ],
                        "candidates": [
                            {
                                "id": "candidate",
                                "configPath": "config.json",
                                "binarySha256": "a" * 64,
                                "dimensions": {"source": "426x240", "output": "1920x1080"},
                                "reviewAssets": [
                                    {
                                        "scene": "tos_daylight",
                                        "frame": 48,
                                        "path": "review.png",
                                        "width": 1920,
                                        "height": 1080,
                                    }
                                ],
                            }
                        ],
                    }
                ),
                encoding="utf-8",
            )
            (root / "manifest.csv").touch()
            (root / "config.json").write_text("{}\n", encoding="utf-8")
            binary = root / "player"
            binary.write_text("player\n", encoding="utf-8")
            binary.chmod(0o755)
            output = root / "output"
            quality_campaign_path = str(ROOT / "benchmarks" / "quality_sweeps")
            with mock.patch.object(sys, "path", [quality_campaign_path, *sys.path]):
                from benchmarks.quality_sweeps import run_quality_campaign

                with mock.patch.object(
                    run_quality_campaign.subprocess,
                    "run",
                    return_value=subprocess.CompletedProcess([], 0),
                ):
                    with mock.patch.object(
                        sys,
                        "argv",
                        [
                            "run_quality_campaign.py",
                            "--campaign",
                            str(campaign),
                            "--binary",
                            str(binary),
                            "--output-root",
                            str(output),
                        ],
                    ):
                        self.assertEqual(run_quality_campaign.main(), 2)

    def test_campaign_output_records_exact_current_git_commit_with_hashes(self) -> None:
        """The spatial campaign aggregate must carry commit, binary, and config identity."""
        with __import__("tempfile").TemporaryDirectory() as directory:
            root = Path(directory)
            binary_digest = hashlib.sha256(b"player\n").hexdigest()
            config_digest = hashlib.sha256(b"{}\n").hexdigest()
            campaign = root / "campaign.json"
            campaign.write_text(
                json.dumps(
                    {
                        "schemaVersion": 2,
                        "campaignId": "provenance-campaign",
                        "baselineCandidateId": "candidate",
                        "corpus": {"manifestPath": "manifest.csv", "selection": ["tos_daylight"]},
                        "classes": ["daylight"],
                        "requiredMetrics": [
                            "static_flicker",
                            "edge_variance",
                            "motion_compensated_error",
                            "ghost_duration_frames",
                            "reset_recovery_frames",
                        ],
                        "candidates": [
                            {
                                "id": "candidate",
                                "configPath": "config.json",
                                "binarySha256": binary_digest,
                                "configSha256": config_digest,
                                "dimensions": {"source": "426x240", "output": "1920x1080"},
                                "reviewAssets": [
                                    {
                                        "scene": "tos_daylight",
                                        "frame": 48,
                                        "path": "review.png",
                                        "width": 1920,
                                        "height": 1080,
                                    }
                                ],
                            }
                        ],
                    }
                ),
                encoding="utf-8",
            )
            (root / "manifest.csv").touch()
            (root / "config.json").write_text("{}\n", encoding="utf-8")
            (root / "review.png").write_bytes(b"review")
            binary = root / "player"
            binary.write_text("player\n", encoding="utf-8")
            binary.chmod(0o755)
            output = root / "output"
            expected_commit = subprocess.run(
                ["git", "rev-parse", "HEAD"],
                cwd=ROOT,
                capture_output=True,
                text=True,
                check=True,
            ).stdout.strip()
            quality_campaign_path = str(ROOT / "benchmarks" / "quality_sweeps")
            with mock.patch.object(sys, "path", [quality_campaign_path, *sys.path]):
                from benchmarks.quality_sweeps import run_quality_campaign

                def child_run(command, **_kwargs):
                    if command[:3] == ["git", "-C", str(ROOT)]:
                        if command[-2:] == ["status", "--porcelain"]:
                            return subprocess.CompletedProcess(command, 0, stdout="", stderr="")
                        return subprocess.CompletedProcess(command, 0, stdout=expected_commit, stderr="")
                    candidate_root = output / "candidate" / "captured-run"
                    candidate_root.mkdir(parents=True)
                    csv_path = candidate_root / "quality.csv"
                    csv_path.write_text("clip_id\nclip\n", encoding="utf-8")
                    still = candidate_root / "still.png"
                    still.write_bytes(b"still")
                    result = {
                        "candidateId": "candidate",
                        "exitCode": 0,
                        "binary": str(binary),
                        "binarySha256": binary_digest,
                        "configSource": str(root / "config.json"),
                        "configSha256": config_digest,
                        "csv": str(csv_path),
                        "metrics": {"rowCount": 1},
                        "representativeStillPaths": [str(still)],
                        "reviewAssets": [
                            {
                                "scene": "tos_daylight",
                                "frame": 48,
                                "path": str(still),
                                "width": 1920,
                                "height": 1080,
                            }
                        ],
                    }
                    (candidate_root / "results.json").write_text("[]\n", encoding="utf-8")
                    (candidate_root / "rankings.csv").write_text("candidateId\ncandidate\n", encoding="utf-8")
                    (candidate_root / "rankings.json").write_text("[]\n", encoding="utf-8")
                    (candidate_root / "candidate").mkdir()
                    (candidate_root / "candidate" / "result.json").write_text(
                        json.dumps(result), encoding="utf-8"
                    )
                    return subprocess.CompletedProcess(command, 0)

                with mock.patch.object(run_quality_campaign.subprocess, "run", side_effect=child_run):
                    with mock.patch.object(
                        run_quality_campaign,
                        "guarded_worker_count",
                        return_value=(2, False),
                    ):
                        with mock.patch.object(
                            sys,
                            "argv",
                            [
                                "run_quality_campaign.py",
                                "--campaign",
                                str(campaign),
                                "--binary",
                                str(binary),
                                "--output-root",
                                str(output),
                            ],
                        ):
                            self.assertEqual(run_quality_campaign.main(), 0)

            aggregate = json.loads((output / "campaign-results.json").read_text(encoding="utf-8"))
            self.assertEqual(len(aggregate), 1)
            self.assertEqual(aggregate[0]["gitCommit"], expected_commit)
            self.assertEqual(aggregate[0]["binarySha256"], binary_digest)
            self.assertEqual(aggregate[0]["configSha256"], config_digest)
            derived_campaign = json.loads((output / "campaign.json").read_text(encoding="utf-8"))
            self.assertEqual(derived_campaign["executionProvenance"]["captureWorkers"], 2)
            self.assertEqual(derived_campaign["executionProvenance"]["captureRetries"], 1)
            self.assertEqual(
                derived_campaign["candidates"][0]["reviewAssets"][0]["path"],
                aggregate[0]["reviewAssets"][0]["path"],
            )
            from benchmarks.quality_sweeps.campaign_provenance import validate_execution_provenance

            validate_execution_provenance(derived_campaign, aggregate, root)

    def test_unavailable_git_capture_is_explicit_and_not_current_head(self) -> None:
        """A failed metadata lookup must remain unavailable for strict verification."""
        from benchmarks.quality_sweeps.campaign_provenance import capture_git_commit

        with mock.patch(
            "benchmarks.quality_sweeps.campaign_provenance.subprocess.run",
            side_effect=OSError("git unavailable"),
        ):
            self.assertIsNone(capture_git_commit(ROOT))

    def test_dirty_worktree_is_not_authoritative_git_provenance(self) -> None:
        """A commit cannot identify a binary built from uncommitted source."""
        from benchmarks.quality_sweeps.campaign_provenance import capture_git_commit

        responses = [
            subprocess.CompletedProcess([], 0, stdout="a" * 40 + "\n", stderr=""),
            subprocess.CompletedProcess([], 0, stdout=" M src/file.cpp\n", stderr=""),
        ]
        with mock.patch(
            "benchmarks.quality_sweeps.campaign_provenance.subprocess.run",
            side_effect=responses,
        ):
            self.assertIsNone(capture_git_commit(ROOT))

    def test_spatial_replay_does_not_infer_git_from_current_head(self) -> None:
        """Old result identities without commit evidence remain explicitly unrecorded."""
        with __import__("tempfile").TemporaryDirectory() as directory:
            root = Path(directory)
            run_root = root / "run"
            candidate_root = run_root / "candidate" / "run-001" / "candidate" / "01"
            candidate_root.mkdir(parents=True)
            result = {
                "candidateId": "candidate",
                "runId": "run-001",
                "exitCode": 0,
                "binarySha256": "a" * 64,
                "configSha256": "b" * 64,
            }
            (candidate_root / "result.json").write_text(json.dumps(result), encoding="utf-8")
            output = root / "campaign-results.json"
            from tools.assemble_spatial_campaign_results import assemble_results

            assembled, commit = assemble_results(run_root, ["candidate"])
            self.assertIsNone(commit)
            self.assertIsNone(assembled[0]["gitCommit"])
            self.assertEqual(assembled[0]["gitCommitStatus"], "unrecorded")
            output.write_text(json.dumps(assembled), encoding="utf-8")

    def test_baseline_aware_sweep_writes_paired_rankings(self) -> None:
        """A completed baseline-aware sweep must publish strict paired evidence."""
        with __import__("tempfile").TemporaryDirectory() as directory:
            root = Path(directory)
            fake_binary = root / "fake-player"
            fake_binary.write_text("#!/usr/bin/env bash\nexit 0\n", encoding="utf-8")
            fake_binary.chmod(0o755)
            config = root / "quality.json"
            config.write_text("{}\n", encoding="utf-8")
            manifest = root / "manifest.json"
            manifest.write_text(
                json.dumps(
                    {
                        "baselineCandidateId": "base",
                        "dimensions": "426x240",
                        "outputDimensions": "1920x1080",
                        "frame": 48,
                        "quality": "high",
                        "experiments": [
                            {"id": "base", "config": "quality.json"},
                            {"id": "candidate", "config": "quality.json"},
                        ],
                    }
                ),
                encoding="utf-8",
            )
            fake_runner = root / "fake-quality-runner"
            fake_runner.write_text(
                "#!/usr/bin/env bash\n"
                "printf 'clip_id,width,height,output_width,output_height,quality,crf,frame,fsr_psnr_db,fsr_ssim,fsr_edge_ssim,fsr_lowfreq_luma_mae,fsr_lowfreq_luma_bias\\n' > \"$3\"\n"
                "printf 'clip-a,426,240,1920,1080,high,12,48,25.0,0.75,0.80,0.10,0.01\\n' >> \"$3\"\n"
                "printf 'still' > \"${TFORGE_QUALITY_ASSET_MANIFEST%.csv}.png\"\n"
                "printf 'scene,frame,path,width,height\\n' > \"$TFORGE_QUALITY_ASSET_MANIFEST\"\n"
                "printf 'clip-a,48,%s,1920,1080\\n' \"${TFORGE_QUALITY_ASSET_MANIFEST%.csv}.png\" >> \"$TFORGE_QUALITY_ASSET_MANIFEST\"\n",
                encoding="utf-8",
            )
            fake_runner.chmod(0o755)
            output_root = root / "output"
            argv = [
                "run_quality_sweep.py",
                "--manifest",
                str(manifest),
                "--binary",
                str(fake_binary),
                "--output-root",
                str(output_root),
                "--tag-prefix",
                "contract-paired-ranking",
            ]
            quality_sweeps_path = str(ROOT / "benchmarks" / "quality_sweeps")
            with mock.patch.object(sys, "path", [quality_sweeps_path, *sys.path]):
                from benchmarks.quality_sweeps import run_quality_sweep

                with mock.patch.object(run_quality_sweep, "QUALITY_RUNNER", fake_runner):
                    with mock.patch.object(sys, "argv", argv):
                        self.assertEqual(run_quality_sweep.main(), 0)

            run_root = next(output_root.iterdir())
            paired_rankings = run_root / "paired_rankings.csv"
            self.assertTrue(paired_rankings.is_file())
            self.assertIn("candidate", paired_rankings.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
