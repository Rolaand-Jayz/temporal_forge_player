# Temporal Forge FSR4 quality campaign orchestration state

## Orchestration metadata

- **Task:** work through every quality-plan slice in dependency order and test at each milestone
- **Pattern:** long-horizon multi-phase quality/correctness campaign
- **Start date:** 2026-08-22
- **Current phase:** final all-slice audit
- **Status:** in-progress

## Phase log

| Phase | Skill/work | Status | Output location | Completed | Issues |
|---|---|---|---|---|---|
| Plan | project-slicer + test-strategy | complete | `docs/slice-plan.md`, `docs/testing/test-strategy.md` | 2026-08-22 | Execution is serial; milestone gates remain separate from slice work. |
| M0 | baseline/provenance/observability | complete as recorded | `docs/exec-plans/QUALITY_PERFECTION_M0_GATE.md` | 2026-08-22 | Preserve prior artifacts; do not overwrite. |
| M1 | postpass parameter contract | complete as recorded | `docs/exec-plans/QUALITY_PERFECTION_M1_GATE.md` | 2026-08-22 | Recovery evidence remains version-specific and trace-driven. |
| M2 | shared reprojection/FP16 temporal state | complete as recorded | `docs/exec-plans/QUALITY_PERFECTION_M2_GATE.md` | 2026-08-22 | Live GPU evidence must remain distinct from CPU contracts. |
| M3 | causal codec motion | complete as recorded | `docs/exec-plans/QUALITY_PERFECTION_M3_GATE.md` | 2026-08-22 | Real temporal quality matrix still pending. |
| M4 | color/transfer/chroma/bit depth | complete as recorded | `docs/exec-plans/QUALITY_PERFECTION_M4_GATE.md` | 2026-08-22 | Supported hardware/software paths remain separately ranked. |
| M5 | decoded-video jitter policy | complete as recorded | `docs/exec-plans/QUALITY_PERFECTION_M5_GATE.md` | 2026-08-22 | Jitter promotion remains evidence-bound. |
| M6 tooling | campaign, temporal metrics, sidecars, review harness | complete as tooling | `docs/exec-plans/QUALITY_PERFECTION_M6_TOOLING_GATE.md` | 2026-08-23 | Real quality matrix remains open; review-harness browser gate passed outside the sandbox. |
| M6.1 | neutral runner + real spatial controls | complete as capture/evidence | `/tmp/tforge-m6-6-1-cas-fixed-escalated-20260823/m6-6-1-cas-fixed-escalated-20260823T042444Z` | 2026-08-23 | Current learned path still trails spatial controls; this is evidence, not a promotion. |
| M6.2 | strict paired spatial metrics and baseline-aware rankings | complete | `/tmp/tforge-m6-2-paired-existing-20260823` | 2026-08-23 | Four real candidates paired against `base_only_bilinear`; no mismatched rows accepted. |
| M6.3 | causal temporal sidecar validation and temporal metric ingestion | complete | `benchmarks/quality_sweeps/motion_sidecar.py`, `benchmarks/quality_sweeps/temporal_sequence.py`, `.m6-captures/m6-final-spatial-pixels-20260823/temporal-final-48-65-vfr` | 2026-08-23 | Corrected 48–65 VFR captures produced 20/20 identity-validated rows with motion, event, mask, and metric sidecars. |
| M6.4 | mirrored split review harness and portable artifact | complete as artifact and browser check | `temporal-forge-frame55-review-single-file.html` | 2026-08-23 | 353 aliases, 243 lossless WebP payloads, 255.3 MiB; real Firefox interaction pass completed outside the sandbox. |
| M6.5 | complete candidate-matrix verifier | complete | `.m6-captures/m6-final-spatial-pixels-20260823/combined-matrix-48-65.json`, `tools/verify_quality_campaign.py`, `benchmarks/quality_sweeps/campaign_matrix.py` | 2026-08-23 | Strict verifier passes 5 candidates × 4 real scenes/classes = 20 spatial and 20 temporal rows. |
| M6 event metadata | retained-row audit complete; capture complete | `.m6-captures/m6-final-spatial-pixels-20260823/temporal-final-48-65-vfr` | 2026-08-23 | Every row has a candidate-linked event trace; authoritative event cause is `pts_gap`, with frame identity 48–65 and 18 frames. |
| M7 | performance/equivalence/default promotion | complete with explicit non-promotion | `.m7-captures/m7-final-promotion-record.json` | 2026-08-23 | Quality and optimized-production equivalence gates did not pass; rollback identity recorded and runtime defaults were not mutated. |

## Decisions log

| Decision | Rationale | Alternatives considered | Date |
|---|---|---|---|
| Treat zero-row or failed-player captures as failures | The previous runner could return zero with no valid images, creating a false-green sweep. | Accepting empty CSVs; rejected because it hides runtime failure. | 2026-08-23 |
| Use escalated GPU/display execution for live captures | The sandbox cannot access `/run/user/1000/wayland-0`; the same binary reaches Vulkan and FSR when permission is available. | Changing player QPA or reconstruction code; rejected because the failure is environmental. | 2026-08-23 |
| Keep the CAS fix narrow and evidence-bound | The shader previously ignored its incoming composed color; the contract test caught that. | Broader quality tuning; deferred until corrected campaign evidence. | 2026-08-23 |
| Require paired spatial keys to match exactly | A candidate with a missing clip, changed source/output dimensions, frame, quality, or CRF is not a valid comparison. | Pairing by clip name only; rejected because it can hide changed benchmark conditions. | 2026-08-23 |

## Issues log

| Issue | Phase | Severity | Resolution | Date |
|---|---|---|---|---|
| Background player aborted in Qt QGuiApplication under the restricted sandbox | M6.1 | important | Confirmed Wayland permission boundary via coredump and escalated GDB/capture; no player-code workaround applied. | 2026-08-23 |
| Runner reported successful empty CSVs | M6.1 | important | Added failing-first tests, then made shell runner and sweep wrapper return failure for failed/zero-row captures. | 2026-08-23 |
| Current learned output trails spatial controls on the first corrected matrix | M6.1 | important | Recorded as a control result; no tone/sharpening/experiment promotion. Continue with matched temporal and learned evidence. | 2026-08-23 |
| Legacy spatial and temporal evidence cannot safely become schemaVersion 2 | M6.5 | important | Added a capture-free gap report and failing-first tests; observed labels and sidecars remain evidence only and are not promoted into classes or candidate attribution. | 2026-08-23 |
| Still evidence is insufficient for temporal annotation | M6.5 | important | Inspected existing real-world frame-48 outputs and grounded four spatial classes only; explicitly left temporal availability false because adjacent frames, motion, masks, and events are absent. | 2026-08-23 |
| Event metadata is not recoverable from the accepted rows | M6 event metadata | important | The 20 retained rows have blank event metrics; motion sidecars contain only capture initialization resets and do not distinguish forced reset from scene cut. The saved cut-events JSON is unscoped, out of range for eight frames, and has unproven thresholds. No event sidecars were created. | 2026-08-23 |
| Temporal runner copies a caller-selected sidecar onto itself | M6.3 | important | Fixed with path-aware copying; the bounded rerun now exits 0 while retaining same-directory motion and metrics sidecars. | 2026-08-23 |
| Sintel rooftop emitted no causal motion from the original H.264 MP4 | M6.3 | important | Retained evidence showed empty vectors on every frame despite H.264 B/P pictures; one evidence-backed retry re-encoded the same eight frames as P-only lossless H.264 in Matroska, which exported causal vectors on frames 1-7 and passed strict metrics. No validation was weakened. | 2026-08-23 |
| Corrected temporal input lost the injected PTS gap in CFR output | M6 event metadata | important | Rebuilt the derived 48–65 input as timestamp-preserving VFR H.264; smoke capture and all 20 event traces now classify the intended `pts_gap` event. | 2026-08-23 |
| Final matrix join rejected review-asset paths as numeric metrics | M6.5 | important | Added failing-first coverage and preserved nonnumeric asset identity metadata during numeric normalization; strict campaign verification now passes. | 2026-08-23 |
| M7.2 had no format-aware equivalence gate | M7.2 | important | Added failing-first tests and a numeric comparator with explicit rgba8/rgba16f/tensor_fp16/tensor_int8 tolerances; shape and non-finite inputs fail hard. | 2026-08-23 |
| M7.3 fused toggle was not exercised by the proof path | M7.3 | important | Ran the existing proof harness with and without `TFORGE_FSR4_ENABLE_FUSED_INT8`; both passed and produced byte-identical 265,420,800-byte final tensors. No shader or default change was justified. | 2026-08-23 |
| Historical M6 Python fixtures no longer match the authoritative campaign schema | final audit | important | Recovered the missing evidence path for diagnosis; retained the historical tests unchanged because they expect `executedBinary` in a pre-capture campaign and omit required control-source fields. The current 161-test suite passes; provenance was not weakened. | 2026-08-23 |

## Artifacts produced

| Artifact | Path | Phase | Status |
|---|---|---|---|
| M6.1 runner contract tests | `tests/test_quality_runner_contract.py` | M6.1 | current |
| Neutral benchmark settings | `benchmarks/video_corpus/benchmark_settings.json` | M6.1 | current |
| Real spatial-control manifest | `benchmarks/quality_sweeps/m6_6_1_real_spatial_controls.json` | M6.1 | current |
| Escalated five-candidate sweep | `/tmp/tforge-m6-6-1-cas-fixed-escalated-20260823/m6-6-1-cas-fixed-escalated-20260823T042444Z` | M6.1 | current |
| Paired spatial report from real sweep | `/tmp/tforge-m6-2-paired-existing-20260823/paired_rankings.csv` | M6.2 | current |
| Paired spatial metric implementation | `benchmarks/quality_sweeps/paired_spatial_metrics.py` | M6.2 | current |
| Portable review harness | `temporal-forge-frame55-review-single-file.html` | M6 tooling | current, not a quality promotion |
| Real-browser harness validator | `tools/verify_review_harness_browser.py` | M6 tooling | Firefox Marionette checks pass outside the restricted sandbox |
| M6.5 matrix contract | `tools/verify_quality_matrix.py` | M6.5 | contract-tested, real matrix pending |
| M6.5 schema-v2 gap report | `tools/report_quality_matrix_gaps.py`, `tests/test_quality_matrix_gap_report.py` | M6.5 | current; `/tmp/tforge-m6-schema2-gap-report.json` records the saved-evidence gaps |
| M6.5 schema-v2 spatial campaign | `benchmarks/quality_sweeps/m6_schema2_spatial_campaign.json`, `tests/test_m6_schema2_campaign.py` | M6.5 | current; five candidates, four real scenes, scene-specific classSelections, temporal pending |
| Visually grounded M6 class annotations | `benchmarks/quality_sweeps/m6_quality_class_annotations.json`, `tests/test_m6_quality_class_annotations.py` | M6.5 | current; four spatial entries, temporal evidence explicitly unavailable |
| First identity-validated temporal capture | `/tmp/tforge-m6-3-bilinear-fabric-rerun-20260823-escalated` | M6.3 | current; one real sequence, wrapper exit 0, not a complete matrix |
| Corrected Sintel rooftop temporal capture | `/tmp/tforge-m6-3-bilinear-rooftop-retry-p-only-20260823` | M6.3 | current; one real baseline sequence, wrapper exit 0, not a complete matrix |
| Complete M6 schema-v2 matrix | `.m6-captures/m6-final-spatial-pixels-20260823/combined-matrix-48-65.json` | M6.5 | current; strict verifier passes 20 spatial and 20 temporal rows |
| M7.1 real-only timing matrix | `.m7-captures/reference-real-matrix.csv` | M7.1 | current; 12 real rows, 23-column stage/pipeline timing schema, no synthetic clips |
| M7.2 equivalence comparator | `tools/compare_stage_equivalence.py`, `tests/test_stage_equivalence.py` | M7.2 | current; test-first contract passes; integration with live stage dumps remains the next slice |
| M7.3 bounded live audit | `/tmp/tforge-m7-clear-final.json`, `/tmp/tforge-m7-fused-final.bin` | M7.3 | current; both proof runs passed, SHA-256 `5af1bedd7d495955ad7aee3bea5912d73095738ced6761b0edf3fecddb16761c` |
| M7.5 promotion decision | `.m7-captures/m7-final-promotion-evidence.json`, `.m7-captures/m7-final-promotion-record.json`, `tools/record_quality_promotion.py`, `tests/test_promotion_record.py` | M7.5 | current; explicit `not_promoted`, with binary hash, rollback identity, artifacts, limitations, and gate reasons |

## Context notes

- Do not reset, clean, or delete the existing dirty worktree.
- The current M6.1 sweep contains five candidates, four real clips per
  candidate, explicit 426x240 → 1920x1080 dimensions, neutral settings, and
  nonzero runner results.
- The paired report ranks `base_only_bilinear` above the current learned path.
  This is the expected diagnostic result to preserve, not a reason to tune
  blindly.
- Normal benchmark capture commands require the display/GPU permission used by
  the escalated run; a sandbox-only abort is not quality evidence.

## Next action

- Use `m6_schema2_spatial_campaign.json` as the current spatial contract and
  `/tmp/tforge-m6-schema2-gap-report.json` to define the next evidence capture
  slice only after each missing temporal class/provenance/sidecar field is
  grounded.
- For the next event slice, capture a candidate-linked sequence spanning a
  known event and retain the runtime event trace, timestamps, reset cause, and
  threshold provenance beside the candidate/reference frames. Do not derive
  event indices or thresholds from residuals, stills, or the existing motion
  `reset` field.
- After those fields are captured and attributed, produce the real candidate ×
  scene × class spatial and temporal matrix, then run it through
  `tools/verify_quality_matrix.py`.
- Preserve the real-browser evidence for the standalone and portable harness;
  the browser portion of M6 is closed.
- M6 is now open: the complete spatial and temporal candidate/class tables
  pass the verifier together, with explicit unrecorded Git provenance.
- M7.1 is complete: `.m7-captures/reference-real-matrix.csv` contains the
  real-only reference timing matrix. Decode, upload, presentation wrapper,
  pipeline, dispatch, and GPU timing fields are populated; the presentation
  value is CPU submission/wrapper timing, not a claim about GPU queue time.
- M7.2 is complete as its test-first diagnostic contract. The comparator is
  intentionally independent of visual preference; the next slice must feed it
  actual clear-reference and optimized/fused stage/final dumps before any
  promotion decision.
- M7.3 audit is complete without a code change: the existing fused toggle did
  not alter the proof path output, so there is no evidence-backed optimization
  to promote from this slice. Keep the path and defaults unchanged.
- M7.5 is complete as a decision record, not a promotion: the quality and
  optimized-production equivalence gates failed, so the campaign stops at the
  documented evidence boundary with `base_only_bilinear` retained as rollback
  identity.
- Do not change shaders, defaults, or quality settings during M7 diagnostics.
