# Temporal Forge motion/history semantic audit

Audit date: 2026-08-24  
Scope: codec motion extraction and expansion, `historyConfidence`, reprojection,
and the existing motion contract/tests/docs.  This is a read-only source and
artifact inspection. No tests, player runs, or GPU captures were run.

## Finding

The highest-confidence discrepancy to test next is **disocclusion / sparse
motion validity**:

> An uncovered destination pixel is represented as zero motion, then consumed
> as an ordinary valid on-screen history sample. `historyConfidence` is only a
> frame-level scalar; it does not supply per-pixel validity, does not affect the
> reactive signal, and does not gate the reprojection itself.

This is a proven semantic gap in the current contracts and implementation. It
is not yet proof that a visible ghost occurs on a particular clip.

## Proven facts

### Extraction, filtering, and scale

- `MvEntry` documents destination block coordinates and source-pixel motion;
  `source < 0` means a backward reference and `source > 0` a future reference:
  `src/media/VideoDecoder.hpp:27-38`.
- FFmpeg motion vectors are normalized by `motion_scale` and their reference
  direction is retained in `source`: `src/media/VideoDecoder.cpp:330-351`.
- The causal filter rejects future references, non-finite vectors, and vectors
  beyond its displacement bound before confidence or upload:
  `src/core/PlaybackEngine.cpp:124-145`.
- Source-to-model coordinate scaling changes destination, motion, and block
  extent consistently: `src/core/PlaybackEngine.cpp:282-306`; the scaled vector
  list is what reaches `uploadMotion`: `src/core/PlaybackEngine.cpp:1772-1775`.

These facts make direction/reference loss and the basic source-to-model scale
less useful as the *next* audit target. The existing tests are still mostly
source-contract tests, not translated-block behavior tests.

### Expansion creates no validity channel

- The GPU owner buffer is cleared to zero before stamping blocks:
  `src/render/GpuImageUploader.cpp:1640-1656`.
- GPU resolve maps owner zero to `vec2(0.0)` and stores it as an ordinary
  `rg16f` motion value: `shaders/fsr4/codec_motion_expand.comp:50-57`.
- The CPU fallback also clears the complete motion image to zero and only
  overwrites covered block pixels: `src/render/GpuImageUploader.cpp:1758-1774`
  and `src/render/GpuImageUploader.cpp:1798-1810`.
- The prepass samples the motion image, adds it to the current target pixel,
  and samples history whenever the resulting coordinate is on screen. There
  is no coverage/validity/confidence texture in this decision:
  `shaders/fsr4/prepass_pq_eotf.comp:37-43` and `:210-231`.
- The public Python sidecar does the same semantic expansion: it initializes
  every target pixel to `(0, 0)` and fills only scaled block rectangles:
  `benchmarks/quality_sweeps/motion_sidecar.py:92-124`.
- The temporal metric treats every finite dense motion value as a usable
  correspondence; only out-of-frame coordinates and an explicit caller mask
  are excluded: `benchmarks/quality_sweeps/temporal_metrics.py:265-301` and
  `:304-379`.

Therefore sparse coverage is not distinguishable from a valid static region at
the reprojection boundary or in metric input.

### `historyConfidence` propagation is global and incomplete

- Codec confidence is computed from block coverage and motion statistics, then
  multiplied by lookahead confidence before `SideBufferSynth::update`:
  `src/core/PlaybackEngine.cpp:1672-1688`.
- The resulting scalar is assigned to the dispatch input:
  `src/core/PlaybackEngine.cpp:2011-2020`.
- The harness uses it to derive `effectiveConfidence` and writes the result to
  postpass `slot1.w`: `src/render/Fsr4DispatchHarness.cpp:3163-3187`.
- The normal postpass consumes `slot1.w` as the learned blend, but this is a
  composition weight, not a reprojection-validity test:
  `shaders/fsr4/postpass_composite.comp:637-649`.
- The prepass reprojection uses a fixed current/history resolve weight and the
  sampled motion coordinate; it never reads `historyConfidence` or a coverage
  value: `shaders/fsr4/prepass_pq_eotf.comp:198-231`.
- `SideBufferSynth::update` stores the supplied confidence and uses it for the
  scene-cut threshold, but its reactive uncertainty is still derived from
  `histogramDelta`, not `motionConfidence`:
  `src/render/SideBufferSynth.cpp:116-141`.

The current path therefore has a scalar reset/composition signal, not local
confidence propagation into reactive or disocclusion behavior.

### Existing contract evidence and its boundary

- `tests/fsr4_motion_contract_tests.cpp:33-60` checks direction filtering,
  scaling tokens, deterministic GPU ownership, and scalar handoff tokens. It
  does not test sparse coverage, a validity mask, or a translated block.
- `tests/sidebuffer_tests.cpp:30-42` checks the scalar reset threshold and
  `:19-28` checks the formula in isolation; no test changes `motionConfidence`
  while holding luma motion constant and observes reactive output.
- `tests/test_motion_sidecar.py:52-65` verifies source-to-target expansion and
  `:83-96` verifies future-vector rejection, but no test requires explicit
  validity for uncovered pixels.
- The M3 plan explicitly requires sparse coverage, adjacent validation,
  confidence-to-reactive/disocclusion propagation, and synthetic occlusion:
  `docs/slice-plan.md:273-280` and `:294-327`.
- The test strategy marks “confidence reaches reactive/disocclusion logic” as a
  must-have integration behavior: `docs/reference/testing/test-strategy.md:92-102`.
- The M3 gate records the current boundary honestly: it says the tests do not
  replace translated-block, occlusion, or adjacent-frame validation:
  `docs/archive/plans/QUALITY_PERFECTION_M3_GATE.md:29-33`.

## Hypotheses to validate next

1. A block-sparse transition with an exposed background will cause the
   uncovered pixels to reuse previous-frame history at zero motion rather than
   be treated as disoccluded. This is expected from the code path, but no
   rendered or measured artifact was collected here.
2. A moderate scalar confidence reduction above the reset threshold will reduce
   learned composition strength while leaving the prepass history sample and
   reactive uncertainty unchanged. This follows from the separate host/shader
   paths and should be confirmed by a CPU-level test first.
3. If the quality-lab experimental composition is enabled, its use of
   `slot5.x` instead of the normal `slot1.w` learned blend may bypass the scalar
   confidence gate. This is configuration-specific and should not be treated
   as a default-path regression without a targeted test.

## Tests-first plan

Keep this sequence CPU/static first; do not start with a GPU capture.

1. **Red test for sparse validity — `tests/test_motion_sidecar.py`.** Add a
   4x2 or 8x4 fixture with one causal block covering only part of the frame and
   an exposed region. Define the contract before implementation: uncovered
   target pixels must be explicitly invalid/low-confidence (or the sidecar
   must be rejected as incomplete); they must not silently become valid
   `(0, 0)` motion. Assert that the current dense-field API fails this new
   requirement. Preserve the existing sign/scale and future-reference tests.

2. **Red test for scalar confidence handoff — `tests/sidebuffer_tests.cpp`.**
   Feed identical consecutive luma frames and identical timing to two
   synthesizers, varying only `motionConfidence` from `1.0` to a value above
   the reset threshold (for example `0.20`). Assert the documented contract:
   the confidence-dependent reactive/disocclusion signal changes while the
   frame is not reset. The current implementation should fail because
   `update()` derives reactive uncertainty from histogram delta alone. This is
   a deterministic unit test and needs no video or GPU.

3. **Boundary guard for actual reprojection —
   `tests/fsr4_motion_contract_tests.cpp`.** Extend the CPU-only contract to
   require an explicit validity/coverage input at the expansion-to-prepass
   boundary and a prepass branch that prevents invalid pixels from sampling
   history. A token-only check is acceptable as a guard, but the intended
   outcome is an executable small-grid model test matching owner expansion,
   border handling, and the `destination + motion` convention. The current
   `owner == 0 -> vec2(0)` plus on-screen history path should fail this guard.

4. **Metric protection — `tests/test_temporal_sequence.py` and
   `tests/test_motion_sidecar.py`.** Add a fixture proving that an invalid
   disoccluded pixel is excluded only when its explicit validity/static mask is
   supplied; no implicit identity-motion repair is allowed. Keep the metric's
   current causal indexing and `x + dx, y + dy` convention unchanged.

5. **Only after the CPU contracts pass**, run an artifact-level translated
   block/occlusion sequence through the existing sidecar/export path. Record
   sparse coverage, confidence, reset, and masked/unmasked residuals together.
   A live GPU result would be a later gate, not evidence needed to establish
   this first semantic contract.

## What this audit does not claim

- It does not claim the current motion sign, reference filtering, or scale is
  wrong; the inspected code is internally consistent on those points.
- It does not claim a visible ghost, a quality regression, or a performance
  regression; no sequence was run.
- It does not claim the existing M3 gate was invalid. Its own documentation
  limits the gate to boundary/static evidence and lists the missing translated
  block and occlusion validation.
- No source, shader, default, test, or existing artifact was modified by this
  audit. The only new file is this sidecar.
