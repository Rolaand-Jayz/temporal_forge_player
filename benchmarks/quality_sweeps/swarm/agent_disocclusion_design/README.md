# Sparse motion and disocclusion validation design

This package is a design-only handoff based on
`agent_motion_audit/motion-history-audit.md`. It intentionally contains no
source, shader, or test edits, and no GPU-capture results.

## Goal

Prove the smallest missing semantic contract before trying a quality change:

1. A target pixel not covered by any accepted motion block is not silently
   treated as a valid zero-motion correspondence.
2. Confidence below full confidence can reach local reactive/disocclusion
   handling without being confused with a scene reset.
3. Temporal metrics only mask a pixel when an explicit validity mask says it is
   invalid; they do not infer validity from `(0, 0)` motion.

This is validation work, not an instruction to add a default quality heuristic.
No new environment variable or runtime knob is proposed here. Existing knobs
such as `TFORGE_FSR4_DISABLE_LEARNED_CONFIDENCE_GATE` and
`TFORGE_FSR4_LEARNED_CONFIDENCE_BLEND` are composition controls; they are not
motion-validity controls and must not be repurposed for this work.

## Smallest tests-first sequence

### 1. Sparse expansion red test

Later change:

- `benchmarks/quality_sweeps/motion_sidecar.py`
  - the dense expansion routine around lines 92–124, where the target field is
    initialized to zero and scaled block rectangles are written;
- `tests/test_motion_sidecar.py`
  - the existing expansion tests around lines 52–65.

Write the test before changing the sidecar API. Use a tiny 8x4 target and one
accepted causal block that covers only part of it. Hold the block's motion at
zero so the uncovered pixels are indistinguishable from static covered pixels
in the current representation.

Expected red assertions:

- the expansion result must expose a separate coverage/validity value for each
  target pixel, or reject the incomplete expansion explicitly;
- every covered pixel is valid, including a covered zero-motion pixel;
- every uncovered pixel is invalid/low-confidence;
- no test may pass merely because an uncovered pixel contains `(0, 0)`;
- existing direction, scale, and future-reference rejection assertions remain
  unchanged.

The first implementation may choose the least disruptive representation, but
the test must describe behavior rather than dictate a field name. Do not infer
coverage from vector magnitude: static zero motion is valid motion.

### 2. Confidence-to-local-signal red test

Later change:

- `src/core/SideBufferSynth.cpp`, `SideBufferSynth::update` around lines
  116–141;
- `tests/sidebuffer_tests.cpp`, alongside the existing scalar reset and
  formula tests around lines 19–42.

Create two otherwise identical updates with identical luma/timing and only
`motionConfidence` different: `1.0` versus a moderate value above the existing
scene-reset threshold, such as `0.20`.

Expected red assertions:

- neither case is a scene reset;
- the confidence-dependent reactive/disocclusion signal differs;
- the difference is attributable to `motionConfidence`, not histogram delta;
- identical confidence and identical inputs remain deterministic.

The exact output member should be taken from the existing `SideBufferSynth`
contract when the test is written. Do not invent a new public knob or assert a
specific algorithmic formula in this design package. The required behavior is
that confidence is no longer only a frame-level learned-composition/reset
input.

### 3. Reprojection-boundary red guard

Later change:

- `shaders/fsr4/codec_motion_expand.comp`, around the owner-zero handling at
  lines 50–57;
- `shaders/fsr4/prepass_pq_eotf.comp`, around motion/history sampling at lines
  198–231;
- `tests/fsr4_motion_contract_tests.cpp`, alongside the existing CPU/static
  motion contract checks at lines 33–60.

Add a CPU-only small-grid model matching the existing convention:
`destination + motion`, border checks, owner expansion, and history sampling.
The test should use the same 8x4 sparse fixture from step 1.

Expected red assertions:

- an invalid/uncovered destination cannot sample history merely because its
  zero motion maps it on screen;
- a covered static pixel can still sample history;
- an out-of-frame reprojected coordinate remains invalid;
- the model requires an explicit validity/coverage input at the expansion to
  prepass boundary.

A static token check may accompany the model test to prevent accidental removal
of the shader branch, but tokens alone are not sufficient acceptance evidence.

### 4. Metric mask red test

Later change:

- `benchmarks/quality_sweeps/temporal_metrics.py`, around the finite-motion
  and caller-mask handling at lines 265–301 and 304–379;
- `tests/test_motion_sidecar.py` and, if present in this checkout, the
  sequence-test module covering `temporal_metrics.py`.

Use one invalid disoccluded pixel whose motion is `(0, 0)` and one valid static
pixel also having `(0, 0)`. Supply an explicit validity mask.

Expected red assertions:

- the invalid pixel is excluded only when the explicit mask is supplied;
- the valid static pixel remains included;
- without an explicit mask, the metric does not silently repair or classify
  the invalid pixel from its zero vector;
- causal indexing and the existing `x + dx, y + dy` convention do not change.

This protects measurement from hiding the exact artifact the runtime contract
is meant to address.

## Later implementation touchpoints

The eventual implementation should be limited to the already identified
boundaries:

- `src/render/GpuImageUploader.cpp`: preserve or upload coverage while the
  owner image is cleared and block rectangles are stamped, around lines
  1640–1656 and 1758–1810;
- `shaders/fsr4/codec_motion_expand.comp`: distinguish owner-zero/uncovered
  from a covered zero-motion block;
- `shaders/fsr4/prepass_pq_eotf.comp`: gate history/reprojection with that
  validity, without changing motion sign, scale, or coordinate convention;
- `src/core/SideBufferSynth.cpp`: pass existing `motionConfidence` into the
  already-defined reactive/disocclusion decision path, while preserving the
  existing scene-reset threshold behavior;
- `benchmarks/quality_sweeps/motion_sidecar.py`: emit the same validity
  semantics used by the runtime path;
- `benchmarks/quality_sweeps/temporal_metrics.py`: consume an explicit mask
  when available and preserve current behavior when callers intentionally do
  not provide one.

The exact member/binding names, descriptor updates, and synchronization details
must be discovered from the current files when implementation begins. This
design does not invent them.

## Evidence required before an opt-in experiment

Passing CPU/static tests only proves the contract. It does not justify a
quality claim. An opt-in experiment would require all of the following:

1. The four red tests above are written first, fail against the current code,
   then pass after the narrowly scoped implementation.
2. A deterministic artifact-level sparse translated-block sequence records:
   coverage, invalid-pixel count, confidence, reset state, and masked versus
   unmasked temporal residuals.
3. A matched baseline/candidate run uses the same binary inputs, frame range,
   warmup, resolution, corpus, and metrics. No comparison against a changed
   capture set is valid.
4. Human inspection confirms that an exposed background does not inherit stale
   foreground history, while covered static detail is not unnecessarily reset.
5. Temporal stability is reported per scene, not only as an aggregate. A small
   aggregate gain is insufficient if cave/debris-style motion scenes regress.
6. Performance is measured separately; no opt-in is promoted if validity work
   adds an unexplained dispatch or material frame-time cost.

Only after those gates pass should a narrowly named, opt-in runtime experiment
be introduced. The knob name and default must be chosen from the implemented
interface at that time; this package intentionally does not propose one.

## Explicit non-goals

- No GPU captures or player runs.
- No change to motion sign, reference filtering, scaling, jitter, FSR
  composition, sharpening, tone mapping, or reconstruction behavior.
- No reuse of the existing learned-confidence blend variables as a proxy for
  per-pixel motion validity.
- No claim that a visible ghosting regression has already been demonstrated.
