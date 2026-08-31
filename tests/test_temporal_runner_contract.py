"""Static contract tests for the optional honest temporal-metric runner path."""

from __future__ import annotations

import subprocess
import os
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / "benchmarks" / "video_corpus" / "run_temporal_quality.sh"
PLAYBACK = ROOT / "src" / "core" / "PlaybackEngine.cpp"


class TemporalRunnerContractTests(unittest.TestCase):
    """Require explicit sidecars before the capture runner emits new metrics."""

    def test_temporal_jitter_policy_uses_decoded_source_dimensions(self) -> None:
        """Jitter is added to source/model coordinates, not presentation pixels."""
        source = PLAYBACK.read_text(encoding="utf-8")
        call = source[source.index("sideBufferSynth_.setRenderSize("):]
        call = call[:call.index(");") + 2]
        self.assertIn("static_cast<uint32_t>(std::max(0, df.width))", call)
        self.assertIn("static_cast<uint32_t>(std::max(0, df.height))", call)
        self.assertNotIn("fsrTargetViewportW_", call)
        self.assertNotIn("fsrTargetViewportH_", call)

    def test_seek_discards_buffered_pending_frame_before_consumption(self) -> None:
        """A frame buffered before seek must never cross the flush boundary."""
        source = PLAYBACK.read_text(encoding="utf-8")
        pending = source[source.index("if (hasPendingDecodedFrame)"):]
        pending = pending[:pending.index("} else {") + 2]
        self.assertIn("seekPending_.load(", pending)
        self.assertIn("hasPendingDecodedFrame = false", pending)
        self.assertIn("pendingDecodedFrame = {}", pending)

    def test_seek_uses_generation_to_reset_analysis_after_demux_flush(self) -> None:
        """A seek must reset CPU analysis even when no flush packet is queued."""
        source = PLAYBACK.read_text(encoding="utf-8")
        self.assertIn("seekGeneration_", source)
        self.assertIn("handledSeekGeneration", source)
        self.assertIn("seekGeneration_.load", source)
        self.assertIn("sideBufferSynth_.resetAnalysisHistory()", source)

    def test_inflight_slots_are_explicitly_opt_in_until_completion_is_committed(self) -> None:
        """Async submission must not publish temporal state before fence completion."""
        source = PLAYBACK.read_text(encoding="utf-8")
        block = source[source.index("const bool asyncSlots") :]
        block = block[:block.index(";", block.index("const bool asyncSlots")) + 1]
        self.assertIn("TFORGE_FSR4_ENABLE_INFLIGHT", block)

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
            "temporal_rate",
            "setpts=N/${temporal_rate}/TB",
            "TFORGE_TEMPORAL_CANDIDATE_ID",
            "TFORGE_TEMPORAL_SCENE",
            "TFORGE_TEMPORAL_CONFIG_ID",
            "TFORGE_TEMPORAL_START_FRAME",
            "TFORGE_FSR4_PRE_EASU",
            "TFORGE_FSR4_EXPERIMENTAL_REFINE_MOTION",
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

    def test_temporal_runner_preserves_input_cadence(self) -> None:
        """24-fps source material must not be silently converted to 30 fps."""
        source = RUNNER.read_text(encoding="utf-8")
        self.assertIn("ffprobe", source)
        self.assertIn("avg_frame_rate", source)
        self.assertIn("r_frame_rate", source)
        self.assertIn('temporal_rate="', source)
        self.assertIn('"$temporal_rate"', source)
        self.assertNotIn("-r 30", source)
        self.assertNotIn("setpts=N/30/TB", source)

    def test_temporal_runner_retains_source_color_metadata(self) -> None:
        """Future color A/B captures must retain the exact decoded stream metadata."""
        source = RUNNER.read_text(encoding="utf-8")
        self.assertIn("source_stream_metadata.json", source)
        self.assertIn("color_range,color_space,color_transfer,color_primaries", source)
        self.assertIn('cp "$tmpdir/source_stream_metadata.json" "$artifact_dir/"', source)

    def test_legacy_ssim_compares_all_outputs_in_one_explicit_metric_domain(self) -> None:
        """SSIM must not mix encoded YUV range with RGB reference pixels."""
        source = RUNNER.read_text(encoding="utf-8")
        self.assertIn("metric_normalize_filter", source)
        self.assertIn("scale=in_range=auto:out_range=full", source)
        self.assertIn("format=gbrp", source)
        self.assertIn("[metric_fsr]", source)
        self.assertIn("[metric_lanczos]", source)
        self.assertIn("[metric_reference]", source)

    def test_temporal_runner_retains_reproduction_identity(self) -> None:
        """A retained capture must identify the binary, tree, inputs, and invocation."""
        source = RUNNER.read_text(encoding="utf-8")
        for token in (
            "binary_sha256.txt",
            "git_commit.txt",
            "worktree_diff_sha256.txt",
            "input_sha256.txt",
            "reference_sha256.txt",
            "capture_command.txt",
            "sha256sum",
            "git -C",
        ):
            self.assertIn(token, source)

    def test_temporal_runner_forwards_native_graph_disable_switch(self) -> None:
        """The generic-graph A/B must not silently execute native INT8."""
        source = RUNNER.read_text(encoding="utf-8")
        self.assertIn("TFORGE_FSR4_DISABLE_NATIVE_INT8 \\", source)

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
        self.assertIn('cp "$benchmark_quality_lab_config" "$artifact_dir/quality_lab.json"', source)
        self.assertIn("quality_lab_source.txt", source)
        # Hardware-decode provenance is added once before the allowlist loop;
        # duplicate entries make saved environments ambiguous.
        self.assertNotIn("TFORGE_DISABLE_HW_DECODE; do", source)
        self.assertIn("interactive checkout-level Quality Lab selection", source)
        # A missing lab file changes the player's typed defaults and can make
        # headless capture diverge from the interactive campaign. The runner
        # must materialize the repository's validated config in its isolated
        # config home unless the caller explicitly supplies another one.
        self.assertIn("config/quality_lab.json", source)
        self.assertIn("cp \"$runner_dir/../../config/quality_lab.json\"", source)
        self.assertIn("benchmark_quality_lab_config=\"$benchmark_config_home", source)
        self.assertIn('TFORGE_BENCHMARK_FFMPEG_THREADS="${TFORGE_BENCHMARK_FFMPEG_THREADS:-1}"', matrix)

    def test_temporal_runner_rejects_spatial_only_quality_lab_config(self) -> None:
        """Temporal captures must not silently publish a base-only spatial A/B."""
        source = RUNNER.read_text(encoding="utf-8")
        self.assertIn("validate_temporal_quality_config", source)
        self.assertIn('mode == "base_only"', source)
        self.assertIn("temporal capture requires a temporal composition", source)
        self.assertIn("TFORGE_ALLOW_SPATIAL_TEMPORAL_CONTROL=1", source)

    def test_temporal_capture_clears_electron_node_mode(self) -> None:
        """Qt must not inherit the host Electron-as-Node mode into the player."""
        source = RUNNER.read_text(encoding="utf-8")
        self.assertIn("unset ELECTRON_RUN_AS_NODE", source)

    def test_temporal_capture_isolates_best_findings_switch(self) -> None:
        """A raw arm must not inherit the best-findings switch from the shell."""
        source = RUNNER.read_text(encoding="utf-8")
        self.assertIn("TFORGE_FSR4_DISABLE_BEST_FINDINGS", source)
        self.assertIn("unset TFORGE_FSR4_DISABLE_BEST_FINDINGS", source)
        self.assertIn('player_environment+=("TFORGE_FSR4_DISABLE_BEST_FINDINGS=', source)

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
            ffprobe = fake_bin / "ffprobe"
            ffprobe.write_text(
                "#!/usr/bin/env bash\nprintf '1/1\\n'\n", encoding="utf-8"
            )
            ffprobe.chmod(0o755)

            input_path = root / "input.mkv"
            reference_path = root / "reference.mkv"
            input_path.write_bytes(b"input")
            reference_path.write_bytes(b"reference")
            output_path = root / "metrics.csv"
            failure_dir = root / "failure-artifacts"
            temporal_config = root / "temporal-quality.json"
            temporal_config.write_text(
                '{"qualityLab":{"composition":{"mode":"current"}}}\n',
                encoding="utf-8",
            )
            environment = os.environ.copy()
            environment.update(
                {
                    "PATH": f"{fake_bin}:{environment['PATH']}",
                    "TFORGE_TEMPORAL_FAILURE_ARTIFACT_DIR": str(failure_dir),
                    "TFORGE_QUALITY_LAB_CONFIG": str(temporal_config),
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
            "TFORGE_FSR4_LEARNED_CONFIDENCE_FLOOR",
            "TFORGE_FSR4_ENABLE_EXPERIMENTAL_CONFIDENCE_GATE",
            "TFORGE_FSR4_EXPERIMENTAL_EMPTY_MOTION_CONFIDENCE",
            "TFORGE_FSR4_EXPERIMENTAL_DISABLE_POSTPASS_TAIL",
            "TFORGE_FSR4_EXPERIMENTAL_RECOVERED_LINEAR_OUTPUT",
            "TFORGE_FSR4_EXPERIMENTAL_MOTION_SIGN",
            "TFORGE_FSR4_EXPERIMENTAL_MOTION_SCALE",
            "TFORGE_FSR4_EXPERIMENTAL_MOTION_ROUNDING",
            "TFORGE_FSR4_MOTION_ESTIMATOR",
            "TFORGE_FSR4_EXPERIMENTAL_HISTORY_INTERPOLATION",
            "TFORGE_FSR4_EXPERIMENTAL_HISTORY_JITTER_DELTA",
            "TFORGE_FSR4_TRUE_FSR1_EASU",
            "TFORGE_FSR4_EXPERIMENTAL_RECURRENT_RESET_ONLY",
            "TFORGE_FSR4_EXPERIMENTAL_CONFIDENCE_ORDERED_MOTION",
            "TFORGE_FSR4_EXPERIMENTAL_MOTION_ANALYSIS_WIDTH",
        ):
            self.assertIn(name, source)

    def test_temporal_runner_records_forwarded_environment_for_retained_artifacts(self) -> None:
        """Retained captures must prove which temporal flags actually reached the player."""
        source = RUNNER.read_text(encoding="utf-8")
        self.assertIn("player_environment.txt", source)
        self.assertIn('printf \'%s\\n\' "${player_environment[@]}"', source)

    def test_explicit_motion_off_overrides_quality_lab_motion_mode(self) -> None:
        """Motion-off A/B captures must not inherit a configured codec estimator."""
        source = (ROOT / "src/core/PlaybackEngine.cpp").read_text(encoding="utf-8")
        self.assertIn("effectiveMotionConfig.mode = environmentMotionConfig.mode;", source)

    def test_explicit_motion_edge_aware_overrides_quality_lab_motion_mode(self) -> None:
        """The dense boundary policy must be selectable in typed review captures."""
        source = (ROOT / "src/core/PlaybackEngine.cpp").read_text(encoding="utf-8")
        self.assertIn("TFORGE_FSR4_MOTION_EDGE_AWARE", source)
        self.assertIn("effectiveMotionConfig.edgeAwareUpscale =", source)
        self.assertIn("environmentMotionConfig.edgeAwareUpscale", source)

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

    def test_temporal_runner_forwards_b_frame_reference_controls(self) -> None:
        """B-picture A/B captures must reach the player instead of being silently identical."""
        source = RUNNER.read_text(encoding="utf-8")
        self.assertIn("TFORGE_FSR4_MOTION_REJECT_B_FRAMES", source)
        self.assertIn("TFORGE_FSR4_MOTION_ALLOW_B_FRAMES", source)

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
            "TFORGE_FSR4_EXPERIMENTAL_FULL_JITTER",
            "TFORGE_FSR4_MOTION_FALLBACK_AFTER_FILTERING",
            "TFORGE_FSR4_MOTION_DENSE_GRID",
            "TFORGE_FSR4_FP8_ROUNDING",
            "TFORGE_FSR4_PROFILE_TIMINGS",
            "TFORGE_FSR4_LOG_INTERVAL",
            "TFORGE_FSR4_TRACE_STAGE_CONFIG",
            "TFORGE_FSR4_TRACE_FINAL_PIPELINE",
            "TFORGE_FSR4_EXPERIMENTAL_FIXED_HISTORY_WEIGHT",
            "TFORGE_FSR4_HISTORY_RECTIFICATION_SCALE",
            "TFORGE_FSR4_EXPERIMENTAL_HISTORY_JITTER_DELTA",
            "TFORGE_FSR4_MOTION_MAX_REFINED_SEEDS",
            "TFORGE_FSR4_DISABLE_FUSED_PRESENTATION",
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

    def test_temporal_runner_forwards_future_evidence_only_probe(self) -> None:
        """Future-frame evidence must be available without displaying it."""
        source = RUNNER.read_text(encoding="utf-8")
        playback = (ROOT / "src/core/PlaybackEngine.cpp").read_text(
            encoding="utf-8"
        )
        self.assertIn("TFORGE_FSR4_EXPERIMENTAL_FUTURE_EVIDENCE_ONLY", source)
        self.assertIn("TFORGE_FSR4_EXPERIMENTAL_FUTURE_EVIDENCE_ONLY", playback)

    def test_temporal_runner_forwards_jitter_sign_probe(self) -> None:
        """Jitter-sign A/B captures must reach the player unchanged."""
        source = RUNNER.read_text(encoding="utf-8")
        self.assertIn("TFORGE_FSR4_JITTER_SIGN", source)

    def test_temporal_runner_forwards_integrated_temporal_profile(self) -> None:
        """The matched motion/color profile must be selectable as one arm."""
        source = RUNNER.read_text(encoding="utf-8")
        self.assertIn("TFORGE_FSR4_INTEGRATED_TEMPORAL", source)
        self.assertIn("TFORGE_FSR4_INTEGRATED_BEST_FINDINGS", source)

    def test_temporal_runner_forwards_integrated_history_confidence_profile(self) -> None:
        """The measured history/confidence combination must remain selectable."""
        source = RUNNER.read_text(encoding="utf-8")
        playback = (ROOT / "src/core/PlaybackEngine.cpp").read_text(
            encoding="utf-8"
        )
        harness = (ROOT / "src/render/Fsr4DispatchHarness.cpp").read_text(
            encoding="utf-8"
        )
        for text in (source, playback, harness):
            self.assertIn("TFORGE_FSR4_INTEGRATED_HISTORY_CONFIDENCE", text)
        self.assertIn("TFORGE_FSR4_LEARNED_CONFIDENCE_BLEND", source)

    def test_temporal_runner_forwards_motion_validity_ab(self) -> None:
        """The legacy motion path is available only for matched A/B evidence."""
        source = RUNNER.read_text(encoding="utf-8")
        harness = (ROOT / "src/render/Fsr4DispatchHarness.cpp").read_text(
            encoding="utf-8"
        )
        self.assertIn("TFORGE_FSR4_DISABLE_MOTION_VALIDITY", source)
        self.assertIn("TFORGE_FSR4_DISABLE_MOTION_VALIDITY", harness)

    def test_temporal_runner_forwards_motion_ablation(self) -> None:
        """Zero and refined captures must be real, independently attributable A/B arms."""
        source = RUNNER.read_text(encoding="utf-8")
        playback = (ROOT / "src/core/PlaybackEngine.cpp").read_text(
            encoding="utf-8"
        )
        self.assertIn("TFORGE_FSR4_MOTION_ABLATION", source)
        self.assertIn("TFORGE_FSR4_MOTION_ABLATION", playback)

    def test_temporal_runner_forwards_dense_motion_sidecar(self) -> None:
        """Dense replay captures must use the player's canonical variable."""
        source = RUNNER.read_text(encoding="utf-8")
        playback = (ROOT / "src/core/PlaybackEngine.cpp").read_text(
            encoding="utf-8"
        )
        self.assertIn("TFORGE_FSR4_EXPERIMENTAL_DENSE_MOTION", source)
        self.assertIn("TFORGE_FSR4_EXPERIMENTAL_DENSE_MOTION", playback)
        self.assertIn("DENSE_MOTION_REPLAY", source)
        self.assertIn("canonical dense-motion variable", source)

    def test_dense_replay_missing_required_frame_fails_before_player_runs(self) -> None:
        """A partial replay sidecar must never produce a successful-looking capture."""

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            player = root / "fake-player"
            player.write_text(
                "#!/usr/bin/env bash\n"
                "printf 'PLAYER_RAN\n' >&2\n"
                "exit 99\n",
                encoding="utf-8",
            )
            player.chmod(0o755)
            input_path = root / "input.mkv"
            reference_path = root / "reference.mkv"
            input_path.write_bytes(b"input")
            reference_path.write_bytes(b"reference")
            replay_path = root / "dense-replay.json"
            replay_path.write_text(
                (
                    '{"schema":"temporal_forge.codec_motion.v1",'
                    '"coordinateDomain":"current_destination_to_previous_reference",'
                    '"motionUnits":"source_pixels",'
                    '"sampleConvention":"destination_plus_motion",'
                    '"frameIndexBase":"capture_relative",'
                    '"sourceWidth":8,"sourceHeight":8,'
                    '"frames":[{"frameIndex":0,"vectors":[]}]}'
                ),
                encoding="utf-8",
            )

            environment = os.environ.copy()
            environment["TFORGE_FSR4_EXPERIMENTAL_DENSE_MOTION"] = str(replay_path)
            result = subprocess.run(
                [
                    str(RUNNER),
                    str(player),
                    str(input_path),
                    str(reference_path),
                    str(root / "metrics.csv"),
                    "2",
                ],
                cwd=ROOT,
                env=environment,
                capture_output=True,
                text=True,
                check=False,
            )

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("required relative frames", result.stderr)
            self.assertNotIn("PLAYER_RAN", result.stderr)

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
