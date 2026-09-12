# Temporal Forge — Lattice Corruption Diagnostic

## Objective

Find and prove the **first pipeline stage that introduces the repeating checkerboard/lattice corruption** in Temporal Forge's FSR4 path, then implement the smallest root-cause fix and verify it.

Do **not** start another full quality campaign, tune CAS, work on motion vectors/optical flow, or make broad architectural changes until this P0 defect is isolated.

---

## Evidence already established

### Comparatively good control
- Scene: `sintel_cave`, frame 48
- Input: `640x360`
- Delivery: `1280x720`
- Nominal FSR scale: `2.00`
- Runtime model grid: `640x360`
- Source/model dimensions match
- Internal source→model bicubic prefilter is bypassed

### Bad cases
- `640x360 -> 1920x1080`
  - model grid `960x540`
  - source/model dimensions differ
  - lattice corruption present
- `1280x720 -> 1920x1080`
  - model grid `960x540`
  - source/model dimensions differ
  - lattice corruption present
- 480p-input Temporal Forge routes also show the same class of corruption

### Controls
Conventional bilinear, bicubic, and Lanczos outputs do **not** show the lattice.

### Visual classification
The artifact is not ordinary motion ghosting. It is a repeating screen-space / feature-grid-like pattern across unrelated image regions. It can resemble foreign-scene leakage at normal size, but visual inspection shows a regular repeating lattice.

### CAS
- Pre-downsample CAS helps stair-step aliasing somewhat but oversharpens, especially at higher input resolutions.
- Post-downsample CAS looks better and oversharpens less.
- CAS is not a root-cause fix.
- Track stair-step aliasing separately as P1.

---

## Strongest current trigger

When:

```text
decoded source dimensions != FSR model-grid dimensions
```

`GpuImageUploader` invokes:

```cpp
dispatchBicubicPrefilter()
```

and the FSR path receives the model-resolution result.

When source and model dimensions are equal, that stage is bypassed.

This correlation is strong, but **do not assume the prefilter itself is guilty until the first corrupt intermediate is captured.**

Relevant current allocation semantics have already been checked:

```text
sourceModel_ = source-resolution RGB10 image
color_       = model-resolution RGB10 image
```

Despite confusing naming, the intended direction is sourceModel_ → color_.

A basic compute write→read barrier also appears to exist. Do not "fix" descriptor direction or synchronization speculatively.

---

## Primary diagnostic case

Use one deterministic bad case first:

```text
scene: sintel_cave
frame: 48
input: 1280x720
delivery: 1920x1080
nominal scale: 2.00
CAS: disabled
profile: AMD_SEMANTIC_BASELINE
```

Existing evidence:

```text
benchmarks/quality_sweeps/quality_campaign_capture/
resolution_720_to_1080/fsr_none/scale_2_00/sintel_cave/
```

Do not overwrite it.

Healthy control:

```text
sintel_cave
frame 48
640x360 -> 1280x720
scale 2.00
CAS disabled
AMD_SEMANTIC_BASELINE
```

---

## Required stage captures

For the **same exact frame/run**, capture:

### A — sourceModel_
Immediately after decoded YUV → RGB10/model-color conversion, before the source→model prefilter.

Expected bad-case dimensions:

```text
1280x720
```

### B — color_
Immediately after `dispatchBicubicPrefilter()`.

Expected bad-case dimensions:

```text
960x540
```

### C — prepass / first network input
Capture a human-viewable representation of the actual FSR prepass/current-color/feature data immediately before the first recovered neural encoder operation.

### D — first useful post-encoder tensor
If A/B/C are clean, capture immediately after the first meaningful neural stage, especially around the first stride/downscale encoder boundary.

### E — native FSR output
Capture before external presentation scaling, downsampling, or post-CAS.

For every diagnostic artifact record:

- stage
- scene/frame
- dimensions
- run/experiment ID
- Git HEAD and dirty state
- player binary SHA256
- artifact SHA256
- model/source/output geometry
- jitter/history/recurrent state

---

## Reference-resize ablation

Add one controlled diagnostic path that replaces **only** the internal source→model prefilter with a known-good resize.

Acceptable diagnostic reference:
- libswscale/FFmpeg bicubic, or
- another already-trusted resize implementation

Keep everything downstream identical:

- same source frame
- same `960x540` model grid
- same FSR4 path
- same jitter
- same history/recurrent state
- same Mu-law/prepass behavior
- CAS disabled
- same output target

This is an **ablation**, not the production fix.

Interpretation:

```text
GPU prefilter bad + reference resize clean
=> isolate GPU prefilter/source→model handoff

both bad
=> source/model mismatch exposes a downstream FSR semantic/layout defect
```

---

## Decision tree

```text
A corrupt
→ YUV conversion / source upload / sourceModel_ integrity

A clean, B corrupt
→ dispatchBicubicPrefilter() is first bad boundary

A/B clean, C corrupt
→ prepass / source-grid handoff / jitter / Mu-law / descriptor/indexing issue

A/B/C clean, D corrupt
→ neural encoder / stride / tensor layout / padding issue

all clean until later
→ walk the graph forward until first corruption
```

Do not declare root cause before locating the first bad stage.

---

## Audit at the first bad boundary

Check, with evidence:

- VkImage extent vs logical dimensions
- format and packing
- descriptor bindings
- push constants
- workgroup dispatch extents
- tail/edge handling
- pixel-center and phase mapping
- source/model coordinate transforms
- row/tensor strides
- packed feature layout
- padding lanes
- partial writes
- out-of-range reads/writes
- resource initialization
- barriers/fences/queue ownership where relevant

If partial/uninitialized writes are plausible, add a **diagnostic sentinel clear** before dispatch. Use a conspicuous deterministic value and determine whether it survives into the corrupted output.

Do not infer uninitialized memory solely from appearance.

---

## Automation

Build a small repeatable diagnostic runner under a new directory, for example:

```text
benchmarks/quality_sweeps/lattice_corruption_diagnostic/
```

It must:

- run only the bad case and healthy control
- emit stage captures
- hash every artifact
- record requested/effective runtime state
- fail on profile/binary/geometry mismatch
- never overwrite the completed campaign
- keep diagnostic outputs clearly separate from validated campaign evidence

Parallelize only independent investigation tasks. Serialize GPU execution and overlapping code edits.

---

## Priority split

```text
P0: repeating lattice/checkerboard corruption
P1: stair-step aliasing
```

Do not use CAS to mask either root cause.

---

## Definition of Done

The investigation is complete only when:

1. The bad case is deterministically reproduced.
2. The healthy control is reproduced.
3. A/B/C captures exist for both.
4. The first corrupt stage is proven.
5. Evidence includes hashes, dimensions, and runtime provenance.
6. The trusted reference-resize ablation is completed.
7. The GPU prefilter is either implicated or exonerated.
8. If corruption begins later, the graph is traced to the first bad stage.
9. Root cause is separated from symptoms/hypotheses.
10. The smallest root-cause fix is implemented.
11. The bad case is rerun and the lattice is gone.
12. The healthy control remains healthy.
13. Conventional scaler controls remain unchanged.
14. A focused regression test covers the exact source!=model failure.
15. A short report records tested hypotheses, rejected hypotheses, first bad stage, root cause, fix, and before/after evidence.

Continue until the causal boundary and fix are proven, or report a specific external blocker with the evidence collected.

## Controlled temporal-participation matrix (2026-09-03)

The known-bad `sintel_cave` frame 48, 1280x720 → 1920x1080 case was rerun with
the geometry-reset containment bypass enabled so temporal participation could
be isolated without changing the source/model inputs. Native/final PNGs were
visually reviewed by the capture worker.

| Color history | Recurrent | Result |
| --- | --- | --- |
| off | off | clean |
| on | off | repeating lattice |
| off | on | repeating lattice |
| on | on | repeating lattice |

The full-reset oracle was clean and the source==model control remained clean.
Therefore neither temporal subsystem is sufficient as a sole explanation; each
independently exposes a shared source/model-mismatch defect. Additional A/Bs
showed that changing motion displacement units, zeroing motion, changing
history interpolation, and disabling the native INT8 graph do not remove the
lattice. Setting learned strength to zero does remove it, but that is a
symptom-suppressing ablation and is not an acceptable fix. No production change
is retained from these rejected probes; the geometry full-reset remains an
oracle/containment control only.

### Stage-C probe

The same bad case was captured with the decoder accumulation readback enabled,
alongside temporal-off and full-reset controls. The stage-C payloads are
output-sized 4-channel float tensors (8,294,400 values each):

```text
probe  2769ff96684505d0df9b0cb9d11e34cab9d988dee32c9c8875b7f4a5121197c5
off    35604701e84ee8ae80b1065e9d7609eb04f8d5191a1db934a85bdfb2d081a98a
oracle 4f3e7e833df10135e5371752eebaccb6d7dc97a615aadb2bb869a199c2a48e56
```

The tensors differ numerically, but normalized visualizations of all three
contain strong architecture-periodic structure and are not a trustworthy
lattice detector. This probe therefore does not move the causal boundary to
stage C; final-output inspection remains authoritative.

### Prepass reprojection boundary (2026-09-03)

An output-sized FP16 readback of `u_reprojectedColor` was captured on the
known-bad 1280x720 → 1920x1080 history-only arm after frame-48 warmup. The
normalized payload itself contains the same large repeating lattice across the
cave, dragon, and foreground, before postpass composition. This proves the
first visible corruption is in the prepass color-history reprojection output,
not in CAS or the final presentation handoff.

Two value-isolation probes were also run. Replacing the sampled history value
with the current resolve in prepass removes the final lattice. Changing only
the postpass history publication value (current or upscaled) does not. These
results implicate the history value consumed by prepass reprojection; they do
not yet establish whether the stored payload or its coordinate transform is
the exact root cause. The temporary probes are not production behavior.

### History publication ablation (2026-09-03)

With the correct 1920x1080 viewport, two independent postpass publication
arms were visually reviewed on the same bad geometry: writing the current
resolve and writing `upscaledColor` both removed the lattice, while the normal
`modelColor` publication reproduced it. Since prepass still reads and blends
the published history on subsequent frames, this identifies the feedback
write of the already history-mixed `modelColor` as the smallest causal
boundary. The retained fix publishes the current neural resolve
(`upscaledColor`) instead, preserving history reads and recurrent admission
without feeding the accumulated temporal blend back into itself. This was
subsequently qualified with recurrent state enabled; see the closeout below.

### Post-fix focused qualification (2026-09-03)

The retained publication fix was rebuilt and exercised with native/final output
at a forced 1920x1080 viewport, frame 48, integrated-best-findings
configuration, CAS disabled, and recurrent admission disabled (history reads
remained enabled). The output dimensions and periodic 2x2 scores were:

| Case | Capture | Dimensions | SHA-256 | Score | Visual review |
| --- | --- | --- | --- | ---: | --- |
| 720→1080 mismatch | `/tmp/tforge_fix720_1788461977/frames/sintel_cave_1280x720_high_crf12_f48.png` | 1920x1080 | `2082d450bb69b8db2bb564e1beee252eadb1077e2870342c8d7ff62a3910b654` | 0.0232 | clean; no lattice or new halos/color shifts |
| 360→1080 mismatch | `/tmp/tforge_fix360_1788462006/frames/sintel_cave_640x360_high_crf12_f48.png` | 1920x1080 | `cbc70dd510c7f2ee617e9a4d01d02f800e19ba74033ff1f28c8553a423c181c1` | 0.0169 | clean; expected upscale softness only |
| 360→720 source==model | `/tmp/tforge_fix360eq_1788462026/frames/sintel_cave_640x360_high_crf12_f48.png` | 1280x720 | `6ab52ef07388d3a8cf350284914c754f8449473c8b308d37d675d9f0c94f7070` | 0.0303 | clean; no periodic contamination |

All scores are below the 0.20 fail-closed tripwire. The focused static
regression contract checks that mismatch no longer forces a full reset and that
postpass history publication remains `upscaledColor`. The precampaign gate
scores final/native output while retaining Stage-B provenance. Conventional
bicubic/Lanczos controls and the existing prefilter/provenance paths were not
changed.

### Full temporal-state semantic qualification (2026-09-03; failed)

The retained fix was qualified with both color history and recurrent state
enabled using the exact AMD semantic activation path. The three final/native
outputs, runtime traces, CSV provenance, and visual review are retained in
`benchmarks/quality_sweeps/lattice_p0_recurrent_qualification_20260903/`.
Each trace reports the required history/recurrent state, `prepass_input_resolve`
jitter, source-tap Mu-law, normal motion, CAS disabled, and no forced reset.
However, final scores were 0.054748 (1280x720→1920x1080), 0.028079
(640x360→1920x1080), and 0.030384 (640x360→1280x720); independent visual
review found a clear lattice in both mismatch outputs while the source==model
control remained clean. A controlled 720→1080 A/B reduced the score to 0.020051
when source-tap Mu-law was disabled; changing only jitter mode left it at
0.055246. The remaining P0 boundary is therefore the source-tap Mu-law path
under geometry mismatch. The P0 remains open; no production workaround or
history-publication change is being claimed from this failed qualification.

### Semantic closeout candidate: FP16 resolve source (2026-09-03)

The source-tap ablation isolated the defect to applying EOTF + Mu-law after
sampling the lossy rgba8 `u_sourceDisplay`. The retained fix keeps the semantic
prepass phase but resolves from the already-transformed RGB10/A2 `u_color`
image (the downstream history/reprojection resources remain FP16).
This removes the second quantization boundary without changing CAS, motion,
recurrent admission, or reset policy.

Exact AMD semantic captures were independently reviewed and found clean:

| case | final PNG SHA-256 |
| --- | --- |
| sintel_cave 720→1080 | `9484c4a0b6e3f07ac48ce6f83db966f30acdcd709b74849b705491e75414ae9f` |
| sintel_cave 360→1080 | `e24ef0c0f62f92129149a463b94f2d482126fa4ebdeed0d187a9c70de53b1915` |
| sintel_cave 360→720 | `70b60699f225e538b5f28dd1f2262b61dbf30e8b05be461f8609b4b43da57641` |

No repeating lattice, trails, halos, or color shifts were observed. The
corresponding SSIM values were 0.983629, 0.794357, and 0.683053. PNGs are
retained beside the prior failing evidence as `candidate_cave720_fix.png`,
`candidate_cave360_fix.png`, and `candidate_cave360eq_fix.png`.

### Production-semantic closeout (2026-09-03; PASS)

The three cases were rerun through `run_fsr_supersampling.py` after the fix,
using its `AMD_SEMANTIC_BASELINE` activation and imported runtime validator.
All traces passed. They report history and recurrent enabled, active
`prepass_input_resolve` jitter, source-tap Mu-law enabled, unjittered motion
with validity, reject-invalid-history, conditional reset policy, CAS disabled,
and no geometry-mismatch forced reset. The current binary is
`531403376de27ffc107f5647ee58d59146626daf3081c1d090d878880f6581a1`; Git HEAD
is `850a6bd6e0b303c02d8efc8b27a9bd8162882b16`; all runs were clean and used
config SHA `576ad1c1d6a02a95a4ef0ce732aea5440fed88d4e0277ea6c9552725d0880346`.

Final-output 2×2 periodic scores were 0.023164 (720→1080), 0.016783
(360→1080), and 0.030382 (360→720), all below the 0.20 fail-closed tripwire.
The mismatch outputs and source==model control were independently reviewed
clean, with no lattice, recurrent trails, stale contamination, halos, color
shifts, or instability. The temporal lattice P0 is closed under the same
semantic contract as the production quality campaign; the retained production
feedback fix remains `u_historyOut = upscaledColor`.

The authoritative effective color path is: decoded YUV → RGB conversion →
EOTF + Mu-law in `yuv_to_fsr_input` → RGB10/A2 `u_color` model image →
source-to-model resampling when needed → prepass-owned Halton-23 jittered
Gaussian resolve → neural reconstruction → FP16 temporal history/recurrent
composition. The rgba8 source-display image is retained for presentation and
control uses and is not sampled for the production prepass current-color
resolve.

### P0 reopened and resolved after human review (2026-09-04)

Human review reopened this P0 because the prior production-semantic
qualification used the old 2×2 checker score, which is not a sufficient lattice
detector. The canonical campaign is explicitly historical/invalidated
(`canonical=false`, `historical_only=true`, `failed_quality_gate=visible_periodic_lattice`);
its automated qualification PASS and human-review FAIL are preserved.

The exact semantic reproducer was rerun with history/recurrent state enabled:

| arm | result |
| --- | --- |
| 1280×720 → 960×540 GPU prefilter → 1920×1080 | A/B clean; final lattice present |
| same path with libswscale reference resize | A/B clean; final clean |
| 640×360 → 640×360 → 1280×720 control | final clean |

The reference-resize ablation changes only the source→model prefilter and
removes the artifact. This isolates the GPU prefilter arithmetic/storage-image
path as the first causal boundary; the normalized Stage-C dump is identical in
bad and healthy runs and is marked `uncertain`.

The smallest retained fix in `shaders/fsr4/bicubic_prefilter.comp` uses a
linear four-sample resolve for downsampling and preserves Catmull–Rom for
enlargement. The rebuilt exact bad case and healthy control were visually
reviewed clean (no lattice, trails, halos, stale contamination, or color
shifts). Evidence and classifications are retained locally under
`benchmarks/quality_sweeps/lattice_corruption_diagnostic/runs/20260904T-reopened-current/`;
new diagnostic captures remain local/untracked and are not pushed; previously
tracked historical media is preserved unchanged. The machine-readable
matrix is `benchmarks/quality_sweeps/lattice_corruption_diagnostic/prevalence_matrix.json`.

The fixed semantic temporal matrix is recorded in
`runs/20260904T-reopened-current/fix_temporal_matrix_v3/matrix.json`; all four
history/recurrent combinations were visually reviewed clean. Required 360→1080
and 720→1080 frame-47/48/49 runs are recorded in
`fix_required_routes/routes.json` pending the final visual-review annotation.
The capture runner’s `--history` and `--recurrent` switches validate the
effective runtime trace, so temporal-off results cannot be mistaken for a
production-on qualification.

### Precision and transform-order ablation (2026-09-04)

The remaining carrier/order hypotheses were tested without changing the
production Vulkan descriptor contract. `precision_order_ablation.py` consumes
the native Stage-A readback from the deterministic reproducer and compares a
float32 carrier with simulated RGB10 quantization, plus transform-before-
resolve versus resolve-before-transform using a float carrier. The report is
`precision_order_ablation.json`.

RGB10 quantization produced RMSE `0.0001482` and maximum error `0.0004830`.
Transform ordering was numerically distinguishable (RMSE `0.0014204`, maximum
error `0.09435`). The report emits 2–32 px band energy and narrow-peak metrics
for matched comparison, but these offline residuals are not sufficient to
identify a causal precision or ordering boundary. The controlled GPU
reference-resize A/B therefore remains the evidence supporting the retained
prefilter fix.
An in-process RGBA16F model-image swap and an independent GPU
resolve-before-transform arm remain intentionally unimplemented: the current
source/model descriptors and shader image formats are RGB10/A2, and adding
those arms would be a broad resource/pipeline change unrelated to the
evidence-supported fix. This limitation is machine-readable in
`hypothesis_status.json`; no claim of a GPU carrier swap is made.
