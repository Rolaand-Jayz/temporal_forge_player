# Dirty worktree scope-boundary audit

Audit date: 2026-08-23. Repository: `/home/rolaandjayz/ZCodeProject/temporal_forge_player`.

The pre-audit boundary was branch `quality-lab-vibecoder` at `7fdf43c`, with
22 tracked modified paths and 68 untracked paths from `git status --short -uall`.
Those paths were already present when this audit began; author/timing evidence
does not identify which earlier task created them. This document is the only
path added by the audit. No reset, clean, stash, delete, or runtime/shader/model
file edit was performed.

Classification is by the file's inspected role, not just its directory. In
particular, `src/render/Fsr4DispatchHarness.*` is category (d); it is the GPU
runtime dispatch harness, not the human-facing review harness in `tools/`.

## (a) Review harness

Evidence: `tools/build_review_harness.mjs` discovers benchmark image assets and
emits HTML plus a manifest; `tools/embed_review_harness.mjs` packages that HTML
and optional WebP payloads; `tools/verify_review_harness_browser.py` launches
Firefox and exercises only the built page. The contract test uses temporary
fixture PNGs and invokes only the two Node tools.

- [M] `tools/build_review_harness.mjs`
- [M] `tools/embed_review_harness.mjs`
- [??] `tools/verify_review_harness_browser.py`

## (b) Quality plan, benchmark tooling, and evidence

Evidence: these files define quality-runner isolation, campaign schemas,
spatial/temporal metrics, sidecar/event assembly, matrix verification, or the
quality-perfection execution plan. They do not implement the playback/render
runtime.

- [M] `benchmarks/quality_sweeps/run_quality_sweep.py`
- [M] `benchmarks/video_corpus/run_quality.sh`
- [M] `benchmarks/video_corpus/run_temporal_quality.sh`
- [??] `benchmarks/quality_sweeps/campaign_matrix.py`
- [??] `benchmarks/quality_sweeps/event_matrix.py`
- [??] `benchmarks/quality_sweeps/m6_6_1_real_spatial_controls.json`
- [??] `benchmarks/quality_sweeps/m6_quality_class_annotations.json`
- [??] `benchmarks/quality_sweeps/m6_schema2_spatial_campaign.json`
- [??] `benchmarks/quality_sweeps/motion_sidecar.py`
- [??] `benchmarks/quality_sweeps/paired_spatial_metrics.py`
- [??] `benchmarks/quality_sweeps/quality_campaign_contract.py`
- [??] `benchmarks/quality_sweeps/quality_lab_contract.py`
- [??] `benchmarks/quality_sweeps/run_quality_campaign.py`
- [??] `benchmarks/quality_sweeps/spatial_matrix.py`
- [??] `benchmarks/quality_sweeps/temporal_matrix.py`
- [??] `benchmarks/quality_sweeps/temporal_metrics.py`
- [??] `benchmarks/quality_sweeps/temporal_sequence.py`
- [??] `benchmarks/video_corpus/benchmark_settings.json`
- [??] `docs/exec-plans/QUALITY_PERFECTION_EXECUTION_SLICES.md`
- [??] `docs/exec-plans/QUALITY_PERFECTION_M0_GATE.md`
- [??] `docs/exec-plans/QUALITY_PERFECTION_M1_GATE.md`
- [??] `docs/exec-plans/QUALITY_PERFECTION_M2_GATE.md`
- [??] `docs/exec-plans/QUALITY_PERFECTION_M3_GATE.md`
- [??] `docs/exec-plans/QUALITY_PERFECTION_M4_GATE.md`
- [??] `docs/exec-plans/QUALITY_PERFECTION_M5_GATE.md`
- [??] `docs/exec-plans/QUALITY_PERFECTION_M6_TOOLING_GATE.md`
- [??] `docs/slice-plan.md`
- [??] `tools/assemble_event_trace.py`
- [??] `tools/assemble_m6_event_matrix.py`
- [??] `tools/assemble_motion_sidecar.py`
- [??] `tools/assemble_spatial_matrix.py`
- [??] `tools/assemble_temporal_matrix.py`
- [??] `tools/measure_temporal_sequence.py`
- [??] `tools/report_quality_matrix_gaps.py`
- [??] `tools/verify_quality_campaign.py`
- [??] `tools/verify_quality_matrix.py`

## (c) Tests and docs

Evidence: these are test registration, contract/CLI tests, fixtures, developer
guidance, progress notes, or test strategy. Tests that inspect category (d)
contracts remain tests; their passing would not approve the runtime path.

- [M] `docs/VIBECODER_DEVELOPER_GUIDE.md`
- [M] `tests/CMakeLists.txt`
- [??] `docs/orchestration/progress-state.md`
- [??] `docs/testing/test-strategy.md`
- [??] `tests/color_metadata_contract_tests.cpp`
- [??] `tests/fixtures/m0_baseline_valid.json`
- [??] `tests/fsr4_motion_contract_tests.cpp`
- [??] `tests/fsr4_postpass_contract_tests.cpp`
- [??] `tests/fsr4_temporal_contract_tests.cpp`
- [??] `tests/jitter_policy_tests.cpp`
- [??] `tests/test_event_trace_assembly.py`
- [??] `tests/test_event_trace_runtime_contract.py`
- [??] `tests/test_jitter_manifest_contract.py`
- [??] `tests/test_m6_event_matrix.py`
- [??] `tests/test_m6_quality_class_annotations.py`
- [??] `tests/test_m6_schema2_campaign.py`
- [??] `tests/test_measure_temporal_sequence_cli.py`
- [??] `tests/test_motion_export_contract.py`
- [??] `tests/test_motion_sidecar.py`
- [??] `tests/test_motion_sidecar_cli.py`
- [??] `tests/test_paired_spatial_metrics.py`
- [??] `tests/test_quality_campaign_contract.py`
- [??] `tests/test_quality_campaign_matrix.py`
- [??] `tests/test_quality_lab_contract.py`
- [??] `tests/test_quality_matrix_gap_report.py`
- [??] `tests/test_quality_runner_contract.py`
- [??] `tests/test_review_harness_contract.py`
- [??] `tests/test_spatial_matrix.py`
- [??] `tests/test_temporal_matrix.py`
- [??] `tests/test_temporal_metadata.py`
- [??] `tests/test_temporal_metrics.py`
- [??] `tests/test_temporal_runner_contract.py`
- [??] `tests/test_temporal_sequence.py`
- [??] `docs/SCOPE_BOUNDARY_AUDIT_2026-08-23.md` (added by this audit)

## (d) Algorithm/runtime path requiring separate review

Evidence: the tracked diffs change shader math/resources, decoded-frame color
metadata and bit depth, Vulkan images/descriptors/barriers, postpass parameters,
motion/history/jitter behavior, and playback wiring. These paths are outside
the review-harness task and remain unapproved by this audit.

- [M] `shaders/fsr4/drm_yuv_to_fsr_input.comp`
- [M] `shaders/fsr4/postpass_composite.comp`
- [M] `shaders/fsr4/prepass_pq_eotf.comp`
- [M] `shaders/fsr4/yuv_to_fsr_input.comp`
- [M] `src/core/PlaybackEngine.cpp`
- [M] `src/media/VideoDecoder.cpp`
- [M] `src/media/VideoDecoder.hpp`
- [M] `src/render/Fsr4DispatchHarness.cpp`
- [M] `src/render/Fsr4DispatchHarness.hpp`
- [M] `src/render/GpuImageUploader.cpp`
- [M] `src/render/GpuImageUploader.hpp`
- [M] `src/render/SideBufferSynth.cpp`
- [M] `src/render/SideBufferSynth.hpp`
- [M] `src/render/upload/YuvConstants.cpp`
- [M] `src/render/upload/YuvConstants.hpp`
- [??] `src/backend/Fsr4PostpassParams.cpp`
- [??] `src/backend/Fsr4PostpassParams.hpp`

## (e) Generated artifact

- [??] `docs/exec-plans/Temporal_Forge_FSR4_Quality_Perfection_Master_Plan.docx`

The DOCX metadata describes it as generated from the quality branch and FSR
reference material. Ignored generated HTML/assets and benchmark result trees
are outside the requested tracked/standard-untracked inventory.

## Harness boundary conclusion

Confirmed: the review-harness task can be completed using only category (a),
its category (c) contract test, and source image/evidence inputs from category
(b). It does not require touching category (d). No code tests were run because
this audit made no code changes; the added manifest is documentation only.
