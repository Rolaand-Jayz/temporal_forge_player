# Temporal Forge FSR4 quality plan — execution slices

## Metadata

- **Source spec:** `docs/exec-plans/Temporal_Forge_FSR4_Quality_Perfection_Master_Plan.docx`
- **Supporting plan:** `docs/exec-plans/QUALITY_RECONSTRUCTION_PLAN.md`
- **Related execution notes:** `docs/exec-plans/QUALITY_PERFECTION_EXECUTION_SLICES.md`
- **Status:** executing serially; milestone status is recorded in
  `docs/orchestration/progress-state.md`
- **Milestones:** 8
- **Implementation slices:** 33
- **Scope rule:** reconstruction correctness first; quality experiments and default promotion stay locked until their gates pass

This is the work breakdown, not a claim that the current dirty worktree has
passed any future gate. Existing changes remain untouched. The next execution
turn selects one milestone, writes and reviews that milestone's tests, then
implements its slices one at a time. The old "parallel group" labels below
identify ownership and file-conflict history; they do not authorize concurrent
implementation in this execution.

## Operating contract

Each milestone is executed in this order:

1. Freeze the milestone contract and write all of its tests and fixtures before
   changing implementation code.
2. Run that new test pack once against the current baseline and record the
   failures. The assertions are now the contract; they are not rewritten to
   accommodate an implementation.
3. Implement the milestone's bounded slices in dependency order. A slice may
   use a compile or syntax check to keep the tree buildable, but it does not
   trigger the milestone acceptance suite.
4. Run the complete milestone gate once all of the slices are present. The gate
   includes the new tests, relevant existing regression tests, and the required
   live or artifact check.
5. If the gate fails, stop at that milestone. Preserve the failing fixture,
   logs, binary/config provenance, and decision; do not hide the failure with
   tone, sharpening, fallback labels, or a later slice.

No slice may introduce a stub, placeholder, silent fallback, or dead control.
Diagnostic paths may be slower, but they must be explicitly selected and must
not become the normal playback path by accident.

## Dependency graph

```text
M0  trustworthy baseline and stage observability
 └── M1  typed/recovered FSR4 postpass parameters
      └── M2  one shared causal reprojection + FP16 model state
           └── M3  causal codec motion
                └── M4  transfer/chroma/bit-depth correctness
                     └── M5  decoded-video jitter policy
                          └── M6  corrected learned-quality campaign + review harness
                               └── M7  performance equivalence + default promotion
```

The dependency is intentionally conservative. M1 and M2 define whether the
learned path is being judged fairly. M3 and M4 make temporal and input evidence
trustworthy. M5 is not interpretable until those contracts are fixed. M6 is the
first milestone allowed to start new learned-quality comparisons. M7 is the
only milestone allowed to promote a default.

## Milestone 0 — trustworthy baseline and observability

**Outcome:** an immutable pre-parity control and a benchmark that records every
image-affecting input and can prove what each diagnostic stage produced.

**Tests written first:**

- provenance schema test for commit, binary hash, config, ordinary settings,
  source metadata, dimensions, decode path, timing, and output paths;
- output-dimension test separating source, model, reconstruction, history, and
  display dimensions;
- D0–D9 stage-artifact contract test for declared format, dimensions, and finite
  values;
- hidden-settings test proving a benchmark cannot silently inherit a user's
  image-affecting settings;
- immutable-artifact test proving a rerun creates a new record rather than
  overwriting the prior control.

### Slice 0.1 — freeze the baseline

- **Dependencies:** none
- **Scope:** record the authority commit, binary hash, corpus rows, source
  metadata, known quality numbers, and performance controls in a new
  versioned baseline manifest. Do not rewrite old run directories or benchmark
  images.
- **Files:** `benchmarks/quality_sweeps/baselines/`,
  `docs/exec-plans/QUALITY_RECONSTRUCTION_PLAN.md`, new baseline fixtures.
- **Complexity:** S
- **Parallel group:** M0-A

### Slice 0.2 — define provenance and stage-artifact schemas

- **Dependencies:** 0.1
- **Scope:** define machine-readable records for run provenance and D0–D9
  artifacts, including format, dimensions, frame identity, finite-value status,
  and failure reason.
- **Files:** `benchmarks/quality_sweeps/quality_lab_contract.py`,
  `benchmarks/quality_sweeps/quality_campaign_contract.py`,
  `tools/verify_quality_campaign.py`, contract fixtures.
- **Complexity:** M
- **Parallel group:** M0-B

### Slice 0.3 — wire capture metadata and stage hooks

- **Dependencies:** 0.2
- **Scope:** make the benchmark runner and the player diagnostic path emit the
  declared provenance and stage records without changing reconstruction math.
  Missing or non-finite requested stages must fail loudly.
- **Files:** `benchmarks/video_corpus/run_quality.sh`,
  `benchmarks/quality_sweeps/run_quality_sweep.py`,
  `src/core/PlaybackEngine.cpp`,
  `src/render/Fsr4DispatchHarness.cpp/.hpp`, diagnostic output helpers.
- **Complexity:** M
- **Parallel group:** M0-C

### M0 gate

- **Build:** player and benchmark tools build/syntax-check.
- **Tests:** complete M0 test pack plus existing quality-lab contract tests.
- **Live/artifact check:** one unchanged baseline capture can be reconstructed
  from its saved manifest and is not overwritten by a second capture.
- **Pass condition:** all requested provenance is present, dimensions are
  unambiguous, source sharpness is explicitly recorded as zero for the parity
  baseline, and every requested diagnostic artifact is finite or reports a
  truthful failure.
- **Stop condition:** inherited settings, missing stage identity, ambiguous
  output dimensions, or overwritten evidence.

## Milestone 1 — FSR4 postpass parameter contract

**Outcome:** the recovered 4.1 extra region is typed as 222 FP32 values,
validated, consumed according to an explicit access map, and observable in a
diagnostic trace.

**Tests written first:**

- byte-layout/bounds test for offset `130088`, 222 FP32 values, and padding;
- endian, alignment, finite-value, and known-initializer decode fixtures;
- host binding to shader parameter-consumption contract;
- parameter-load/contribution trace test;
- reset determinism test for decoded parameters and finite postpass output.

### Slice 1.1 — type and decode the 4.1 region

- **Dependencies:** M0 gate
- **Scope:** replace legacy scale-zone semantics with a typed postpass-region
  accessor, preserve only necessary compatibility naming, and validate the
  known offsets.
- **Files:** `src/backend/WeightBlob.hpp/.cpp`,
  `src/backend/Fsr4PostpassParams.hpp/.cpp`,
  `tests/fsr4_weight_tests.cpp`,
  `tests/fsr4_postpass_contract_tests.cpp`, fixtures.
- **Complexity:** M
- **Parallel group:** sequential

### Slice 1.2 — define the shader access map

- **Dependencies:** 1.1
- **Scope:** document and implement the recovered 4.1 parameter loads and their
  roles in filter, bias, scale, and blend composition. Do not infer an access
  merely from an image improvement.
- **Files:** `shaders/fsr4/postpass_composite.comp`,
  `src/render/Fsr4DispatchHarness.cpp/.hpp`, shader-contract fixtures.
- **Complexity:** L
- **Parallel group:** sequential

### Slice 1.3 — add postpass diagnostics

- **Dependencies:** 1.2
- **Scope:** emit decoded parameter values, load destinations, bias/blend
  contributions, and finite output diagnostics behind an explicit diagnostic
  switch. Production composition remains unchanged until the gate passes.
- **Files:** `src/backend/Fsr4PostpassParams.*`,
  `src/render/Fsr4DispatchHarness.*`,
  `benchmarks/quality_sweeps/quality_lab_contract.py`, diagnostic fixtures.
- **Complexity:** M
- **Parallel group:** sequential

### M1 gate

- **Build:** player, shader generation, and CPU contract tests build.
- **Tests:** all M1 tests, existing weight/blob tests, and shader validation.
- **Live/artifact check:** a diagnostic frame shows the parameter trace and
  finite postpass outputs; every active recovered load is accounted for.
- **Pass condition:** no active parameter remains unexplained and the disabled
  versus enabled plumbing control is distinguishable without relying on visual
  judgment.
- **Stop condition:** ambiguous recovered access, non-finite values, or a
  parameter region that is bound but not observable.

## Milestone 2 — one shared reprojection and FP16 temporal state

**Outcome:** prepass and postpass use one target-grid reprojected-color sample,
history/recurrent state has the declared precision, and reset/ping-pong lifetime
is causal and deterministic.

**Tests written first:**

- descriptor/resource identity test for prepass write and postpass read;
- same-coordinate test for history color and recurrent state, including border
  and disocclusion cases;
- causal read/write and in-flight-slot test;
- FP16 format, usage, dimension, and ping-pong-role test;
- unclamped model-range test for values above 1 and below 0;
- first-frame, seek, scene-cut, and teardown reset test.

### Slice 2.1 — add the shared resource graph

- **Dependencies:** M1 gate
- **Scope:** add an explicit per-dispatch-slot target-grid reprojected-color
  resource and its ownership/descriptor contract, keeping display output
  separate from model state.
- **Files:** `src/render/Fsr4DispatchHarness.hpp/.cpp`,
  `src/render/GpuImageUploader.hpp/.cpp`,
  `tests/fsr4_temporal_contract_tests.cpp`.
- **Complexity:** L
- **Parallel group:** sequential

### Slice 2.2 — move model state to FP16

- **Dependencies:** 2.1
- **Scope:** allocate history and preserve recurrent state as RGBA16F model
  resources, with format and range diagnostics. Do not change display format.
- **Files:** `src/render/GpuImageUploader.*`,
  `src/render/Fsr4DispatchHarness.*`, Vulkan resource fixtures.
- **Complexity:** M
- **Parallel group:** sequential

### Slice 2.3 — publish and consume the exact reprojection

- **Dependencies:** 2.2
- **Scope:** make prepass write the mixed history value once, make features and
  postpass read that value, and remove the independent parity-path history
  reprojection.
- **Files:** `shaders/fsr4/prepass_pq_eotf.comp`,
  `shaders/fsr4/postpass_composite.comp`,
  `src/render/Fsr4DispatchHarness.*`.
- **Complexity:** L
- **Parallel group:** sequential

### Slice 2.4 — finish barriers, resets, and diagnostics

- **Dependencies:** 2.3
- **Scope:** add layout transitions, barriers, in-flight ownership, reset and
  teardown handling, and resource readback diagnostics. Keep any legacy path
  explicit and non-default.
- **Files:** `src/render/Fsr4DispatchHarness.*`,
  `src/core/PlaybackEngine.cpp`,
  `benchmarks/video_corpus/run_temporal_quality.sh`, reset fixtures.
- **Complexity:** L
- **Parallel group:** sequential

### M2 gate

- **Build:** player and shader targets build.
- **Tests:** all M2 resource, coordinate, range, and reset tests plus existing
  media/backend regressions.
- **Live/artifact check:** GPU validation or an equivalent live dispatch trace
  proves one reprojection resource is written/read in the same frame and state
  survives a multi-frame sequence without invalid access.
- **Pass condition:** no second parity-path history reprojection, no accidental
  model-state clamp, and deterministic reset behavior.
- **Stop condition:** GPU runtime unavailable, synchronization uncertainty, or
  any resource graph mismatch. Static tests alone do not promote M2.

## Milestone 3 — causal codec motion

**Outcome:** only causal motion can enter previous-frame history reprojection,
with documented sign/scale, deterministic overlap handling, and confidence.

**Tests written first:**

- past/current/future direction preservation fixture;
- future-reference exclusion test;
- sign/scale mapping fixture for source, model, and target coordinates;
- sparse, border, and overlapping-block policy tests;
- adjacent-frame validation and low-confidence repair fixture;
- confidence propagation and scene-cut reset tests.

### Slice 3.1 — preserve reference direction

- **Dependencies:** M2 gate
- **Scope:** retain codec motion source/reference direction through decode,
  upload, and GPU expansion; define the causal candidate filter.
- **Files:** `src/media/VideoDecoder.hpp/.cpp`,
  `src/render/GpuImageUploader.hpp/.cpp`,
  `shaders/fsr4/codec_motion_expand.comp`,
  `tests/fsr4_motion_contract_tests.cpp`.
- **Complexity:** M
- **Parallel group:** sequential

### Slice 3.2 — define coordinate and overlap policy

- **Dependencies:** 3.1
- **Scope:** make sign, scale, block overlap, sparse coverage, borders, and
  non-adjacent past references deterministic and inspectable.
- **Files:** `shaders/fsr4/codec_motion_expand.comp`,
  `src/render/SideBufferSynth.hpp/.cpp`, motion fixtures.
- **Complexity:** M
- **Parallel group:** sequential

### Slice 3.3 — validate, repair, and propagate confidence

- **Dependencies:** 3.2
- **Scope:** compare cheap codec motion to adjacent displayed frames, repair
  only low-confidence regions, expose confidence to reactive/disocclusion
  behavior, and make scene reset win over stale motion.
- **Files:** `src/render/SideBufferSynth.*`,
  `src/core/PlaybackEngine.cpp`,
  `benchmarks/video_corpus/run_temporal_quality.sh`,
  `tests/sidebuffer_tests.cpp`.
- **Complexity:** L
- **Parallel group:** sequential

### M3 gate

- **Build:** player and motion tests build.
- **Tests:** complete M3 motion pack and existing decoder/side-buffer tests.
- **Live/artifact check:** a real sequence exports motion metadata/sidecar with
  rejected future vectors and confidence counts; scene-cut reset is visible in
  the trace.
- **Pass condition:** motion semantics are independently proven; subjective
  “less ghosting” is not used as the specification.
- **Stop condition:** direction or reference identity is lost, overlap is
  insertion-order dependent, or future motion reaches history.

## Milestone 4 — color, transfer, chroma, and bit depth

**Outcome:** decoded pixels enter the model in an explicit metadata-driven
domain, and software and supported DRM paths preserve source precision.

**Tests written first:**

- metadata propagation test for range, matrix, transfer, primaries, chroma
  location, and bit depth;
- SDR BT.709 limited-range fixture;
- SDR BT.709 full-range fixture;
- HDR/PQ or HLG high-bit-depth fixture where supported;
- colored one-pixel diagonal and text-edge chroma-siting fixture;
- 10/12-bit precision and no-unintended-8-bit-round-trip test;
- model/display separation test proving presentation encoding does not mutate
  model history or recurrent state;
- explicit source-sharpening-zero test.

### Slice 4.1 — propagate source metadata

- **Dependencies:** M3 gate
- **Scope:** extend the decoded-frame contract with the metadata FFmpeg
  exposes, preserving matrix/transfer/range as separate properties.
- **Files:** `src/media/VideoDecoder.hpp/.cpp`,
  decoder metadata fixtures, `tests/color_metadata_contract_tests.cpp`.
- **Complexity:** M
- **Parallel group:** sequential

### Slice 4.2 — implement transfer, range, and chroma contracts

- **Dependencies:** 4.1
- **Scope:** define the model-color transform, use transfer metadata, and derive
  4:2:0 sample phase from chroma location rather than a fixed center assumption.
- **Files:** `src/render/upload/YuvConstants.hpp/.cpp`,
  `shaders/fsr4/yuv_to_fsr_input.comp`,
  `shaders/fsr4/drm_yuv_to_fsr_input.comp`,
  `shaders/fsr4/prepass_pq_eotf.comp`.
- **Complexity:** L
- **Parallel group:** sequential

### Slice 4.3 — preserve high-bit-depth samples

- **Dependencies:** 4.2
- **Scope:** use bit-depth-aware R16/P010 resources and byte-copy/normalization
  rules for 10/12-bit paths; keep the FP16 model boundary independent of
  source storage.
- **Files:** `src/render/GpuImageUploader.*`,
  `src/render/upload/YuvConstants.*`,
  high-bit-depth fixtures.
- **Complexity:** L
- **Parallel group:** sequential

### Slice 4.4 — validate software/DRM parity and quarantine source sharpening

- **Dependencies:** 4.3
- **Scope:** validate the supported software and DRM paths separately, ensure
  source sharpening is forced to zero for parity captures, and keep display
  sharpening outside the model input.
- **Files:** `shaders/fsr4/yuv_to_fsr_input.comp`,
  `shaders/fsr4/drm_yuv_to_fsr_input.comp`,
  `benchmarks/video_corpus/run_quality.sh`,
  `benchmarks/video_corpus/benchmark_settings.json`.
- **Complexity:** M
- **Parallel group:** sequential

### M4 gate

- **Build:** player, shader, and metadata tests build.
- **Tests:** complete M4 color/precision pack and media regressions.
- **Live/artifact check:** separate captures for SDR limited, SDR full, and one
  HDR/high-bit-depth path show recorded metadata and no hidden tone correction.
- **Pass condition:** no permanent exposure/contrast compensation is needed to
  make the transfer contract appear correct; no >8-bit path stores samples in
  R8 resources.
- **Stop condition:** unsupported metadata silently falls back, chroma phase is
  guessed, or the hardware/software paths are mixed in one ranking.

## Milestone 5 — decoded-video jitter policy

**Outcome:** select a jitter policy from matched temporal evidence rather than
  assuming game-render jitter benefits decoded video.

**Tests written first:**

- mode contract test for off/current/reduced/controlled amplitude and phase;
- static-region flicker test;
- edge-position variance test;
- motion-compensated residual test;
- occlusion/disocclusion ghost-duration test;
- seek/scene-cut phase-reset test.

### Slice 5.1 — make jitter policy explicit

- **Dependencies:** M4 gate
- **Scope:** add exact jitter mode, amplitude, phase, and source-frame metadata
  to capture manifests and runtime diagnostics.
- **Files:** `src/render/SideBufferSynth.*`,
  `benchmarks/video_corpus/run_quality.sh`,
  `benchmarks/quality_sweeps/quality_lab_contract.py`,
  `tests/jitter_policy_tests.cpp`.
- **Complexity:** M
- **Parallel group:** sequential

### Slice 5.2 — implement controlled modes

- **Dependencies:** 5.1
- **Scope:** implement no-jitter, reduced, and deterministic controlled modes
  without changing history ownership or motion semantics. Each mode must be
  explicit; no mode is silently chosen by a failed option.
- **Files:** `src/render/SideBufferSynth.*`, jitter fixtures.
- **Complexity:** M
- **Parallel group:** M5-A

### Slice 5.3 — capture and measure matched sequences

- **Dependencies:** 5.2
- **Scope:** capture the same frames/content classes under J0–J3, measure
  temporal metrics, and record human-review references. Still-frame ranking is
  insufficient for this milestone.
- **Files:** `benchmarks/video_corpus/run_temporal_quality.sh`,
  `benchmarks/quality_sweeps/temporal_metrics.py`,
  `benchmarks/quality_sweeps/temporal_sequence.py`,
  sequence fixtures.
- **Complexity:** L
- **Parallel group:** M5-B

### M5 gate

- **Build:** player and temporal tooling build/syntax-check.
- **Tests:** complete M5 policy and metric pack plus existing temporal runner
  tests.
- **Live/artifact check:** matched sequence tables and human review cover
  static regions, thin edges/text, motion, occlusion, and scene cuts.
- **Pass condition:** the selected mode wins or is the least harmful across the
  required temporal failure modes; no still-only promotion.
- **Stop condition:** sequence capture is not aligned, motion is guessed, or a
  mode is promoted from a single still metric.

## Milestone 6 — corrected learned-quality campaign and review harness

**Outcome:** run the first interpretable learned campaign only after M1–M5,
with paired metrics, complete provenance, and a distributable human-review
artifact.

**Tests written first:**

- campaign manifest and candidate-config schema test;
- per-class paired-metric and worst-case/median aggregation test;
- temporal metric test for flicker, motion-compensated error, edge variance,
  ghost persistence, and reset recovery;
- non-regression test against immutable pre-parity controls;
- review-harness manifest test proving every valid real-world asset is selectable
  on either side, synthetic families are excluded, and no invented paths exist;
- browser contract test for one draggable split image, independent left/right
  selectors, URL/hash restoration, fit/zoom/pan, and max-resolution 1:1 scaling.

### Slice 6.1 — campaign schema and runner bridge

- **Dependencies:** M5 gate
- **Scope:** represent candidates, source/output dimensions, exact configs,
  quality class, binary/settings provenance, and review assets without full-name
  comparison dropdowns or cross-corpus ranking.
- **Files:** `benchmarks/quality_sweeps/quality_campaign_contract.py`,
  `benchmarks/quality_sweeps/run_quality_campaign.py`,
  `tools/verify_quality_campaign.py`, campaign fixtures.
- **Complexity:** M
- **Parallel group:** M6-A

### Slice 6.2 — paired spatial metrics and verification

- **Dependencies:** 6.1
- **Scope:** add per-clip deltas, median, mean, worst-case, output-dimension
  checks, and candidate invariants. Do not merge different corpus subsets or
  intended output sizes.
- **Files:** `benchmarks/quality_sweeps/run_quality_sweep.py`,
  `tools/verify_quality_campaign.py`,
  `tests/test_quality_campaign_contract.py`, metric fixtures.
- **Complexity:** M
- **Parallel group:** M6-B

### Slice 6.3a — strict temporal sidecar validation

- **Dependencies:** 6.1
- **Scope:** validate causal motion sidecars from exported records. Reject
  frame-zero motion, missing or duplicate frames, invalid vectors, ambiguous
  direction, and identity-motion fallback before any metric is computed.
- **Files:** `benchmarks/quality_sweeps/motion_sidecar.py`,
  `tools/assemble_motion_sidecar.py`, sidecar fixtures.
- **Complexity:** M
- **Parallel group:** sequential

### Slice 6.3b — temporal alignment and metric ingestion

- **Dependencies:** 6.3a
- **Scope:** join candidate/reference frames by exact scene, candidate, frame,
  and dimension keys; ingest temporal metrics; leave unavailable event metrics
  blank rather than guessing them.
- **Files:** `benchmarks/quality_sweeps/temporal_metrics.py`,
  `benchmarks/quality_sweeps/temporal_sequence.py`,
  `tools/measure_temporal_sequence.py`,
  `benchmarks/video_corpus/run_temporal_quality.sh`.
- **Complexity:** M
- **Parallel group:** sequential

### Slice 6.4a — review asset manifest and pool parity

- **Dependencies:** 6.1
- **Scope:** discover structured real-world asset metadata, exclude synthetic
  and invented paths, and prove that both sides receive the exact same pool.
- **Files:** `tools/build_review_harness.mjs`,
  `tests/test_review_harness_contract.py`, generated ignored review manifest.
- **Complexity:** M
- **Parallel group:** sequential

### Slice 6.4b — dependent selector resolution

- **Dependencies:** 6.4a
- **Scope:** resolve scene, input resolution, output resolution, technique, and
  applicable stackable modifiers from the manifest. Correct invalid prior
  selections to a valid asset and keep left/right state independent while
  sharing the asset pool.
- **Files:** `tools/build_review_harness.mjs`,
  `tests/test_review_harness_contract.py`.
- **Complexity:** M
- **Parallel group:** sequential

### Slice 6.4c — split viewer and pixel inspection

- **Dependencies:** 6.4b
- **Scope:** keep one image with one draggable divider as the primary view;
  update both halves in real time; support fit, true 100% mode, zoom, and pan;
  scale lower-resolution sources into the highest-resolution comparison canvas
  without claiming that the source contains higher-resolution pixels.
- **Files:** `tools/build_review_harness.mjs`,
  `tests/test_review_harness_contract.py`, browser fixtures.
- **Complexity:** M
- **Parallel group:** sequential

### Slice 6.4d — distributable artifact packaging

- **Dependencies:** 6.4c
- **Scope:** generate the standalone folder and a self-contained HTML file,
  preserve asset identity/hash integrity, stream large payloads without a
  single giant Node string, and reject malformed or impractically oversized
  output rather than reporting success. Synthetic assets remain test-only.
- **Files:** `tools/embed_review_harness.mjs`,
  `tests/test_review_harness_contract.py`, generated ignored review artifacts.
- **Complexity:** M
- **Parallel group:** sequential

### Slice 6.5 — complete candidate-matrix assembly and verification

- **Dependencies:** 6.2, 6.3b, 6.4d
- **Scope:** verify complete candidate × scene × class × resolution coverage;
  join spatial and temporal rows by exact keys; reject missing combinations,
  duplicate rows, changed dimensions, failed/zero-row captures, missing
  provenance, and review assets absent from the declared campaign.
- **Files:** `benchmarks/quality_sweeps/quality_campaign_contract.py`,
  `tools/verify_quality_campaign.py`, matrix fixtures and tests.
- **Complexity:** M
- **Parallel group:** sequential

### M6 gate

- **Build:** all campaign tools and the review-harness generator build/check.
- **Tests:** complete M6 Python suite, campaign/matrix verifier, temporal CLI
  tests, artifact parser/load check, and browser interaction contract.
- **Live/artifact check:** complete matched real-world candidate/class matrix,
  aligned temporal rows, retained finalist stills/sequences, standalone folder,
  and portable single-file review output.
- **Pass condition:** at least one nonzero learned contribution beats the best
  spatial control on a meaningful class without unacceptable difficult-class or
  temporal regressions; otherwise stop at the core-unlock decision with the
  failed evidence preserved.
- **Stop condition:** zero-row/failed captures are represented as success,
  sidecar alignment is guessed, review assets differ by side, the matrix is
  incomplete, the browser runtime is unavailable, or the portable artifact is
  malformed or exceeds the declared distribution size budget.

## Milestone 7 — performance equivalence and default promotion

**Outcome:** optimize only the verified winner, prove semantic equivalence, and
promote a reversible production default inside the real-time budget.

**Tests written first:**

- GPU timing decomposition for reconstruction, decode/upload, and presentation;
- mean/p50/p95 frame-time budget test at supported source/output sizes;
- memory, barriers, allocation, and synchronization regression test;
- clear-reference versus optimized-path equivalence fixture;
- rollback configuration test;
- default-policy test proving diagnostic knobs are disabled unless explicitly
  requested.

### Slice 7.1 — measure the verified reference path

- **Dependencies:** M6 gate
- **Scope:** measure the corrected clear path with separate GPU, CPU, decode,
  upload, and presentation timings; identify actual bottlenecks before changing
  shaders or resource formats.
- **Files:** `benchmarks/quality_sweeps/run_quality_sweep.py`,
  `benchmarks/video_corpus/run_quality.sh`,
  `src/render/Fsr4DispatchHarness.*`, performance fixtures.
- **Complexity:** M
- **Parallel group:** M7-A

### Slice 7.2 — establish semantic equivalence checks

- **Dependencies:** 7.1
- **Scope:** compare any optimized/fused implementation against the clear
  verified reference at internal stages and final output, with tolerances tied
  to declared formats rather than visual preference.
- **Files:** `tests/fsr4_harness_tests.cpp`,
  `tests/fsr4_temporal_contract_tests.cpp`,
  equivalence fixtures, diagnostic comparison tooling.
- **Complexity:** L
- **Parallel group:** M7-B

### Slice 7.3 — optimize without changing semantics

- **Dependencies:** 7.2
- **Scope:** remove redundant history work, fuse only proven postpass work,
  re-profile RDNA3 register pressure/occupancy, and keep one normal FSR stage.
  Narrower formats remain explicit experiments, never silent substitutions.
- **Files:** `shaders/fsr4/prepass_pq_eotf.comp`,
  `shaders/fsr4/postpass_composite.comp`,
  `src/render/Fsr4DispatchHarness.*`, performance manifests.
- **Complexity:** L
- **Parallel group:** sequential

### Slice 7.4 — final gate, rollback, and promotion record

- **Dependencies:** 7.3
- **Scope:** rerun the full quality/performance gate, record the promoted
  binary/config/artifacts/limitations, and make the pre-promotion control
  restorable.
- **Files:** `docs/exec-plans/QUALITY_RECONSTRUCTION_PLAN.md`,
  `docs/exec-plans/QUALITY_PERFECTION_M7_GATE.md`,
  runtime default configuration, release manifest.
- **Complexity:** M
- **Parallel group:** sequential

### Slice 7.5 — record promotion decision and rollback identity

- **Dependencies:** 7.4
- **Scope:** record the final binary/config/artifact hashes, known-good
  rollback candidate, gate results, and limitations. Promote only when quality,
  equivalence, performance, and diagnostic-default gates all pass; otherwise
  record an explicit non-promotion and stop at the evidence boundary.
- **Files:** `tools/record_quality_promotion.py`,
  `tests/test_promotion_record.py`, M7 evidence records.
- **Complexity:** M
- **Parallel group:** sequential

### M7 gate

- **Build:** release/player build and all enabled regression tests.
- **Tests:** complete M7 performance/equivalence/rollback pack plus the full
  prior milestone suites.
- **Live/artifact check:** fresh-session rerun reproduces the result direction;
  GPU mean/p50/p95 and pipeline CPU remain within the declared budget.
- **Pass condition:** optimized output is semantically equivalent to the clear
  winner, reversible, and no diagnostic setting leaks into normal playback.
- **Stop condition:** performance gain requires semantic drift, equivalence is
  unproven, or rollback cannot restore the immutable control.

M7.5 result for the current worktree: `not_promoted`. The current learned
candidate trails `base_only_bilinear`, and the bounded proof comparison did not
exercise a verified optimized production/reference pair. The rollback identity,
binary hash, artifacts, and limitations are recorded in
`.m7-captures/m7-final-promotion-record.json`; runtime defaults were not
mutated.

## Parallel work groups and shared-file rules

| Group | Work that can run in parallel | Constraint |
|---|---|---|
| M0-A/B | baseline freeze and schema drafting | 0.3 waits for the schema contract |
| M5-A/B | jitter mode implementation and fixture preparation | capture waits for modes to be complete |
| M6-B | spatial metrics, temporal sidecars, and review harness | execute strictly in dependency order: 6.2 -> 6.3a -> 6.3b -> 6.4a -> 6.4b -> 6.4c -> 6.4d -> 6.5 |
| M7-A/B | performance measurement and equivalence fixture preparation | optimization waits for both |

Several implementation files are intentionally shared across milestones:
`postpass_composite.comp`, `Fsr4DispatchHarness.*`, `GpuImageUploader.*`,
`VideoDecoder.*`, and `SideBufferSynth.*`. They are modified only in the
listed dependency order; no parallel slice may edit one of these files without
an explicit handoff.

## Risk markers

| Milestone | Risk | Why | Mitigation |
|---|---|---|---|
| M0 | High | A hidden setting or mislabeled dimension invalidates every later comparison. | Immutable manifests, isolated config, provenance tests, no overwrite. |
| M1 | High | 4.1 postpass access evidence may be incomplete or ambiguous. | Typed offsets, known bytes, access trace, stop on unexplained loads. |
| M2 | High | Vulkan resource lifetime and shader synchronization can fail only at runtime. | CPU contract tests plus live GPU dispatch/reset gate; static pass is insufficient. |
| M3 | High | Codec motion direction is not the same as adjacent-frame identity. | Causal fixtures, adjacent validation, explicit confidence and reset. |
| M4 | High | Transfer/chroma/bit-depth errors can masquerade as reconstruction quality. | Separate sample classes and software/DRM rankings; no tone compensation. |
| M5 | Medium | Jitter benefit is a content- and sequence-dependent hypothesis. | Matched sequences and temporal metrics; no still-only promotion. |
| M6 | High | A runner or review manifest can report a plausible but false comparison. | Strict schemas, zero-row failure, paired metrics, same asset pool on both sides. |
| M7 | High | Optimization can silently change semantics or destroy frame-time budget. | Clear reference, equivalence tests, percentile timings, rollback config. |

## Global stop conditions

Stop and document rather than advance when:

- the GPU/runtime cannot execute a required live gate;
- a source/reference artifact cannot be matched causally;
- a recovered behavior remains ambiguous after available evidence is exhausted;
- a quality win depends on hidden settings, tone compensation, source
  sharpening, recursive FSR, or relabeled fallbacks;
- the runner reports success with no valid rows or missing artifacts; or
- performance cannot preserve the declared real-time budget.

The failing fixture, command, logs, binary/config provenance, and decision stay
in the evidence trail. No destructive cleanup is part of this plan.
