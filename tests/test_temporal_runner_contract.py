"""Static contract tests for the optional honest temporal-metric runner path."""

from __future__ import annotations

import subprocess
import os
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / "benchmarks" / "video_corpus" / "run_temporal_quality.sh"


class TemporalRunnerContractTests(unittest.TestCase):
    """Require explicit sidecars before the capture runner emits new metrics."""

    def test_runner_wires_the_standalone_metric_tool_without_renaming_legacy_fields(self) -> None:
        source = RUNNER.read_text(encoding="utf-8")

        for token in (
            "TFORGE_TEMPORAL_MOTION_JSON",
            "TFORGE_TEMPORAL_METRICS_CSV",
            "TFORGE_TEMPORAL_CLASS",
            "TFORGE_TEMPORAL_EVENTS_JSON",
            "TFORGE_TEMPORAL_STATIC_MASK_JSON",
            "TFORGE_TEMPORAL_WARMUP_FRAMES",
            "TFORGE_FSR4_DUMP_SEQUENCE_WARMUP",
            "TFORGE_FSR4_DUMP_SEQUENCE=$frames",
            "benchmark_config_home",
            "benchmark_settings.json",
            "XDG_CONFIG_HOME=$benchmark_config_home",
            "temporal_reference_filter",
            "setpts=N/30/TB",
            "TFORGE_TEMPORAL_CANDIDATE_ID",
            "TFORGE_TEMPORAL_SCENE",
            "TFORGE_TEMPORAL_CONFIG_ID",
            "TFORGE_TEMPORAL_START_FRAME",
            "tools/measure_temporal_sequence.py",
            "--candidate-dir",
            "--reference-dir",
            "--motion-json",
            "--static-mask-json",
            "--candidate-id",
            "--scene",
            "--config-id",
            "--start-frame",
            "-start_number 0",
        ):
            self.assertIn(token, source)

        # The historical tblend/frame-delta column remains distinct from the
        # new metric output; the bridge must not silently rewrite its header.
        self.assertIn("fsr_temporal_delta_mean", source)
        self.assertIn("temporal_metrics_output", source)

    def test_warmup_is_not_added_to_the_post_warmup_dump_count(self) -> None:
        """The player applies warmup before numbering the requested outputs."""
        source = RUNNER.read_text(encoding="utf-8")
        self.assertIn(
            '"TFORGE_FSR4_DUMP_SEQUENCE=$frames"',
            source,
        )
        self.assertNotIn(
            '"TFORGE_FSR4_DUMP_SEQUENCE=$capture_frames"',
            source,
        )

    def test_runner_script_is_valid_bash(self) -> None:
        result = subprocess.run(
            ["bash", "-n", str(RUNNER)],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_campaign_can_limit_ffmpeg_threads_without_changing_capture_defaults(self) -> None:
        """Campaign fan-out has an explicit CPU scheduling control."""
        source = RUNNER.read_text(encoding="utf-8")
        matrix = (ROOT / "benchmarks/video_corpus/run_temporal_quality_matrix.sh").read_text(
            encoding="utf-8"
        )
        self.assertIn("TFORGE_BENCHMARK_FFMPEG_THREADS", source)
        self.assertIn("run_ffmpeg()", source)
        self.assertIn('ffmpeg -threads "$ffmpeg_threads"', source)
        self.assertIn("TFORGE_TEMPORAL_CAPTURE_TIMEOUT", source)
        self.assertIn('timeout "${capture_timeout}s"', source)
        self.assertIn("benchmark_quality_lab_config", source)
        self.assertIn("interactive checkout-level Quality Lab selection", source)
        self.assertIn('TFORGE_BENCHMARK_FFMPEG_THREADS="${TFORGE_BENCHMARK_FFMPEG_THREADS:-1}"', matrix)

    def test_temporal_capture_clears_electron_node_mode(self) -> None:
        """Qt must not inherit the host Electron-as-Node mode into the player."""
        source = RUNNER.read_text(encoding="utf-8")
        self.assertIn("unset ELECTRON_RUN_AS_NODE", source)

    def test_opt_in_failure_artifact_dir_preserves_partial_capture_and_log(self) -> None:
        """A failed capture keeps diagnostic inputs without becoming successful."""

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fake_bin = root / "bin"
            fake_bin.mkdir()
            player = fake_bin / "fake-player"
            player.write_text(
                "#!/usr/bin/env bash\n"
                "set -eu\n"
                "printf 'partial player output\\n'\n"
                "mkdir -p \"$TFORGE_FSR4_DUMP_SEQUENCE_DIR\"\n"
                "printf 'partial frame\\n' > \"$TFORGE_FSR4_DUMP_SEQUENCE_DIR/temporal_forge_fsr4_0000.ppm\"\n"
                "exit 7\n",
                encoding="utf-8",
            )
            player.chmod(0o755)
            for name in ("ffmpeg", "identify"):
                tool = fake_bin / name
                tool.write_text("#!/usr/bin/env bash\nexit 0\n", encoding="utf-8")
                tool.chmod(0o755)

            input_path = root / "input.mkv"
            reference_path = root / "reference.mkv"
            input_path.write_bytes(b"input")
            reference_path.write_bytes(b"reference")
            output_path = root / "metrics.csv"
            failure_dir = root / "failure-artifacts"
            environment = os.environ.copy()
            environment.update(
                {
                    "PATH": f"{fake_bin}:{environment['PATH']}",
                    "TFORGE_TEMPORAL_FAILURE_ARTIFACT_DIR": str(failure_dir),
                }
            )

            result = subprocess.run(
                [
                    str(RUNNER),
                    str(player),
                    str(input_path),
                    str(reference_path),
                    str(output_path),
                    "2",
                ],
                cwd=ROOT,
                env=environment,
                capture_output=True,
                text=True,
                check=False,
            )

            self.assertNotEqual(result.returncode, 0)
            self.assertTrue(
                (failure_dir / "player.log").is_file(),
                result.stderr,
            )
            self.assertTrue(
                (failure_dir / "fsr" / "temporal_forge_fsr4_0000.ppm").is_file(),
                result.stderr,
            )
            self.assertIn("partial player output", (failure_dir / "player.log").read_text())

    def test_failure_retention_is_opt_in_and_success_still_cleans_temp_directory(self) -> None:
        """Failure copying is gated; the normal exit trap still removes tmpdir."""

        source = RUNNER.read_text(encoding="utf-8")
        self.assertIn("TFORGE_TEMPORAL_FAILURE_ARTIFACT_DIR", source)
        self.assertIn('cp "$tmpdir/player.log" "$artifact_dir/"', source)
        self.assertIn("if (( status != 0 ))", source)
        self.assertIn("cp -a \"$tmpdir\"/.", source)
        self.assertIn("rm -rf \"$tmpdir\"", source)

    def test_enhanced_artifact_copy_is_idempotent_for_paths_inside_artifact_dir(self) -> None:
        """A caller-selected sidecar may already be the artifact destination."""

        source = RUNNER.read_text(encoding="utf-8")
        self.assertIn("copy_artifact_file()", source)
        self.assertIn(
            'if [[ "$(realpath "$source")" == "$(realpath -m "$destination")" ]]; then',
            source,
        )
        for variable in (
            '"$temporal_motion_json"',
            '"$temporal_events_json"',
            '"$temporal_static_mask_json"',
        ):
            self.assertIn(
                f"copy_artifact_file {variable} \"$artifact_dir\"",
                source,
            )
        # The direct copies are the regression: cp rejects source == destination
        # after all valid enhanced metrics have already been written.
        self.assertNotIn(
            'cp "$temporal_motion_json" "$artifact_dir/"',
            source,
        )
        self.assertNotIn(
            'cp "$temporal_static_mask_json" "$artifact_dir/"',
            source,
        )

    def test_requested_sidecars_cannot_be_reused_from_a_previous_capture(self) -> None:
        """Every enhanced capture must start with a fresh sidecar namespace."""

        source = RUNNER.read_text(encoding="utf-8")
        self.assertIn("requested sidecar already exists", source)
        self.assertIn("temporal_motion_json", source)
        self.assertIn("temporal_events_json", source)
        self.assertIn("temporal_metrics_output", source)
        self.assertIn("temporal_static_mask_json", source)
        self.assertIn("TFORGE_FSR4_DUMP_MOTION_SIDECAR=1", source)
        self.assertIn("TFORGE_FSR4_EXPERIMENTAL_REPLACE_MOTION", source)

    def test_requested_sidecar_path_entry_cannot_be_a_dangling_symlink(self) -> None:
        """A preexisting symlink is not a fresh output namespace."""

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            player = root / "player"
            player.write_text("#!/usr/bin/env bash\nexit 0\n", encoding="utf-8")
            player.chmod(0o755)
            input_path = root / "input.mkv"
            reference_path = root / "reference.mkv"
            input_path.write_bytes(b"input")
            reference_path.write_bytes(b"reference")

            for variable in (
                "TFORGE_TEMPORAL_MOTION_JSON",
                "TFORGE_TEMPORAL_METRICS_CSV",
                "TFORGE_TEMPORAL_EVENTS_JSON",
            ):
                with self.subTest(variable=variable):
                    sidecar = root / f"{variable}.out"
                    sidecar.symlink_to(root / "missing-sidecar-target")
                    environment = os.environ.copy()
                    environment[variable] = str(sidecar)

                    result = subprocess.run(
                        [
                            str(RUNNER),
                            str(player),
                            str(input_path),
                            str(reference_path),
                            str(root / "results.csv"),
                            "2",
                        ],
                        cwd=ROOT,
                        env=environment,
                        capture_output=True,
                        text=True,
                        check=False,
                    )

                    self.assertEqual(result.returncode, 2, result.stderr)
                    self.assertIn("requested sidecar already exists", result.stderr)

    def test_temporal_runner_forwards_declared_quality_environment(self) -> None:
        """Temporal rows must use the same explicit candidate settings as spatial rows."""
        source = RUNNER.read_text(encoding="utf-8")
        for name in (
            "TFORGE_FSR4_ENABLE_COLOR_HISTORY",
            "TFORGE_FSR4_DISABLE_PREPASS",
            "TFORGE_FSR4_DISPATCH_TRACE",
            "TFORGE_FSR4_ENABLE_RECURRENT",
            "TFORGE_FSR4_CAS_STRENGTH",
            "TFORGE_FSR4_LEARNED_STRENGTH",
            "TFORGE_FSR4_FORCE_RESET",
            "TFORGE_FSR4_LEARNED_CONFIDENCE_BLEND",
            "TFORGE_FSR4_ENABLE_EXPERIMENTAL_CONFIDENCE_GATE",
            "TFORGE_FSR4_EXPERIMENTAL_EMPTY_MOTION_CONFIDENCE",
            "TFORGE_FSR4_EXPERIMENTAL_DISABLE_POSTPASS_TAIL",
            "TFORGE_FSR4_EXPERIMENTAL_RECOVERED_LINEAR_OUTPUT",
            "TFORGE_FSR4_EXPERIMENTAL_MOTION_SIGN",
            "TFORGE_FSR4_EXPERIMENTAL_MOTION_SCALE",
            "TFORGE_FSR4_EXPERIMENTAL_MOTION_ROUNDING",
            "TFORGE_FSR4_EXPERIMENTAL_HISTORY_INTERPOLATION",
            "TFORGE_FSR4_EXPERIMENTAL_RECURRENT_RESET_ONLY",
        ):
            self.assertIn(name, source)

    def test_spatial_runner_forwards_motion_history_candidates(self) -> None:
        """Spatial and temporal captures must expose the same opt-in probes."""
        source = (ROOT / "benchmarks/video_corpus/run_quality.sh").read_text(
            encoding="utf-8"
        )
        for name in (
            "TFORGE_FSR4_EXPERIMENTAL_MOTION_SIGN",
            "TFORGE_FSR4_EXPERIMENTAL_MOTION_SCALE",
            "TFORGE_FSR4_EXPERIMENTAL_MOTION_ROUNDING",
            "TFORGE_FSR4_EXPERIMENTAL_HISTORY_INTERPOLATION",
            "TFORGE_FSR4_EXPERIMENTAL_RECURRENT_RESET_ONLY",
        ):
            self.assertIn(name, source)

    def test_temporal_runner_forwards_postpass_tail_experiment(self) -> None:
        """The opt-in tail A/B must reach the host without changing defaults."""
        source = RUNNER.read_text(encoding="utf-8")
        harness = (ROOT / "src/render/Fsr4DispatchHarness.cpp").read_text(
            encoding="utf-8"
        )
        self.assertIn("TFORGE_FSR4_EXPERIMENTAL_DISABLE_POSTPASS_TAIL", source)
        self.assertIn("TFORGE_FSR4_EXPERIMENTAL_DISABLE_POSTPASS_TAIL", harness)

    def test_temporal_runner_forwards_recovered_linear_output_experiment(self) -> None:
        """The final-store transfer A/B must reach the postpass host path."""
        source = RUNNER.read_text(encoding="utf-8")
        harness = (ROOT / "src/render/Fsr4DispatchHarness.cpp").read_text(
            encoding="utf-8"
        )
        shader = (ROOT / "shaders/fsr4/postpass_composite.comp").read_text(
            encoding="utf-8"
        )
        self.assertIn("TFORGE_FSR4_EXPERIMENTAL_RECOVERED_LINEAR_OUTPUT", source)
        self.assertIn("TFORGE_FSR4_EXPERIMENTAL_RECOVERED_LINEAR_OUTPUT", harness)
        self.assertIn("(slot0.z & 256u) != 0u", shader)
        self.assertIn("const bool hdrOutput = slot2.w != 0u", shader)
        self.assertIn("srgbToLinear(finalColor)", shader)

    def test_temporal_runner_forwards_rec709_input_eotf_experiment(self) -> None:
        """The Rec.709 input-domain A/B must reach the upload host path."""
        source = RUNNER.read_text(encoding="utf-8")
        host = (ROOT / "src/render/upload/YuvConstants.cpp").read_text(
            encoding="utf-8"
        )
        self.assertIn("TFORGE_FSR4_EXPERIMENTAL_REC709_INPUT_EOTF", source)
        self.assertIn("TFORGE_FSR4_EXPERIMENTAL_REC709_INPUT_EOTF", host)

    def test_temporal_runner_forwards_uploader_color_jitter_and_fp8_controls(self) -> None:
        """The new probes must be explicitly allowlisted by the capture runner."""
        source = RUNNER.read_text(encoding="utf-8")
        uploader = (ROOT / "src/render/GpuImageUploader.cpp").read_text(
            encoding="utf-8"
        )
        constants = (ROOT / "src/render/upload/YuvConstants.cpp").read_text(
            encoding="utf-8"
        )
        sidebuffer = (ROOT / "src/render/SideBufferSynth.cpp").read_text(
            encoding="utf-8"
        )
        harness = (ROOT / "src/render/Fsr4DispatchHarness.cpp").read_text(
            encoding="utf-8"
        )
        for name in (
            "TFORGE_FSR4_INPUT_SHARPEN_STRENGTH",
            "TFORGE_FSR4_INPUT_TRANSFER",
            "TFORGE_FSR4_CHROMA_FILTER",
            "TFORGE_FSR4_CHROMA_PHASE",
            "TFORGE_FSR4_JITTER_SEQUENCE",
            "TFORGE_FSR4_JITTER_CADENCE",
            "TFORGE_FSR4_FP8_ROUNDING",
            "TFORGE_FSR4_PROFILE_TIMINGS",
            "TFORGE_FSR4_LOG_INTERVAL",
            "TFORGE_FSR4_TRACE_STAGE_CONFIG",
            "TFORGE_FSR4_TRACE_FINAL_PIPELINE",
            "TFORGE_FSR4_EXPERIMENTAL_FIXED_HISTORY_WEIGHT",
        ):
            self.assertIn(name, source)
        self.assertIn("TFORGE_FSR4_INPUT_SHARPEN_STRENGTH", uploader)
        self.assertIn("TFORGE_FSR4_INPUT_TRANSFER", constants)
        self.assertIn("TFORGE_FSR4_CHROMA_FILTER", constants)
        self.assertIn("TFORGE_FSR4_CHROMA_PHASE", constants)
        self.assertIn("TFORGE_FSR4_JITTER_SEQUENCE", sidebuffer + source)
        self.assertIn("TFORGE_FSR4_JITTER_CADENCE", source)
        self.assertIn("TFORGE_FSR4_FP8_ROUNDING", harness)

    def test_temporal_runner_forwards_unknown_matrix_bt709_experiment(self) -> None:
        """The unknown-matrix BT.709 A/B must reach the YUV host path."""
        source = RUNNER.read_text(encoding="utf-8")
        host = (ROOT / "src/render/upload/YuvConstants.cpp").read_text(
            encoding="utf-8"
        )
        self.assertIn("TFORGE_FSR4_EXPERIMENTAL_UNKNOWN_MATRIX_BT709", source)
        self.assertIn("TFORGE_FSR4_EXPERIMENTAL_UNKNOWN_MATRIX_BT709", host)

    def test_temporal_runner_forwards_legacy_rcas_experiment(self) -> None:
        """The legacy current-path sharpen amount must be capture-controlled."""
        source = RUNNER.read_text(encoding="utf-8")
        harness = (ROOT / "src/render/Fsr4DispatchHarness.cpp").read_text(
            encoding="utf-8"
        )
        shader = (ROOT / "shaders/fsr4/postpass_composite.comp").read_text(
            encoding="utf-8"
        )
        self.assertIn("TFORGE_FSR4_LEGACY_RCAS_STRENGTH", source)
        self.assertIn("TFORGE_FSR4_LEGACY_RCAS_STRENGTH", harness)
        self.assertIn("slot4.w", shader)

    def test_temporal_runner_forwards_display_base_experiment(self) -> None:
        """The current-path base-space experiment must be explicit and auditable."""
        source = RUNNER.read_text(encoding="utf-8")
        harness = (ROOT / "src/render/Fsr4DispatchHarness.cpp").read_text(
            encoding="utf-8"
        )
        shader = (ROOT / "shaders/fsr4/postpass_composite.comp").read_text(
            encoding="utf-8"
        )
        self.assertIn("TFORGE_FSR4_USE_DISPLAY_BASE", source)
        self.assertIn("TFORGE_FSR4_DISPLAY_BASE_STRENGTH", source)
        self.assertIn("TFORGE_FSR4_USE_DISPLAY_BASE", harness)
        self.assertIn("TFORGE_FSR4_DISPLAY_BASE_STRENGTH", harness)
        self.assertIn("sampleDisplaySourceBicubic", shader)

    def test_temporal_runner_forwards_quality_lab_display_base_probe(self) -> None:
        """The model-space versus display-space base choice is explicit."""
        source = RUNNER.read_text(encoding="utf-8")
        harness = (ROOT / "src/render/Fsr4DispatchHarness.cpp").read_text(
            encoding="utf-8"
        )
        shader = (ROOT / "shaders/fsr4/postpass_composite.comp").read_text(
            encoding="utf-8"
        )
        self.assertIn("TFORGE_FSR4_QUALITY_LAB_DISPLAY_BASE", source)
        self.assertIn("TFORGE_FSR4_QUALITY_LAB_DISPLAY_BASE", harness)
        self.assertIn("TFORGE_POSTPASS_DISPLAY_SPACE_BASE = 512u", shader)
        self.assertIn("sampleDisplaySourceBicubic(baseSourcePos", shader)

    def test_temporal_runner_forwards_current_base_filter_experiment(self) -> None:
        """The current-path base kernel must be independently benchmarkable."""
        source = RUNNER.read_text(encoding="utf-8")
        harness = (ROOT / "src/render/Fsr4DispatchHarness.cpp").read_text(
            encoding="utf-8"
        )
        shader = (ROOT / "shaders/fsr4/postpass_composite.comp").read_text(
            encoding="utf-8"
        )
        self.assertIn("TFORGE_FSR4_CURRENT_BASE_FILTER", source)
        self.assertIn("TFORGE_FSR4_CURRENT_BASE_FILTER", harness)
        self.assertIn("sampleSourceBilinear(currentBaseSourcePos", shader)

    def test_temporal_runner_forwards_learned_confidence_experiment(self) -> None:
        """Learned-strength confidence gating must be independently testable."""
        source = RUNNER.read_text(encoding="utf-8")
        harness = (ROOT / "src/render/Fsr4DispatchHarness.cpp").read_text(
            encoding="utf-8"
        )
        self.assertIn("TFORGE_FSR4_DISABLE_LEARNED_CONFIDENCE_GATE", source)
        self.assertIn("TFORGE_FSR4_DISABLE_LEARNED_CONFIDENCE_GATE", harness)

    def test_temporal_runner_forwards_continuous_confidence_blend(self) -> None:
        """The opt-in confidence interpolation must reach the host path."""
        source = RUNNER.read_text(encoding="utf-8")
        harness = (ROOT / "src/render/Fsr4DispatchHarness.cpp").read_text(
            encoding="utf-8"
        )
        self.assertIn("TFORGE_FSR4_LEARNED_CONFIDENCE_BLEND", source)
        self.assertIn("TFORGE_FSR4_LEARNED_CONFIDENCE_BLEND", harness)

    def test_temporal_runner_forwards_motion_validity_ab(self) -> None:
        """The legacy motion path is available only for matched A/B evidence."""
        source = RUNNER.read_text(encoding="utf-8")
        harness = (ROOT / "src/render/Fsr4DispatchHarness.cpp").read_text(
            encoding="utf-8"
        )
        self.assertIn("TFORGE_FSR4_DISABLE_MOTION_VALIDITY", source)
        self.assertIn("TFORGE_FSR4_DISABLE_MOTION_VALIDITY", harness)

    def test_temporal_runner_forwards_current_linear_blend_experiment(self) -> None:
        """The current blend-space choice must be benchmark-controlled."""
        source = RUNNER.read_text(encoding="utf-8")
        harness = (ROOT / "src/render/Fsr4DispatchHarness.cpp").read_text(
            encoding="utf-8"
        )
        shader = (ROOT / "shaders/fsr4/postpass_composite.comp").read_text(
            encoding="utf-8"
        )
        self.assertIn("TFORGE_FSR4_CURRENT_BLEND_LINEAR", source)
        self.assertIn("TFORGE_FSR4_CURRENT_BLEND_LINEAR", harness)
        self.assertIn("linearToSrgb(mix", shader)


if __name__ == "__main__":
    unittest.main()
