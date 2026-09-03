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
