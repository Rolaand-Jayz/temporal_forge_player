# Temporal Forge FSR4 quality campaign — test strategy

## Metadata

- **Project / feature:** Temporal Forge reconstruction-quality perfection plan
- **Source:** the archived quality-perfection plan and milestone evidence in `docs/archive/plans/`
- **Scope:** M0 baseline trust through M7 default promotion, including the
  distributable review harness
- **Execution rule:** tests for a milestone are authored and reviewed before
  that milestone's implementation slices; the complete suite runs after the
  slices, not after every slice
- **Test purpose:** prove behavior and evidence integrity, not produce a
  reassuring number

## Test-first milestone protocol

For each milestone:

1. Add the test cases, fixtures, schemas, and expected failure reasons before
   implementation changes.
2. Run the new pack against the current baseline once and save the failures.
3. Freeze the assertions. Implementation slices may change code, but not the
   test merely to make the result pass.
4. Keep slices buildable with compile/syntax checks as needed; defer the formal
   milestone acceptance run until all slices in that milestone are present.
5. Run the full milestone pack, the relevant existing regressions, and the
   required real/live evidence check together.

If the contract itself must change, the change requires new source evidence,
an explicit plan revision, and a new failing-first test. “The image looks
better” is never sufficient reason to weaken an assertion.

## Risk classification summary

| Risk level | Components | Behavior target |
|---|---:|---|
| High | 5: provenance, postpass, temporal state, motion/color, promotion | Every acceptance behavior covered; live/integration evidence required for GPU and real-media claims. |
| Medium | 3: campaign metrics, jitter policy, review-harness data model | At least 70% of defined behaviors, with all critical paths covered. |
| Low | 1: visual styling and generated-page cosmetics | Test only functional behavior; skip arbitrary pixel/style coverage. |

These are behavior targets, not line-coverage promises. Generated shader code,
third-party codecs, and Qt/Vulkan internals are not counted as project behavior.

## Test plan by component

### Component: baseline provenance and benchmark trust

**Risk level:** high

#### Testable behaviors

| ID | Behavior | Test level | Mock? | Priority |
|---|---|---|---|---|
| B0-1 | A run records commit, binary hash, settings, dimensions, source metadata, decode path, timing, and artifact paths. | integration | no; use a real manifest and runner fixture | must |
| B0-2 | Source/model/reconstruction/display dimensions remain distinct. | unit + integration | no | must |
| B0-3 | A requested stage is finite and dimensioned, or the run fails with a reason. | integration | no for artifact validator | must |
| B0-4 | User image-affecting settings cannot silently enter a parity run. | integration | mock only the user config directory in a temp fixture | must |
| B0-5 | A rerun does not overwrite historical evidence. | integration | no | must |

#### Critical test cases

| ID | Proves | Level |
|---|---|---|
| TC0-1 | An isolated benchmark produces a complete provenance record with source sharpness explicitly zero. | integration |
| TC0-2 | Wrong output dimensions and missing binary/config fields are rejected. | unit |
| TC0-3 | A second run receives a distinct artifact identity. | integration |

### Component: FSR4 postpass parameter contract

**Risk level:** high

| ID | Behavior | Test level | Mock? | Priority |
|---|---|---|---|---|
| B1-1 | Offset 130088 contains exactly 222 finite FP32 values and known padding. | unit | no | must |
| B1-2 | Known initializer bytes decode consistently across alignment/endian paths. | unit | no | must |
| B1-3 | Every active recovered postpass load has a host binding and named contribution. | integration/static contract | no; inspect the real shader and host code | must |
| B1-4 | Parameter traces are deterministic across reset-equivalent runs. | integration | no | must |
| B1-5 | Non-finite/short/overrun parameter data is rejected rather than defaulted. | unit | no | must |

### Component: shared reprojection and temporal state

**Risk level:** high

| ID | Behavior | Test level | Mock? | Priority |
|---|---|---|---|---|
| B2-1 | Prepass and postpass use the same target-grid reprojected-color resource. | integration/live GPU | no; use the actual descriptor graph | must |
| B2-2 | History and recurrent state use one historical coordinate. | unit + live GPU | synthetic coordinate fixtures are allowed; no fake quality result | must |
| B2-3 | History state is RGBA16F, ping-ponged correctly, and is not silently clamped. | integration/live GPU | no | must |
| B2-4 | Frame N cannot read a future frame or an uncommitted write image. | unit + live GPU | no | must |
| B2-5 | First frame, seek, cut, and teardown reset all temporal state deterministically. | integration | no | must |

### Component: codec motion and causal temporal inputs

**Risk level:** high

| ID | Behavior | Test level | Mock? | Priority |
|---|---|---|---|---|
| B3-1 | Past/current/future reference direction survives decode-to-GPU expansion. | unit + integration | synthetic motion fixtures are appropriate | must |
| B3-2 | Future-reference vectors cannot enter previous-frame history. | unit | no | must |
| B3-3 | Sign, scale, overlap, sparse coverage, and borders are deterministic. | unit | no | must |
| B3-4 | Adjacent-frame validation rejects or downgrades unhelpful motion. | integration | use real adjacent frames; no guessed identity motion | must |
| B3-5 | Confidence reaches reactive/disocclusion logic and scene reset wins. | integration | no | must |

### Component: color, transfer, chroma, and bit depth

**Risk level:** high

| ID | Behavior | Test level | Mock? | Priority |
|---|---|---|---|---|
| B4-1 | Range, matrix, transfer, primaries, chroma location, and bit depth survive decode/upload. | integration | no | must |
| B4-2 | Limited/full SDR paths use the correct model transform. | integration | real encoded fixtures | must |
| B4-3 | Chroma phase follows metadata on diagonals/text edges. | unit + integration | controlled color fixtures | must |
| B4-4 | 10/12-bit paths preserve stored precision and do not use R8 as a hidden round trip. | integration | real high-bit-depth fixtures | must |
| B4-5 | Display encoding/tone does not mutate model history/recurrent state. | integration | no | must |
| B4-6 | Source sharpening is zero in parity captures and post-sharpen is a separate display concern. | integration | isolated config directory | must |

### Component: decoded-video jitter policy

**Risk level:** medium

| ID | Behavior | Test level | Mock? | Priority |
|---|---|---|---|---|
| B5-1 | Off/current/reduced/controlled modes record exact amplitude and phase. | unit | deterministic phase source | must |
| B5-2 | Jitter changes only the intended sampling stage. | integration | no | must |
| B5-3 | Static flicker and edge-position variance are measured causally. | integration | real sequence frames | must |
| B5-4 | Occlusion/scene-cut reset metrics remain aligned. | integration | real sequence + strict sidecar | must |

### Component: campaign metrics and evidence integrity

**Risk level:** medium/high

| ID | Behavior | Test level | Mock? | Priority |
|---|---|---|---|---|
| B6-1 | Candidates are ranked only on matched source/frame/output subsets. | unit + integration | fixtures for malformed manifests | must |
| B6-2 | Paired deltas include mean, median, and worst-case; missing rows fail. | unit | no | must |
| B6-3 | Temporal metrics require explicit causal motion and alignment sidecars. | unit + integration | no identity fallback | must |
| B6-4 | Failed captures cannot become a successful zero-row campaign. | integration | use a failing child-process fixture | must |
| B6-5 | Historical failed experiments remain addressable and are not overwritten. | integration | no | should |

### Component: distributable review harness

**Risk level:** medium

| ID | Behavior | Test level | Mock? | Priority |
|---|---|---|---|---|
| B6-6 | Both sides expose the same real-world asset pool. | unit/static + browser e2e | fixture manifest for parser tests; real generated build for e2e | must |
| B6-7 | Scene, input resolution, output resolution, technique, and applicable modifiers are dependent selectors. | browser e2e | no | must |
| B6-8 | One draggable divider reveals the selected left/right images in real time. | browser e2e | no | must |
| B6-9 | Fit, zoom, pan, and true 1:1 use the highest-resolution canvas; lower-resolution images are scaled visibly rather than silently resampled as evidence. | browser e2e | no | must |
| B6-10 | Hash/URL state restores exact left/right selections. | browser e2e | no | should |
| B6-11 | Synthetic families and invented/broken asset paths are absent. | unit/static | no | must |
| B6-12 | The standalone and single-file manifests contain the same asset names, and every alias resolves to one embedded lossless payload. | unit/static + artifact | no | must |
| B6-13 | A self-contained artifact above the declared size budget fails before replacing the destination. | integration/artifact | no | must |
| B6-14 | The candidate × scene × class spatial and temporal matrix has exact key parity, dimensions, metrics, and provenance. | unit + integration | no | must |
| B6-15 | A legacy M6 manifest and saved evidence produce a capture-free missing-key report without inventing quality classes or candidate attribution. | unit + integration | no | must |

### Component: performance and promotion

**Risk level:** high

| ID | Behavior | Test level | Mock? | Priority |
|---|---|---|---|---|
| B7-1 | GPU reconstruction, decode/upload, CPU, and presentation timings are separated. | integration/performance | no | must |
| B7-2 | Mean/p50/p95 remain within the declared budget at supported sizes. | performance | real target GPU | must |
| B7-3 | Optimized path matches the clear verified path within format-aware tolerances. | integration/live GPU | no | must |
| B7-4 | Diagnostic knobs are off in normal playback unless explicitly selected. | integration | isolated runtime config | must |
| B7-5 | Prior known-good configuration is restorable. | integration | no | must |

## Mock boundaries

| Boundary | Unit tests | Integration/gate tests |
|---|---|---|
| User config directory | Use a temporary isolated directory | Use a real isolated benchmark config, never the user's live settings |
| Time/randomness | Inject deterministic frame/phase sources | Use recorded frame indices and the actual runtime clock only when measuring performance |
| Motion vectors | Synthetic translation fixtures for sign/scale/overlap | Real decoder-exported sidecars for causal sequence claims |
| Image files | Tiny controlled fixtures for parser/math tests | Real benchmark/reference images for campaign claims |
| FFmpeg/Vulkan/Qt | Do not test third-party internals | Exercise the real boundary in media/GPU integration tests |
| Browser | Static DOM/manifest checks for fast contract feedback | Real Firefox/Chromium interaction for split, selectors, zoom, pan, and URL state |
| Hardware/GPU | No fake “GPU passed” substitute | If the target GPU gate cannot run, mark the milestone blocked and preserve logs |

The file-based review harness can be opened directly as
`review_harness/index.html`. Browser-level checks should use installed Firefox
Marionette when the environment permits a headless browser; restricted
environments must report `blocked` rather than substituting a mock result.

## What not to test

- Do not re-test FFmpeg, Vulkan, Qt, or browser behavior that belongs to the
  third party; test Temporal Forge's boundary contracts instead.
- Do not pursue arbitrary line coverage for generated shader headers, generated
  review HTML, or simple data classes.
- Do not use screenshot pixel equality as proof of GPU reconstruction parity;
  drivers, color management, and browser scaling make that brittle. Use stage
  artifacts, format-aware numeric comparisons, and interaction checks.
- Do not rank synthetic diagnostic images as human-facing quality evidence.
  Synthetic fixtures remain valid for isolated motion, chroma, geometry, and
  parser tests only; the distributable review corpus stays real-world only.
- Do not test a fallback as though it were the requested technique, and do not
  accept a test that passes when a capture produces no valid rows.
- Do not start weights/topology/quantization tests before the core-unlock gate.

## Implementation priority

### Phase 1 — evidence integrity

1. M0 provenance, hidden-settings isolation, immutable artifact naming.
2. M1 typed postpass region and trace contract.
3. M2 shared resource graph, FP16 state, causal reset.

### Phase 2 — input and temporal correctness

4. M3 causal codec motion and confidence.
5. M4 transfer/chroma/bit-depth correctness and source-sharpen isolation.
6. M5 matched jitter sequence evidence.

### Phase 3 — quality judgment and delivery

7. M6 campaign/metric verification and review-harness e2e.
8. M7 equivalence, performance, rollback, and default promotion.

## Milestone gate inventory

| Gate | Required evidence |
|---|---|
| M0 | Reconstructable baseline manifest, no hidden settings, finite/dimensioned stage records, no overwrite. |
| M1 | All recovered 4.1 postpass accesses typed, decoded, traced, and finite. |
| M2 | One shared reprojection, FP16 model state, causal resource lifetime, live reset proof. |
| M3 | Past-only motion, deterministic coordinate/overlap policy, adjacent validation, confidence/reset proof. |
| M4 | Separate SDR limited/full and HDR/high-bit-depth evidence, metadata-aware chroma, no hidden tone correction. |
| M5 | Matched sequence metrics and human review across static, edge, motion, occlusion, and cuts. |
| M6 | Complete real-world candidate matrix, strict temporal sidecars, paired rankings, working distributable harness, and real-browser interaction evidence. |
| M7 | Reference/optimized equivalence, percentile performance, fresh-session reproducibility, rollback and promotion record. |

## Success criteria

- All high-risk behaviors have an integration or live-gate proof.
- Every milestone's tests existed before its implementation slices.
- No test assertion was weakened merely to match code.
- No milestone is marked passed from static checks when its required live gate
  was unavailable.
- Historical artifacts and failed experiments remain preserved.
- The final default is promoted only after quality, temporal stability, color
  correctness, performance, reproducibility, and rollback all pass.
