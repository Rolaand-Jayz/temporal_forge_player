# Temporal Forge FSR4 quality campaign: execution slices

This document turns `Temporal_Forge_FSR4_Quality_Perfection_Master_Plan.docx`
into a sequence that can actually be executed without losing causal evidence.
It is a planning document only. It does not authorize implementation, new
captures, model changes, or quality experiments by itself.

## Operating rules

1. Write the tests and acceptance fixtures for a milestone before coding any
   slice inside that milestone.
2. Do not rewrite a test to match an implementation. If the implementation
   disagrees with a test, fix the implementation or formally revise the
   contract with new evidence before coding continues.
3. Slices are implementation units, not test checkpoints. Finish the slices in
   a milestone, then run that milestone's complete test set together.
   M6 candidates may run in a bounded isolated worker pool; milestone gates
   remain serial and no candidate result is promoted until the full matrix is
   assembled and verified.
   work groups; no agent may concurrently edit a shared implementation file.
4. Keep the pre-milestone baseline immutable. Every candidate records the Git
   commit, binary hash, config, source metadata, output dimensions, GPU timing,
   and capture command.
5. Diagnostic paths may be slower or noisier than production paths. They must
   be explicit and must not silently become the default playback path.
6. No new reconstruction-quality experiment begins until the contract gate
   that makes the experiment interpretable has passed.
7. A failed gate stops promotion of that milestone. The next slice is not
   allowed to hide the failure with tone, sharpening, filtering, or labels.

## Dependency map

```text
M0 trustworthy baseline and observability
  -> M1 postpass parameter contract
  -> M2 shared reprojection and FP16 state
  -> M3 causal codec motion
  -> M4 color / transfer / chroma / bit depth
  -> M5 decoded-video jitter policy
  -> M6 corrected quality campaign
  -> M7 performance and default promotion
```

M1 and M2 are the critical semantic path. M3 and M4 provide the input
correctness needed to judge temporal reconstruction. M5 is deliberately last
among the correctness packages because jitter is not interpretable while the
input, history, and postpass contracts are still wrong.

## Milestone 0 — trustworthy baseline and observability

### Outcome

Create an immutable pre-parity reference and prove that every important stage
can be inspected without changing the production result.

### Tests to write first

- Baseline manifest test: every candidate records commit, binary hash, config,
  settings, dimensions, input metadata, output path, and timing path.
- Output-dimension test: source, model, reconstruction, history, and display
  dimensions are reported independently; no 1278x720 relabeling is accepted.
- Stage-dump contract test: each requested diagnostic stage either produces a
  finite artifact with declared dimensions/format or fails loudly.
- No-hidden-settings test: benchmark launches cannot inherit image-affecting
  user settings without recording them.
- Reproducibility test: rerunning the unchanged baseline produces compatible
  metadata and does not overwrite the prior artifact.

### Implementation slices

0.1 Freeze the current baseline manifest, binary hash, settings, and corpus
    selection.

0.2 Define the stage artifact schema for D0–D8 from the master plan.

0.3 Add capture metadata and immutable artifact naming to the lab tooling.

0.4 Add finite-value, dimension, format, and provenance validators.

0.5 Record the current quality/performance numbers as controls without
    changing the player path.

### Milestone gate

The baseline can be reproduced and audited from saved artifacts. A missing
stage, ambiguous dimension, inherited setting, or overwritten result fails M0.

## Milestone 1 — FSR4 postpass parameter contract

### Outcome

Make the recovered 4.1 FP32 postpass parameter region typed, addressable,
traceable, and testable before judging its visual effect.

### Tests to write first

- Weight-region bounds test: offset `130088`, 222 FP32 values, and zero-padding
  boundary are validated against the known blob layout.
- Parameter decode fixture test: known bytes decode to expected named values;
  endianness, alignment, and finite-value rules are explicit.
- Shader binding/consumption test: every declared postpass parameter used by the
  contract has a matching host-side binding and an observable shader use.
- Parameter trace test: diagnostic output identifies each active parameter load
  and its destination contribution.
- Reset determinism test: identical input/history/reset state produces identical
  decoded postpass parameters and finite output.

### Implementation slices

1.1 Replace the legacy scale-zone naming with a typed 4.1 postpass-region
    accessor while preserving compatibility aliases only where required.

1.2 Build the host-side parameter decode and validation layer.

1.3 Define the shader-side parameter access map from the recovered evidence.

1.4 Wire decoded parameters into the diagnostic postpass path first.

1.5 Add parameter and contribution dumps without changing the production
    composition policy yet.

### Milestone gate

Every recovered active parameter is accounted for, decoded values are finite and
stable, and the diagnostic trace proves which values affect composition. “The
image looks better” is not sufficient to pass M1.

## Milestone 2 — one causal reprojection and FP16 temporal state

### Outcome

Use one exact reprojected color sample for network conditioning and final
composition, with model-state precision preserved between frames.

### Tests to write first

- Shared-resource identity test: prepass and postpass consume the same
  reprojected-color resource for one frame.
- Coordinate-consistency test: history color and recurrent state use the same
  historical coordinate, including borders and invalid/disoccluded samples.
- Causal-history test: frame N cannot read frame N+1 or an uncommitted write
  image.
- FP16 format test: history, recurrent, and reprojected-color images use the
  declared FP16 formats, dimensions, usage flags, and ping-pong roles.
- Range-preservation test: values above 1 and negative model-space values are
  not silently clamped by the state resource.
- Reset/ping-pong test: seek, scene cut, first frame, and teardown reset both
  history and recurrent state deterministically.

### Implementation slices

2.1 Add an explicit target-grid reprojected-color resource to the dispatch
    resources and frame input contract.

2.2 Move history/recurrent allocation to FP16 model-state formats while keeping
    display output separate.

2.3 Make prepass publish the shared reprojected color and matching coordinate
    validity information.

2.4 Remove the independent postpass history reprojection from the parity path.

2.5 Update barriers, descriptor bindings, readback diagnostics, and reset/
    teardown ownership.

2.6 Keep a clearly labeled legacy diagnostic path only if it is needed for
    comparison; it must not be silently selected as the corrected path.

### Milestone gate

The resource graph, shader trace, and reset tests prove one causal reprojection
and FP16 model-state lifetime. Only after this gate may learned-only or learned
blend quality be retested.

## Milestone 3 — causal codec motion

### Outcome

Ensure motion used for previous-frame history is directionally causal and has a
defined confidence/repair policy.

### Tests to write first

- Motion-sign fixture: past, current, and future codec-reference vectors retain
  their direction through decode, upload, and shader expansion.
- Future-vector exclusion test: future-reference vectors cannot enter the
  previous-frame history field.
- Scale/sign test: codec motion maps to source/model/target coordinates with the
  documented sign and scale.
- Overlap policy test: overlapping blocks resolve deterministically.
- Adjacent-validation fixture: a vector that does not explain the adjacent
  displayed frame is rejected or downgraded according to the contract.
- Confidence propagation test: confidence reaches reactive/disocclusion logic
  and cannot silently become a global always-confident value.
- Scene-cut test: reset wins over stale motion and clears temporal state.

### Implementation slices

3.1 Preserve reference direction and any required source-frame identity in the
    GPU motion-vector structure.

3.2 Define deterministic sign, scale, overlap, sparse-block, and border rules.

3.3 Add cheap adjacent-frame validation and low-confidence repair.

3.4 Propagate confidence into reactive/disocclusion and scene-reset decisions.

3.5 Add synthetic translation, occlusion, and scene-cut fixtures.

### Milestone gate

Motion semantics pass independently before any subjective “less ghosting” claim
is accepted. Future references cannot contaminate previous-frame history.

## Milestone 4 — color, transfer, chroma, and bit depth

### Outcome

Make decoded pixels enter the model in a documented, metadata-driven color
domain without using exposure or contrast as a correction for a transfer bug.

### Tests to write first

- Metadata propagation test: range, matrix/colorspace, transfer, chroma
  location, bit depth, and primaries survive decode-to-upload conversion.
- SDR limited-range BT.709 fixture test.
- SDR full-range BT.709 fixture test.
- HDR/PQ or HLG high-bit-depth fixture test, where available.
- Chroma-siting edge test using one-pixel colored diagonals and text edges.
- 10/12-bit precision test: values survive conversion without an unintended
  8-bit round trip or clipping.
- Model/display-domain separation test: display encoding does not mutate model
  history or recurrent state.

### Implementation slices

4.1 Extend `DecodedVideoFrame` metadata for chroma location, primaries, and
    source bit depth where FFmpeg exposes them.

4.2 Define the model-color transfer/range contract and conversion constants.

4.3 Implement metadata-aware software and DRM/GPU conversion paths.

4.4 Implement explicit chroma sample phase handling.

4.5 Add high-bit-depth resources and readback diagnostics.

4.6 Remove or quarantine hidden source sharpening from the parity path.

### Milestone gate

All required metadata tests pass for the supported sample classes. No permanent
tone/exposure correction is promoted until this gate passes.

## Milestone 5 — decoded-video jitter policy

### Outcome

Determine whether the trained temporal model benefits from current jitter when
the input is already decoded video, without assuming game-render jitter applies.

### Tests to write first

- Jitter mode contract test: off, current, reduced, and controlled modes record
  exact phase/amplitude and affect only the intended stage.
- Static-region flicker test.
- Edge-position variance test.
- Motion-compensated residual test.
- Occlusion/disocclusion ghost-duration test.
- Reset/phase test: seek and scene cut restart the documented jitter sequence.

### Implementation slices

5.1 Add explicit jitter policy/configuration metadata to the capture manifest.

5.2 Implement the no-jitter diagnostic mode.

5.3 Implement controlled/reduced amplitude modes without changing history rules.

5.4 Capture matched sequences across relevant content classes.

### Milestone gate

Jitter is selected from sequence evidence, not a still-frame score. A mode that
improves one still but increases static shimmer or edge variance fails promotion.

## Milestone 6 — corrected learned reconstruction campaign

### Outcome

Retest learned-only, learned blend, residual, and post-reconstruction options
after the reconstruction contract is corrected.

### Tests to write first

- Candidate-manifest schema test with exact config and binary provenance.
- Per-class paired-metric test: natural daylight, dark scenes, faces/hair,
  foliage, debris, line art, text/UI, geometry, gradients, pans, occlusions,
  and scene cuts.
- Temporal metric test: static flicker, motion-compensated error, edge variance,
  ghost persistence, and reset recovery.
- Non-regression test against the immutable pre-parity controls.
- Review-harness asset test: every finalist maps to the correct source,
  resolution, technique, and modifier metadata.

### Implementation slices

6.1 Freeze the campaign schema and runner bridge.

6.2 Add strict paired spatial metrics and verification.

6.3a Validate causal temporal sidecars, including frame ordering and motion
    availability.

6.3b Align candidate/reference sequences and ingest temporal metrics without
    identity-motion or event-metric guesses.

6.4a Discover the complete real-world review asset pool and prove left/right
    parity.

6.4b Resolve dependent selectors and stackable modifiers from metadata.

6.4c Validate the one-divider viewer, real-time updates, fit, 1:1, zoom, pan,
    and highest-resolution canvas behavior.

6.4d Package the standalone folder and self-contained HTML, with streamed
    payloads, integrity checks, and an explicit size/load gate.

6.5 Assemble and verify the complete candidate × scene × class × resolution
    matrix before any learned-quality comparison is promoted.

Only after those contracts are closed may the campaign capture slices begin:
baseline and controls, learned candidates, residual/postpass candidates, the
small tone screen, and finalist preservation. Each capture slice remains
bounded-parallel and the complete M6 gate runs only after the set is complete.

### Milestone gate

A candidate must beat the best spatial control on at least one meaningful class,
remain acceptable on difficult classes, avoid temporal regressions, and preserve
the full artifact trail. If no learned candidate wins, the core-unlock gate is
opened for a separate investigation; the result is not hidden by presentation
tuning.

## Milestone 7 — performance and default promotion

### Outcome

Optimize only the verified winner and promote a production default with a
rollback path.

### Tests to write first

- GPU-time budget test separating reconstruction, upload/decode, and presentation.
- Frame-time percentile test at the target source/output resolutions.
- Resource-memory and synchronization regression test.
- Quality-equivalence test between clear reference implementation and optimized
  implementation.
- Rollback-config test proving the prior known-good control can be restored.
- Production-default test proving diagnostic Quality Lab settings are disabled
  unless explicitly requested.

### Implementation slices

7.1 Measure the corrected clear path and identify real bottlenecks.

7.2 Remove redundant history sampling only after semantic equivalence is tested.

7.3 Fuse or specialize postpass work only behind the equivalence tests.

7.4 Re-run the full quality and performance gate on the optimized path.

7.5 Promote the default and record rollback configuration, final binary hash,
    artifacts, and known limitations.

### Milestone gate

The optimized path is semantically equivalent to the verified reference,
within the performance budget, and reversible. Only this gate permits declaring
the campaign complete.

## Milestone test ownership

Tests are authored in the test/fixture phase before the implementation slices in
that milestone. The expected ownership is:

- M0: `benchmarks/quality_sweeps/`, capture metadata validators, and immutable
  manifest fixtures.
- M1: `tests/fsr4_weight_tests.cpp`, new postpass parameter fixtures, and shader
  parameter trace validation.
- M2: `tests/fsr4_harness_tests.cpp`, uploader resource tests, and reset fixtures.
- M3: motion fixtures in `tests/sidebuffer_tests.cpp` plus decoder/uploader
  contract tests.
- M4: decoder/conversion fixtures and new color metadata tests.
- M5: temporal sequence runner and metric fixtures.
- M6: quality-sweep manifests, paired metric validators, and review-harness
  manifest checks.
- M7: performance runner, equivalence fixtures, and rollback checks.

The test files may gain new cases, but their acceptance contract must be written
and reviewed before implementation begins. A failing test is evidence about the
implementation, not permission to weaken the assertion.

## Stop conditions

Stop and document the evidence when:

- the current hardware cannot execute a required GPU gate;
- a source/reference artifact is missing or cannot be matched causally;
- a recovered 4.1 behavior remains ambiguous after the available evidence is
  exhausted;
- a quality improvement requires changing weights/topology before the core-
  unlock gate; or
- performance cannot preserve the stated real-time budget.

In each case, keep the failing fixture, logs, binary/config provenance, and the
decision. Do not silently substitute a synthetic result, relabel a fallback, or
continue to the next milestone as if the gate passed.
