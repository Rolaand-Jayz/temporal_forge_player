# Temporal Forge Reconstruction Quality Plan

**Status:** ACTIVE
**Priority:** P0 — sole development priority
**Theme:** Reconstruction quality / image formation
**Non-goal:** Feature development

---

## 1. Goal

Achieve clearly superior real-time video reconstruction quality while preserving Temporal Forge's existing native Vulkan/RDNA3 performance advantage.

The immediate observed quality problems are:

- Low-resolution source aliasing/stair-step geometry remains visible after reconstruction.
- Temporal Forge recovers significant detail, but the final picture is somewhat soft.
- Tone/exposure/local-contrast behavior appears different from the native high-resolution reference, especially in dark scenes.
- Existing evidence suggests the core FSR reconstruction is substantially functional.
- The current postpass and presentation path are therefore primary suspects.

The current postpass approximately behaves as:

```text
stableColor  = bilinear(source)
learnedColor = FSR reconstruction
finalColor   = mix(stableColor, learnedColor, learnedStrength)
```

For some low-resolution inputs, `learnedStrength` falls to roughly `0.05–0.15`, making the final image overwhelmingly derived from the bilinear source base. This may preserve low-resolution staircase geometry while overlaying neural detail.

The purpose of this plan is to determine whether that hypothesis is correct and, if so, replace the weak image-formation stage without disturbing the neural core.

---

## 2. Current Working Hypotheses

### H1 — Primary
The neural reconstruction contains useful recovered detail, but the final compositing path reintroduces or preserves low-resolution staircase geometry through a bilinear spatial base.

### H2
The apparent exposure problem is not a simple exposure offset but a tone-transfer / low-frequency contrast / model-color reconstruction discrepancy.

### H3
The final Qt presentation scaler may add a secondary quality loss when target-resolution reconstructed images are resized for display.

### H4
If pure learned output still contains the staircase defect, the source of the problem is deeper than the stable-base compositing path and must be isolated before further filtering work.

These are hypotheses, not conclusions.

---

## 3. Non-Goals

During this campaign, do not spend development effort on:

- UI redesign
- player feature additions
- general cleanup
- broad refactors
- new media-library functionality
- packaging polish
- model retraining
- replacing the model weights
- changing convolution topology
- redesigning the native 14-pass graph
- replacing codec-motion handling
- adding full-frame expensive optical flow
- unrelated reverse engineering

The player already looks and functions well enough for this phase.

---

## 4. Completion Criteria

This plan is **not complete** merely because all experimental infrastructure exists.

It is complete only when all of the following are true:

1. The primary cause of persistent staircase artifacts has been identified.
2. A reproducible reconstruction configuration materially reduces those artifacts.
3. Recovered detail is retained or improved.
4. Softness is reduced without unacceptable haloing, overshoot, ringing, or noise amplification.
5. The tone/color discrepancy has been identified and corrected or explicitly bounded.
6. The winning configuration passes the representative real-video corpus.
7. Multi-frame temporal validation shows no unacceptable regression in flicker, crawl, ghosting, or disocclusion.
8. Performance remains within the project's accepted real-time target.
9. The winning behavior is runtime-configurable and reproducible without source edits.
10. The winning configuration is suitable to become the default.
11. Claims are backed by saved benchmark artifacts, exact configuration, and before/after comparisons.
12. Strong AMD/Linux-compatible spatial upscaling controls are included before any public superiority claim.

---

## 5. Non-Negotiable Experiment Architecture

### 5.1 Runtime configurability

Campaign evidence may be recorded as `metrics_only` when the numeric results,
configuration identity, binary identity, timing, and temporal provenance are
available without retaining image payloads. This relaxes storage of review
images only; it does not relax any matrix coverage, metric, provenance, or
temporal validation requirement.

Do **not** hardcode experimental constants and rebuild for every experiment.

Build a centralized **Quality Lab** configuration system first.

Preferred persistent file:

```text
config/quality_lab.json
```

CLI overrides may be added where practical.

Existing environment-variable controls may remain for compatibility, but the new testing system must not depend solely on environment variables.

Every meaningful quality parameter introduced during this work must be runtime configurable.

### 5.2 Typed configuration

Create one typed configuration structure and resolve all values into it once.

Do not scatter:

- `getenv()`
- magic floats
- one-off experiment constants
- hidden resolution-specific hacks

through rendering code.

At minimum expose:

```text
qualityLab.enabled

composition.mode
composition.learnedStrength
composition.residualStrength

baseFilter.mode
baseFilter.b
baseFilter.c

residual.lowpassMode
residual.radius
residual.sigma

sharpen.mode
sharpen.strength
sharpen.limit
sharpen.threshold

tone.exposureEV
tone.contrast
tone.contrastPivot
tone.gamma

presentation.filter
```

Defaults must reproduce the current baseline exactly unless a parameter is explicitly experimental and disabled by default.

---

## 6. Baseline Preservation

Before changing image behavior:

- [x] Record current git commit / tree state.
- [x] Build the current player.
- [x] Run the existing representative quality corpus.
- [x] Preserve current output and metrics under an immutable baseline tag.
- [x] Record GPU reconstruction timing.
- [x] Record full pipeline timing.
- [x] Record p95 where supported.
- [x] Record PSNR.
- [x] Record SSIM.
- [x] Record edge SSIM.
- [x] Record mean luminance.
- [x] Record useful luminance percentiles if practical.
- [x] Preserve representative stills when available; metrics-only campaign
  closure does not require retaining image payloads.
- [x] Preserve exact baseline configuration.
- [x] Do not overwrite existing benchmark results.

### Baseline result

**Status:** Captured 2026-08-21 before new image-behavior edits.

**Commit/tree:**
Branch `main`, `HEAD=45272e22cbd0964314cd3e07289b83b808a9aa61`. The worktree was
already dirty: 36 tracked files modified and 27 untracked paths. The exact
control binary was `build-fast/temporal_forge_player`, SHA-256
`dbd26dc6d0a34e4ee4d80617c60cb38d9391cb6eb4e79bad98359b897fd1ad45`.

**Representative timing:**
The unchanged binary built with `cmake --build build-fast -j$(nproc)` and
passed 9 enabled CTest cases; 1 test was skipped and 3 were disabled. On
AMD Radeon RX 7900 GRE, warmed 426x240 -> 1920x1080 captures measured
1.129-1.147 ms mean GPU, 1.091-1.100 ms p50, and 1.660-1.664 ms p95 across
Sintel rooftop, synthetic edges/text, and synthetic dark. Pipeline CPU mean
was 0.131-0.156 ms, with p95 1.382-1.461 ms.

**Quality summary:**
The tagged 7-row high-quality 426x240 capture is preserved at
`/tmp/tforge-quality-baseline-20260821/quality_426x240.csv`, with stills and
logs under the unique tag
`quality-campaign-baseline-20260821`. Mean FSR PSNR/SSIM/edge-SSIM was
27.5954 dB / 0.744904 / 0.815945, versus 0.827699 SSIM for Lanczos and
0.832742 for bicubic controls. The baseline output visibly preserves stepped
diagonal structure and softens the synthetic text/edges relative to the
reference.

**Known visible defects:**
The baseline still has staircase geometry, softness, and a measurable gap to
the spatial controls. Tone and temporal behavior remain unisolated.

---

## 7. Reconstruction Modes

Support at minimum:

```text
current
base_only
learned_only
direct_blend
detail_residual
```

### 7.1 `current`

Exactly preserve current Temporal Forge behavior.

This is the control.

### 7.2 `base_only`

Output only the selected spatial reconstruction base.

Purpose:

- isolate the spatial/base filter;
- compare staircase geometry independently of the neural output.

### 7.3 `learned_only`

Output the learned FSR reconstruction with no stable-source blend.

This experiment is essential.

Question:

> Does staircase geometry remain in the pure learned reconstruction?

This result determines the next branch of work.

### 7.4 `direct_blend`

```text
final = mix(base, learned, learnedStrength)
```

All strength values runtime configurable.

### 7.5 `detail_residual`

Test:

```text
base = spatialFilter(source)

lowLearned = lowpass(learned)

learnedDetail = learned - lowLearned

final = base + residualStrength * learnedDetail
```

Clamp safely. Preserve finite output.

Purpose:

Allow a smooth, reference-stable spatial reconstruction to define large-scale geometry and tone while using the FSR output primarily for recovered high-frequency information.

This is a hypothesis to test, not a predetermined final design.

---

## 8. Spatial Base Filters

Implement runtime-selectable:

```text
bilinear
mitchell
catmull_rom
lanczos2
```

### Mitchell-Netravali default

```text
B = 1/3
C = 1/3
```

### Catmull-Rom

Equivalent to:

```text
B = 0
C = 0.5
```

If B/C are useful generally, keep them runtime configurable.

Do not remove bilinear. It is the control.

Prefer GPU-friendly implementations and reuse the existing source-tile / LDS infrastructure where practical.

Avoid unnecessary target-resolution intermediate images where the operation can remain fused in the postpass.

---

## 9. Sharpening

Implement an optional inexpensive **post-reconstruction adaptive sharpening** stage.

Modes:

```text
none
adaptive
```

If a compatible CAS/RCAS-style implementation is legally and architecturally suitable, it may be used. Otherwise implement a compact edge/contrast-adaptive sharpening operator.

Required runtime parameters:

```text
strength
threshold
limit
```

Must guard against:

- halos
- overshoot
- undershoot
- compression-noise amplification
- ringing

Do **not** sharpen the decoded low-resolution source before FSR.

Sharpen only the reconstructed/final high-resolution result.

---

## 10. Tone / Exposure Controls

The visible difference may not be a simple exposure error.

Treat separately:

```text
exposure
contrast
gamma / tone response
```

Expose:

```text
tone.exposureEV
tone.contrast
tone.contrastPivot
tone.gamma
```

Exposure adjustment must be performed in the correct linear-light domain, not by arbitrarily multiplying final sRGB values.

Defaults must preserve current output.

The objective is to determine whether the perceived discrepancy is caused by:

- global exposure;
- gamma mismatch;
- compressed contrast;
- incorrect model-color inverse;
- display transfer behavior;
- low-frequency luminance drift.

Do not permanently mask a transfer-function bug with a cosmetic exposure offset.

Any retained tone adjustment must be classified as either:

```text
corrective
```

or

```text
aesthetic
```

Corrective changes are preferred.

---

## 11. Presentation Isolation

Separate reconstruction quality from Qt/player presentation quality.

Add a reliable way to dump the exact reconstructed target-resolution image **before Qt presentation resizing**.

For benchmark candidates preserve, where applicable:

```text
reconstruction_output.png
presented_output.png
```

The reconstruction image is authoritative for FSR/postpass quality.

If the reconstruction image is correct and the displayed player image is not, the defect belongs to presentation.

Make presentation filtering runtime-selectable.

At minimum:

```text
nearest   # diagnostic only
linear
bicubic   # if practical
```

When reconstructed output dimensions equal the physical presentation target, avoid unnecessary rescaling.

---

# 12. Experiment Stages

Do **not** run the full Cartesian product of every parameter.

Use staged experiments and decision gates.

---

## Stage A — Locate the Staircase Origin

### Purpose

Determine whether staircase structure:

1. already exists in pure learned FSR output; or
2. is primarily preserved/reintroduced by stable-source compositing.

### Conditions

No sharpening.
Neutral tone controls.
Dump reconstruction output before Qt presentation.

### A1 — Baseline

- [x] `current`

**Result:**
The complete seven-clip 426x240 run is preserved in
`/tmp/tforge-quality-stageA-20260821/runner/stageA-20260821T204600Z/`.
The runner recorded seven rows, with mean FSR PSNR/SSIM/edge-SSIM of
27.607751 dB / 0.745318 / 0.816074. This is consistent with the earlier B0
control within warmed-run variance; B0 remains the campaign baseline.

**Conclusion:**
The current path is reproducible and remains the control. The small metric
variation between independent warmed runs is not treated as an improvement.

### A2 — Pure base

- [x] `base_only + bilinear`

**Result:**
Configuration was `benchmarks/quality_sweeps/stage_a/base_only_bilinear.json`:
Quality Lab enabled, `composition.mode=base_only`, `baseFilter.mode=bilinear`,
neutral tone, no sharpening. On the seven-clip 426x240 subset, mean FSR
PSNR/SSIM/edge-SSIM was 27.865129 dB / 0.750403 / 0.819522. The saved
candidate timing rows averaged 1.476 ms GPU and 1.593 ms pipeline CPU.
Artifacts and exact config are under the Stage A runner directory above.

**Conclusion:**
SUPPORTED. Removing the learned contribution and using the stable base
improved the aggregate metrics over both B0 and the independently rerun
current control. The gain is large enough to justify the spatial base-filter
stage, but it does not by itself prove bilinear is the best filter.

### A3 — Pure learned

- [x] `learned_only`

**Question:**
Does the staircase remain?

**Result:**
Configuration was `benchmarks/quality_sweeps/stage_a/learned_only.json`:
Quality Lab enabled, `composition.mode=learned_only`, neutral tone, and no
sharpening. On the same seven-clip subset, mean FSR PSNR/SSIM/edge-SSIM was
27.249229 dB / 0.738438 / 0.808600; mean GPU time was 1.470 ms. The
synthetic edge/text still shows colored stepped edge structure and weaker text
than the native reference.

**Conclusion:**
REJECTED as a deeper reconstruction fix at this stage. The learned-only path
does not remove the visible staircase/colored-edge defect and is measurably
worse than the stable base. This does not authorize changes to weights,
topology, motion handling, or the native INT8 graph.

**Decision gate:**

- If **NO / materially reduced** → continue to Stage B. Postpass/base reconstruction is the primary suspect.
- If **YES / substantially unchanged** → stop the planned base-filter campaign and create a deeper reconstruction-isolation subplan before proceeding.

**Gate result:** The staircase/colored edge defect is not materially reduced in
the pure learned output, but the pure learned path is measurably worse and the
pure stable base is measurably better. The dominant quality loss is therefore
in the postpass composition/base reconstruction for this campaign. The planned
spatial campaign proceeds; the learned-only failure is preserved as a rejection
of a deeper graph change.

### A4 — Direct blend sweep

Test:

```text
learnedStrength:
0.00
0.10
0.25
0.50
0.75
1.00
```

- [x] Sweep completed.
- [x] Representative frames saved.
- [x] Metrics saved.
- [x] Artifact trend documented.

**Result:**
The complete sweep used the six exact configs
`stage_a/direct_blend_000.json`, `direct_blend_010.json`,
`direct_blend_025.json`, `direct_blend_050.json`, `direct_blend_075.json`,
and `direct_blend_100.json` on the same seven-clip 426x240 subset. Mean
FSR SSIM by learned strength was:

```text
0.00  0.745343
0.10  0.745361
0.25  0.745068
0.50  0.743955
0.75  0.741870
1.00  0.738435
```

Mean edge-SSIM followed the same downward trend from 0.816242 at 0.00 to
0.808619 at 1.00. GPU means were 1.487 ms at 0.00, 1.488 ms at 0.10,
1.492 ms at 0.25, 1.489 ms at 0.50, 1.415 ms at 0.75, and 1.490 ms at
1.00. The complete ranked CSV is
`/tmp/tforge-quality-stageA-20260821/runner/stageA-20260821T204600Z/rankings.csv`.

**Conclusion:**
SUPPORTED for rejecting strong direct learned blending, INCONCLUSIVE between
0.00 and 0.10. Directly adding more learned color does not repair the
staircase and degrades real-video metrics after 0.10. The tiny 0.00/0.10
difference is within run variance, so neither is selected as a winner yet.

**Decision:**
Proceed to Stage B with a stable-base-first composition. Keep the current path
and direct blend as controls, but do not spend further sweep budget on learned
strengths above 0.10 until a better spatial base or residual formulation is
found.

**Next action:**
Run bilinear, Mitchell, Catmull-Rom, and Lanczos2 with neutral tone and no
sharpening, beginning with `base_only` and then the strongest A4 blend
neighborhood if the base-filter result supports it.

---

## Stage B — Spatial Base Reconstruction

Proceed only if Stage A supports the postpass/base hypothesis.

Test:

```text
bilinear
mitchell
catmull_rom
lanczos2
```

No sharpening.
Neutral tone controls.

For direct blend, sweep useful `learnedStrength` neighborhoods discovered in Stage A.

- [x] Bilinear control.
- [x] Mitchell.
- [x] Catmull-Rom.
- [x] Lanczos2.
- [x] Visual comparison.
- [x] Metrics.
- [x] Timing.

**Best spatial base:**
`base_only + bilinear` for the tested seven-clip 426x240 subset.

**Rejected filters and why:**
Mitchell was close but lower: mean SSIM 0.749177 versus bilinear 0.750403,
with mean GPU 1.545 ms versus 1.474 ms. Catmull-Rom and Lanczos2 were close
to the current-path control and materially below bilinear at mean SSIM
0.745343 and 0.744997. Catmull-Rom also recreates the soft/stepped diagonal
appearance; Lanczos2 does not recover the metric gap and costs more GPU time.
These are rejected as the Stage B base winner, not removed from the runtime
Quality Lab.

**Result:**

The immutable Stage B run is
`/tmp/tforge-quality-stageB-20260821/stageB-20260821T205110Z/`.
All four candidates produced seven rows with finite output and tagged stills.
The best mean FSR PSNR/SSIM/edge-SSIM was bilinear at 27.865129 dB /
0.750403 / 0.819522. Mitchell measured 27.836836 / 0.749177 / 0.819374,
Catmull-Rom 27.610918 / 0.745343 / 0.816242, and Lanczos2 27.606651 /
0.744997 / 0.816238.

**Decision:**

Retain bilinear as the Stage C stable base. Do not select a sharper kernel
solely because it looks crisper; the measured and visual edge behavior does
not support it. Investigate a low-frequency stable base plus controlled learned
detail residual instead of direct color blending.

---

## Stage C — Detail Residual Composition

Test:

```text
base:
  mitchell
  catmull_rom
  lanczos2
```

Test:

```text
residualStrength:
0.25
0.50
0.75
1.00
1.25
```

Test several small GPU-cheap low-pass configurations appropriate to a high-frequency decomposition.

Begin with a compact 3x3 / separable kernel.

Do not add a costly large blur without evidence.

### Required questions

- Does the smoother base remove staircase structure?
- Does the FSR residual restore useful texture and edge detail?
- Does the residual reintroduce the staircase?
- Does the residual introduce ringing or halos?
- Does low-frequency tone remain closer to reference?

- [x] Sweep completed.
- [x] Metrics recorded.
- [x] Representative crops saved.
- [x] Timing recorded for representative candidates; coverage caveat is recorded below.
- [x] Best residual candidate identified and compared against the stable-base control.

**Best residual configuration:**
`bilinear + gaussian3x3 + detail_residual + residualStrength=0.25` was the
best residual candidate in the compact screen by mean metrics. It is not the
best overall configuration because the matched stable-base control was better.

**Conclusion:**
REJECTED for promotion. The immutable sweep is
`/tmp/tforge-quality-stageC-20260821/stageC-20260821T205509Z/` and contains
23 runtime candidates over `tos_daylight`, `sintel_rooftop`, and
`synthetic_edges_text`, all at frame 48 and 426x240 input. Every candidate
returned exit code 0 with three finite quality rows.

The best residual candidate measured mean FSR PSNR/SSIM/edge-SSIM of
22.631703 / 0.705373 / 0.701233. The matched `base_only + bilinear` control
measured 22.664905 / 0.706354 / 0.702011 on the same three clips, so the
residual lost 0.033202 dB PSNR, 0.000981 SSIM, and 0.000778 edge-SSIM.
Increasing residual strength consistently moved the metrics downward; the
synthetic edge/text still did not recover useful detail, while higher
strengths retained more of the existing colored/stepped structure.

The residual path also expands the postpass work substantially. The best
candidate's measured sample was GPU 2.889 ms / pipeline CPU 3.001 ms, versus
the Stage B bilinear aggregate of GPU 1.474 ms / pipeline CPU 1.619 ms. Timing
summary lines were present in 11 of 69 per-clip logs; the missing timing rows
are preserved and require finalist reruns with explicit timing verification.

The detail-residual implementation remains a runtime-selectable rejected
control. It did not establish that the defect is inside the native INT8 graph,
so no model, topology, codec-motion, or optical-flow changes are justified.

**Decision:**

Do not carry detail residual into the promoted path. Keep the stable bilinear
base and test adaptive sharpening independently, starting at low strength and
with conservative limit/threshold settings. Retain the residual configs and
artifacts for regression controls.

---

## Stage D — Adaptive Sharpen

Take only top candidates from Stages A–C.

Test approximate sharpen strength:

```text
0.00
0.10
0.20
0.30
0.40
0.50
```

Sweep limiter/threshold over a compact useful range.

Do not reward higher SSIM if the image visibly develops halos.

Required visual checks:

- hair
- faces
- text
- thin diagonals
- foliage
- high-contrast edges
- compression noise

- [x] Sweep completed.
- [x] Halo/overshoot review completed.
- [x] Best sharpen configuration identified.

**Best sharpen configuration:**
`none` remains the promoted choice. `adaptive` is retained as a runtime
control, but no tested point produced a material quality gain.

**Conclusion:**

The immutable screen is
`/tmp/tforge-quality-stageD-20260821/stageD-20260821T210236Z/`. It contains
six strength points (`0.00` through `0.50`) and four compact
limiter/threshold variants on the matched three-clip, frame-48, 426x240
screen. All ten candidates exited successfully and produced three finite
rows with timing aggregates.

The zero-sharpen control measured mean FSR PSNR/SSIM/edge-SSIM of
22.664905 / 0.706354 / 0.702011. The highest edge-SSIM was only 0.702023
(`adaptive_s020`), while the highest SSIM was 0.706354 for the zero-sharpen
control within rounding. The synthetic edge/text still did not show useful
text or diagonal recovery at strengths 0.10, 0.20, or 0.50. The visual
review found no large halo or overshoot, but also no material reduction in
softness; the stronger settings are therefore not justified by the tiny
metric movement.

The zero-sharpen timing aggregate was GPU 1.477 ms / pipeline CPU 1.595 ms.
Adaptive candidates were approximately GPU 1.628–1.639 ms and pipeline CPU
1.736–1.831 ms. That is a measurable cost without a corresponding quality
gain.

**Decision:**

Do not promote adaptive sharpening. Keep it runtime-selectable for later
interaction checks, but carry `sharpen.mode=none` into Stage E. Investigate
tone/transfer behavior independently before considering any stronger spatial
operator.

---

## Stage E — Tone / Exposure

Only after spatial structure is selected.

### E1 — Exposure

Test:

```text
-0.50 EV
-0.25 EV
 0.00 EV
+0.25 EV
+0.50 EV
```

Then narrow around the best result at approximately `0.05–0.10 EV` resolution.

### E2 — Contrast

Test modest values around:

```text
-0.10
 0.00
+0.10
```

### E3 — Gamma

Test:

```text
0.90
1.00
1.10
```

Do not blindly Cartesian-product all tone controls.

Use coordinate-descent style testing:

```text
exposure
→ contrast
→ gamma
→ small interaction sweep around winner
```

Measure low-frequency luminance error.

Do not mistake an attractive but reference-inaccurate tone curve for correctness.

- [x] Exposure sweep.
- [x] Contrast sweep.
- [x] Gamma sweep.
- [x] Interaction check.
- [x] Corrective vs aesthetic determination.

**Tone conclusion:
The exposure sweep initially produced a misleading SSIM result: `-0.50 EV`
was the SSIM winner in the wide screen, but its PSNR and low-frequency luma
were much worse, so it was a tone mismatch rather than a correction. The
neutral control had the lowest low-frequency luma MAE in that screen
(`0.020485`) and a near-zero bias (`+0.003250`). The contrast/gamma screen
rejected both contrast adjustments. Gamma `0.90` improved SSIM by changing
the tone curve, but increased low-frequency luma MAE to `0.027212`; gamma
`1.10` was worse. Neither is corrective.

The narrow exposure screen was then refined at `0.005 EV` resolution. The
immutable run is
`/tmp/tforge-quality-stageEfine-20260821/stageEfine-20260821T211407Z/`.
`-0.010 EV` was the stable winner across all three clips: mean FSR
PSNR/SSIM/edge-SSIM `22.675192 / 0.706577 / 0.702106`, low-frequency luma
MAE `0.019838`, and signed bias `+0.002084`. The neutral candidate measured
`22.664905 / 0.706354 / 0.702011`, MAE `0.020485`, and bias `+0.003250`.
The winner stayed at the stable bilinear-base cost, approximately `1.477 ms`
GPU and `1.597 ms` pipeline CPU for the three-clip run.

This is retained as a **corrective** small transfer/exposure compensation,
not an aesthetic grade. Visual review confirms that it slightly closes the
low-frequency brightness gap without changing the still's spatial softness;
the remaining softness and presentation behavior must be tested separately.

---

## Stage F — Local Neighborhood / Interaction Check

Take the best 3–5 complete configurations.

Perform a small parameter neighborhood sweep around each.

Purpose:

Ensure the winning result is not an accidental isolated parameter point.

- [x] Candidate neighborhoods tested.
- [x] Robust winner identified.

**Winner:**
`base_only` composition, bilinear base, residual disabled, adaptive sharpen
disabled, corrective tone `exposureEV=-0.015`, neutral contrast/pivot/gamma.

The post-jitter-fix local sweep is preserved at
`/tmp/tforge-quality-stageF-local-fixed-20260821/stageF-local-fixed-20260821T220253Z/`.
Its three-clip mean for the winner was PSNR/SSIM/edge-SSIM
`24.101126 / 0.737582 / 0.713305`, low-frequency luma MAE `0.014732`,
signed bias `+0.001477`, GPU `1.425 ms`, and pipeline `1.539 ms`.
The `-0.010 EV` neighbor was effectively tied but lower at
`24.096957 / 0.737503 / 0.713210`; the `-0.005 EV` neighbor was lower again.
Both adaptive-sharpen neighbors were lower than the no-sharpen control while
adding roughly `0.18 ms` GPU, so sharpening was not promoted. The earlier
pre-jitter-fix local run remains preserved but is not used for selection
because it measured the diagnosed jitter shimmer.

---

# 13. Test Corpus

Retain the existing real-world corpus.

Prioritize at least:

```text
Tears of Steel daylight
Tears of Steel debris
Sintel rooftop
Sintel cave
Big Buck Bunny foliage / branches
synthetic edges / text
synthetic motion
synthetic dark
```

Prioritize source resolutions:

```text
426x240
640x360
854x480
1280x720
1920x1080 where reference data exists
```

Do not judge the system from one aggregate score.

Report by:

- scene
- source resolution
- output resolution
- compression level

---

## 14. Proper Anti-Aliasing Ground Truth

The existing synthetic diagonal test is insufficient if the supposed high-resolution reference itself contains raster stair-stepping.

Add a supersampled ground-truth suite.

Generate vector/high-resolution primitives at **at least 4x** final target resolution:

- diagonal lines at multiple slopes
- circles / arcs
- thin text
- 1px-equivalent lines
- high-contrast boundaries
- colored boundaries

### Path A — Ground truth

```text
supersampled master
→ high-quality downsample
→ target-resolution antialiased reference
```

### Path B — Temporal Forge

```text
same supersampled master
→ low-resolution source
→ Temporal Forge
→ target resolution
```

Compare Path B against Path A.

This becomes the authoritative staircase/de-aliasing test.

- [x] Generator implemented.
- [x] Ground truth produced.
- [x] Low-resolution inputs produced.
- [x] Comparison integrated into benchmark tooling.

The supersampled suite is implemented by
`benchmarks/quality_sweeps/run_supersampled_aa.sh` and the 8x vector master
`benchmarks/quality_sweeps/supersampled_aa/master.svg`. The preserved run under
`/tmp/tforge-supersampled-aa-20260821b/` uses a 7680x4320 master, a 1920x1080
antialiased reference, and a 426x240 source. The selected stable-base tone
candidate measured PSNR `19.604901`, SSIM `0.786898`, edge-SSIM `0.734911`,
luma MAE `0.013326`, and bias `+0.002227`. Its edge-SSIM exceeded the
Lanczos control by `0.019074`, although SSIM remained below the bicubic and
Lanczos controls. Visual inspection found no severe staircase discontinuity;
the remaining issue is controlled softness, not a justification for a sharper
global kernel.

---

## 15. Metrics

Keep:

```text
PSNR
SSIM
edge SSIM
```

Add where practical:

```text
mean luminance error
luminance percentile error
RGB or perceptual color error
edge overshoot / undershoot
high-frequency energy comparison
```

For synthetic-edge tests, add measurements capable of detecting:

- staircase persistence
- haloing
- oversharpening
- edge-width error

Do not optimize solely for PSNR or SSIM.

---

## 16. Automated Sweep Runner

Create a quality sweep runner.

Conceptual invocation:

```bash
./benchmarks/video_corpus/run_quality_sweep.py \
  --config benchmarks/quality_sweeps/spatial_stage.json
```

The runner must:

1. Generate experiment configurations.
2. Run the player automatically.
3. Capture outputs.
4. Compute metrics.
5. Record performance.
6. Store the exact configuration used.
7. Produce CSV and/or JSON results.
8. Rank candidates.
9. Preserve representative stills/crops.
10. Never overwrite prior sweep artifacts.
11. Allow a staged subset of the corpus for rapid iteration.
12. Allow finalist runs over the broader corpus.

Prefer explicit sweep manifests over hidden script constants.

---

## 17. Performance Gate

Quality is the current priority, but do not silently destroy the project's performance advantage.

Record GPU and full pipeline time for every finalist.

Guidance:

```text
+0.1 to +0.5 ms:
potentially acceptable for a substantial quality gain.

multi-millisecond regression:
requires strong justification and should trigger optimization work before acceptance.
```

Do not reject an experimental algorithm solely because an unoptimized prototype is slightly slower.

Preserve the current fast path as a selectable baseline throughout the campaign.

---

## 18. AMD/Linux Competitor Controls

Before any public claim that Temporal Forge is superior to existing Linux solutions, generate comparable outputs using strong AMD/Linux-compatible spatial video scaling paths.

At minimum include:

```text
mpv/libplacebo high-quality EWA/Lanczos baseline
mpv FSR1 + appropriate sharpening
one strong modern shader chain such as ArtCNN/FSRCNNX
with good chroma reconstruction and adaptive sharpening
```

Use:

- identical source frames;
- identical output dimensions;
- comparable tone handling;
- saved exact configurations.

These controls do **not** block the initial quality investigation.

They are required before superiority claims.

The required same-frame control capture is preserved under
`/tmp/tforge-competitors-20260821/`. libplacebo EWA and FSRCNNX were available
and were compared at identical 426x240 input, frame 48, and 1920x1080 output.
The installed mpv `0.41.0` build does not expose an `fsr` scale option, so an
mpv FSR1 result could not be generated; this limitation is recorded rather
than substituted with an unrelated path. No superiority claim is made. On the
shared Tears of Steel frame, Temporal Forge / libplacebo EWA / FSRCNNX / native
bicubic SSIM was `0.759343 / 0.766153 / 0.763241 / 0.767033`, with mixed
edge-SSIM results (`0.616418 / 0.611988 / 0.609697 / 0.612062`).

---

## 19. Temporal Quality — Second Phase

Once still-frame spatial quality is materially improved, extend validation beyond one warmed frame.

Measure sequences for:

- flicker
- crawling edges
- ghosting
- disocclusion
- scene cuts
- motion stability
- fine-detail persistence

Do not use single-frame SSIM as proof of temporal quality.

Create a reproducible temporal benchmark path.

- [x] Multi-frame capture added.
- [x] Temporal metrics selected/implemented.
- [x] Representative motion sequences tested.
- [x] No unacceptable temporal regression.

The temporal runner now waits for complete, byte-valid PPM frames, preserves
the FSR/reference/Lanczos/direct-bilinear sequences when
`TFORGE_TEMPORAL_ARTIFACT_DIR` is set, and reports both the Lanczos-relative
and direct-bilinear frame-delta errors. Post-fix runs covered Tears of Steel,
Sintel, and a freshly generated moving control whose frame hashes change.
FSR-vs-bilinear temporal-delta absolute error was `0.005165` for Tears of
Steel, `0.291354` for Sintel, and `0.014183` for the moving control; the strips
show no visible ghost trails or new shimmer. The existing `synthetic_motion`
corpus generator was found to evaluate its drawbox position once, producing a
static sequence, so it is not used as temporal acceptance evidence.

The root cause was not missing future-frame data: base-only composition has no
temporal accumulator to consume Halton jitter, so applying jitter to its base
sample converted the jitter sequence into visible shimmer. The base-only path
now samples an unjittered source coordinate, while learned/recurrent modes
retain their jittered coordinate. The runtime jitter override is also now
propagated to `SideBufferSynth`; the neural graph, weights, topology, and
motion architecture remain unchanged.

---

## 20. Network / Motion Freeze Rule

During the current image-formation phase, do **not**:

- retrain the model;
- alter weights;
- change convolution topology;
- redesign the INT8 graph;
- rewrite codec-motion handling;
- add expensive optical flow;
- change recurrent architecture;

unless controlled `learned_only` testing proves the defect exists before the postpass/presentation stages.

If `learned_only` contains the staircase defect, stop and document that result before modifying deeper reconstruction behavior.

---

## 21. Required Deliverables

- [x] Quality Lab typed configuration system.
- [x] Persistent runtime configuration file support.
- [x] Runtime-selectable composition modes.
- [x] Runtime-selectable base filters.
- [x] Detail-residual composition mode.
- [x] Runtime-selectable adaptive sharpening.
- [x] Runtime tone/exposure controls.
- [x] Reconstruction-vs-presentation capture.
- [x] Automated staged parameter sweeps.
- [x] Proper supersampled anti-aliasing test.
- [x] Updated quality benchmark report.
- [x] Updated performance report.
- [x] Before/after comparison page.
- [x] Temporal sequence validation.
- [x] Competitor-control comparison before superiority claims (with mpv FSR1
  unavailable in the installed build and no superiority claim made).
- [x] Concise technical conclusion identifying the actual staircase source.
- [x] Final winning configuration documented in this plan and the persistent
  runtime configuration.

---

# 22. Living Experiment Log

Every completed experiment must add an entry here.

Use this format:

```text
## YYYY-MM-DD — Experiment ID

Hypothesis:
...

Configuration:
...

Corpus subset:
...

Metrics:
...

Visual observations:
...

Performance:
...

Conclusion:
SUPPORTED / REJECTED / INCONCLUSIVE

Decision:
...

## 2026-08-29 — Refined motion was previously disconnected from live decode

The first traced 1280x720 → 3840x2160 refined-motion run exposed a real
integration failure: the runtime selected `codec_refined`, but every frame
reported `seeds=0`, `accepted=0`, and `refined=0`. The decoder only treated
`TFORGE_FSR4_MOTION_ESTIMATOR` as a request for software decode; the capture
runner supplied `TFORGE_FSR4_MOTION_ABLATION=refined` instead. VAAPI therefore
remained enabled and its DRM handoff discarded `AV_FRAME_DATA_MOTION_VECTORS`.

The decoder now applies the same environment precedence as the estimator and
also recognizes `MOTION_ABLATION=refined`, forcing software decode only for
explicit codec/refined motion captures. The normal hardware-decoded path is
unchanged. The motion contract test and build pass.

The corrected four-frame trace confirms the fix: software decode is selected;
subsequent frames contain 3,704–5,961 accepted codec seeds with confidence
statistics and approximately 0.5–0.9 ms CPU estimator time. The short 4K
diagnostic produced SSIM `0.930108`; it is not a quality decision. A matched
36-warmup/24-scored A/B is required next.

Next action:
...
```

Do not erase failed experiments.

They are part of the evidence trail.

## 2026-08-21 — B0 Baseline capture

Hypothesis:

The current output path is the control against which the postpass and
presentation experiments must be measured.

Configuration:

`TFORGE_BENCHMARK_PRESET=Quality`, isolated `XDG_CONFIG_HOME`, persisted
settings with `backend=Fsr4ReExperimental`, `preset=Quality`,
`presentationScaler=Bicubic`, neutral brightness/contrast/gamma, and no
Quality Lab overrides. Frame 48. Output was dumped before the player process
was terminated.

Corpus subset:

High-quality 426x240 rows for Tears of Steel daylight/debris, Sintel
rooftop/cave, synthetic edges/text, synthetic motion, and synthetic dark.
Four representative timing logs additionally cover Sintel rooftop at 426x240
and 640x360, synthetic edges/text at 426x240, and synthetic dark at 426x240.

Metrics:

Seven quality rows were captured. Mean FSR PSNR/SSIM/edge-SSIM was
27.5954 dB / 0.744904 / 0.815945. Lanczos control SSIM was 0.827699 and
bicubic control SSIM was 0.832742. All output files are preserved under
`/tmp/tforge-quality-baseline-20260821/` and the unique tagged corpus frame
directory.

Visual observations:

The synthetic edge frame shows visibly stepped diagonals and softer text than
the reference. This is a valid baseline observation, not yet proof that the
postpass is the source.

Performance:

Warmed GPU mean was 1.129-1.147 ms, p50 1.091-1.100 ms, and p95
1.660-1.664 ms. Pipeline CPU mean was 0.131-0.156 ms. Build and test status:
9 enabled tests passed, 1 skipped, 3 disabled.

Conclusion:

SUPPORTED as a control capture. The baseline defect is reproduced and its
artifact/performance envelope is recorded. No causal stage has been selected.

Decision:

Preserve this state and proceed to the runtime Quality Lab configuration and
Stage A isolation experiments. Do not compare later candidates against an
unrecorded or rebuilt baseline.

Next action:

Trace the existing postpass resource bindings and output-dump/presentation
boundary, then add typed runtime configuration without changing the default
path.

## 2026-08-21 — A1 current control rerun

Hypothesis:

The current output path remains a reproducible control after introducing the
runtime Quality Lab plumbing.

Configuration:

`benchmarks/quality_sweeps/stage_a/current.json`, which disables the Quality
Lab and selects the current path. Quality preset, frame 48, isolated
`XDG_CONFIG_HOME`, and 426x240 output to 1920x1080.

Corpus subset:

`tos_daylight`, `tos_debris`, `sintel_rooftop`, `sintel_cave`,
`synthetic_edges_text`, `synthetic_motion`, and `synthetic_dark`, all high
quality 426x240 clips.

Metrics:

Seven rows; mean FSR PSNR/SSIM/edge-SSIM 27.607751 dB / 0.745318 / 0.816074.
The small difference from B0 is treated as warmed-run variance, not a gain.

Visual observations:

The synthetic edge/text frame retains the stepped diagonal and soft text seen
in B0.

Performance:

The runner recorded mean GPU 1.703 ms and pipeline CPU 1.824 ms across the
seven tagged logs. These are noisy relative to the dedicated B0 timing logs;
the control remains valid because the image path is unchanged.

Conclusion:

SUPPORTED as a reproducibility control, not as an improvement.

Decision:

Preserve B0 and this tagged rerun; use neither as an unmeasured replacement
for the other.

Next action:

Compare pure stable-base spatial filters.

## 2026-08-21 — A2 pure stable base

Hypothesis:

The staircase and softness are primarily caused by the learned/stable postpass
composition, so a stable source base without learned color will improve the
frame.

Configuration:

`benchmarks/quality_sweeps/stage_a/base_only_bilinear.json`: Quality Lab
enabled, `composition.mode=base_only`, `baseFilter.mode=bilinear`, neutral
tone, and no sharpening. Exact copied config and experiment metadata are in
`/tmp/tforge-quality-stageA-20260821/runner/stageA-20260821T204600Z/base_only_bilinear/`.

Corpus subset:

The same seven high-quality 426x240 clips at frame 48.

Metrics:

Mean FSR PSNR/SSIM/edge-SSIM 27.865129 dB / 0.750403 / 0.819522 across seven
rows. Lanczos and bicubic controls were unchanged at mean SSIM 0.827699 and
0.832742.

Visual observations:

The synthetic edge/text output is cleaner than pure learned/direct-blend
outputs and avoids their colored edge fringe, but remains softer than the
native reference. Thin diagonals remain visibly stepped.

Performance:

Mean GPU 1.476 ms and pipeline CPU 1.593 ms.

Conclusion:

SUPPORTED. The stable base is the strongest Stage A composition and supports
continuing with spatial-filter experiments.

Decision:

Do not change the model graph. Keep base-only as the current Stage B control
and test better base kernels.

Next action:

Run Mitchell, Catmull-Rom, and Lanczos2 beside the bilinear control.

## 2026-08-21 — A3 pure learned

Hypothesis:

If the learned reconstruction itself is the staircase source, isolating it
should preserve or improve the artifact relative to the stable base.

Configuration:

`benchmarks/quality_sweeps/stage_a/learned_only.json`: Quality Lab enabled,
`composition.mode=learned_only`, neutral tone, and no sharpening.

Corpus subset:

The same seven high-quality 426x240 clips at frame 48.

Metrics:

Mean FSR PSNR/SSIM/edge-SSIM 27.249229 dB / 0.738438 / 0.808600. Mean GPU
time was 1.470 ms.

Visual observations:

The synthetic edge/text still contains stronger colored stepped edges and
weaker text than the native reference. The defect is not removed.

Performance:

Mean GPU 1.470 ms and pipeline CPU 1.608 ms.

Conclusion:

REJECTED as a deeper graph fix at this stage.

Decision:

Do not alter weights, topology, codec motion, recurrent state, or the native
INT8 graph. Continue at the postpass/base stage.

Next action:

Run Stage D adaptive sharpening on the matched `base_only + bilinear` control;
keep tone neutral and residual disabled so the sharpen effect is isolated.

## 2026-08-21 — A4 direct learned-strength sweep

Hypothesis:

A moderate learned contribution may retain useful detail while suppressing
the stable-base staircase.

Configuration:

Six runtime JSON configs: `direct_blend_000.json`, `direct_blend_010.json`,
`direct_blend_025.json`, `direct_blend_050.json`, `direct_blend_075.json`,
and `direct_blend_100.json`. Each uses `composition.mode=direct_blend`, a
Catmull-Rom base, neutral tone, no sharpening, and learned strengths 0.00,
0.10, 0.25, 0.50, 0.75, and 1.00 respectively.

Corpus subset:

The same seven high-quality 426x240 clips at frame 48. The immutable sweep
manifest and outputs are under
`/tmp/tforge-quality-stageA-20260821/runner/stageA-20260821T204600Z/`.

Metrics:

Mean FSR SSIM was 0.745343, 0.745361, 0.745068, 0.743955, 0.741870,
and 0.738435 for strengths 0.00 through 1.00. Mean edge-SSIM was 0.816242
at 0.00 and 0.808619 at 1.00. Full per-clip metrics and rankings are in
`rankings.csv` and each candidate's `quality.csv`.

Visual observations:

The synthetic edge/text still shows the colored/stepped learned structure
becoming more prominent as learned strength rises. Strength 0.00 is the
cleanest of this group; 0.10 is visually indistinguishable at normal viewing
size.

Performance:

Mean GPU times were 1.487, 1.488, 1.492, 1.489, 1.415, and 1.490 ms for
strengths 0.00 through 1.00. No candidate caused a multi-millisecond GPU
regression.

Conclusion:

SUPPORTED for rejecting strong direct learned blending; INCONCLUSIVE between
0.00 and 0.10 because their difference is within run variance.

Decision:

Use stable-base composition for Stage B. Keep direct blend as a runtime
control, but do not pursue strengths above 0.10 without a separately proven
residual formulation.

Next action:

Run the four spatial base filters with neutral tone and no sharpening, then
inspect edge width, ringing, and texture as well as aggregate metrics.

## 2026-08-21 — B1 spatial base-filter sweep

Hypothesis:

A stable base filter with less ringing or staircase emphasis than the current
Catmull-Rom base can improve smoothness while retaining useful edge detail.

Configuration:

Four runtime configs from `benchmarks/quality_sweeps/stage_b_manifest.json`:
`base_only_bilinear`, `base_only_mitchell`, `base_only_catmull_rom`, and
`base_only_lanczos2`. All use `composition.mode=base_only`, neutral tone,
and `sharpen.mode=none`. The sweep runner copied each exact JSON config into
its candidate directory.

Corpus subset:

The same seven high-quality 426x240 clips at frame 48.

Metrics:

Mean FSR PSNR/SSIM/edge-SSIM was 27.865129 / 0.750403 / 0.819522 for
bilinear, 27.836836 / 0.749177 / 0.819374 for Mitchell, 27.610918 /
0.745343 / 0.816242 for Catmull-Rom, and 27.606651 / 0.744997 / 0.816238
for Lanczos2. Mean GPU time was 1.474, 1.545, 1.475, and 1.581 ms in the
same order. Full rows are in
`/tmp/tforge-quality-stageB-20260821/stageB-20260821T205110Z/rankings.csv`.

Visual observations:

On the synthetic edge/text still, bilinear and Mitchell are the cleanest
stable-base outputs. Mitchell restores a little apparent text crispness but
does not provide a metric gain and remains slightly softer than the reference.
Catmull-Rom and Lanczos2 preserve more high-frequency edge structure without
removing the staircase, so their extra sharpness is not useful quality here.

Performance:

All four remain sub-2 ms GPU for the warmed one-pass output. Mitchell and
Lanczos2 add roughly 0.07 ms and 0.11 ms respectively over the bilinear
candidate in this run.

Conclusion:

SUPPORTED for bilinear as the stable-base winner. REJECTED for Mitchell,
Catmull-Rom, and Lanczos2 as the Stage B winner.

Decision:

Carry bilinear into Stage C. Keep all filters runtime-selectable for controls,
but do not promote a sharper filter without residual evidence.

Next action:

Test detail-residual composition with bilinear/Mitchell/Catmull-Rom/Lanczos2
bases, compact low-pass variants, and residual strengths from 0.25 through
1.25.

---

## 2026-08-21 — C1 detail-residual sweep

Hypothesis:

A low-frequency stable base plus a controlled high-frequency residual from the
learned output could restore useful texture without bringing back the
staircase artifact.

Configuration:

`benchmarks/quality_sweeps/stage_c_manifest.json` ran 23 runtime candidates:
bilinear, Mitchell, Catmull-Rom, and Lanczos2 bases; box3x3 and gaussian3x3
low-pass modes; and residual strengths from 0.25 through 1.25. Tone and
sharpening were neutral/disabled. The exact generated configs are retained in
each candidate directory under
`/tmp/tforge-quality-stageC-20260821/stageC-20260821T205509Z/`.

Corpus subset:

`tos_daylight`, `sintel_rooftop`, and `synthetic_edges_text`, all at frame 48
with 426x240 input and 1920x1080 output.

Measurements:

All 23 candidates exited successfully and produced three finite rows. The
numeric winner was `bilinear_gaussian_025` at mean FSR PSNR/SSIM/edge-SSIM
22.631703 / 0.705373 / 0.701233. The same-clip `base_only_bilinear` control
was 22.664905 / 0.706354 / 0.702011, so the residual was worse by
0.033202 dB / 0.000981 / 0.000778. The residual winner's measured timing
sample was GPU 2.889 ms and pipeline CPU 3.001 ms. The sweep exposed timing
summary lines in 11 of 69 per-clip logs; missing rows remain available for
follow-up verification.

Visual observations:

At strength 0.25 the synthetic edge/text output was nearly unchanged from
the stable base and did not recover reference text detail. Higher strengths
made the learned high-frequency structure more visible without removing the
staircase, and did not produce a defensible quality gain. No residual level
was promoted.

Conclusion:

REJECTED as the promoted composition. The evidence points to postpass
composition/filtering rather than a proven defect in the native INT8 graph;
the native graph, model topology, codec-motion path, and optical-flow scope
remain unchanged.

Decision:

Retain `detail_residual` as a runtime control and preserve all artifacts, but
carry only `base_only + bilinear` into Stage D. Isolate adaptive sharpening
next with neutral tone and no residual.

---

## 2026-08-21 — D1 adaptive-sharpen screen

Hypothesis:

A restrained post-reconstruction edge-adaptive operator could restore some
soft detail after the bilinear base without reintroducing the staircase,
halos, or compression noise.

Configuration:

`benchmarks/quality_sweeps/stage_d_manifest.json` used `base_only + bilinear`,
disabled residual and neutral tone, then tested adaptive strengths 0.10,
0.20, 0.30, 0.40, and 0.50 with limit 0.125 / threshold 0.05. It also tested
strength 0.20 with limits 0.0625 and 0.25, and thresholds 0.025 and 0.10.
The zero-strength candidate used `sharpen.mode=none`.

Corpus subset:

`tos_daylight`, `sintel_rooftop`, and `synthetic_edges_text`, frame 48,
426x240 input to 1920x1080 output.

Measurements:

All ten candidates returned exit code 0 with three rows. The zero-sharpen
control was 22.664905 / 0.706354 / 0.702011 mean FSR PSNR/SSIM/edge-SSIM.
`adaptive_s020` had the highest edge-SSIM at 0.702023, but lower PSNR/SSIM;
`adaptive_s020_threshold100` was effectively tied on SSIM and still below
the control in PSNR. GPU time rose from 1.477 ms for the control to roughly
1.628–1.639 ms for adaptive candidates.

Visual observations:

The stills for text, thin diagonals, and the Sintel scene were materially
unchanged at the tested strengths. No strong halos or overshoot were visible,
but the lack of recovered detail means the small edge-SSIM movement is not a
quality win. The stronger settings were retained as rejected artifacts.

Conclusion:

REJECTED as a promoted sharpen setting. The stable-base output remains the
best controlled spatial path so far; adaptive sharpening did not solve its
softness at an acceptable evidence-backed tradeoff.

Decision:

Carry `sharpen.mode=none` into Stage E. Keep adaptive sharpening available
for a later neighborhood check around any tone/presentation finalist.

---

## 2026-08-21 — E1/E2/E3 tone diagnosis

Hypothesis:

The remaining reference discrepancy may be a low-frequency transfer or
exposure error rather than a spatial reconstruction defect. A corrective tone
choice must improve luminance agreement without winning only by making the
frame aesthetically darker or brighter.

Configuration:

All candidates used `composition.mode=base_only`, `baseFilter.mode=bilinear`,
no residual, `sharpen.mode=none`, and the runtime tone controls from
`benchmarks/quality_sweeps/stage_e/tone_template.json`. The wide exposure
screen used `-0.50`, `-0.25`, `0.00`, `+0.25`, and `+0.50 EV`; the contrast /
gamma screen used contrast `-0.10`, `0.00`, `+0.10` and gamma `0.90`, `1.00`,
`1.10`; the final neighborhood used exposure `-0.010`, `-0.005`, `0.000`,
`+0.005`, and `+0.010 EV`.

Corpus subset:

`tos_daylight`, `sintel_rooftop`, and `synthetic_edges_text`, all frame 48,
426x240 input to 1920x1080 output. The exact immutable runs are
`/tmp/tforge-quality-stageE1b-20260821/stageE1b-20260821T210935Z/`,
`/tmp/tforge-quality-stageE23-20260821/stageE23-20260821T211055Z/`,
`/tmp/tforge-quality-stageErefine-20260821/stageErefine-20260821T211254Z/`,
and `/tmp/tforge-quality-stageEfine-20260821/stageEfine-20260821T211407Z/`.

Metrics:

The final `-0.010 EV` candidate was best on the three-clip mean PSNR,
SSIM, edge-SSIM, and low-frequency luma MAE: `22.675192 / 0.706577 /
0.702106`, MAE `0.019838`, bias `+0.002084`. Neutral was
`22.664905 / 0.706354 / 0.702011`, MAE `0.020485`, bias `+0.003250`.
The `-0.50 EV` wide-screen SSIM result was rejected because its low-frequency
MAE was `0.053124` and bias `-0.046132`. Gamma `0.90` was rejected as an
aesthetic tone tradeoff despite SSIM `0.737135`, because its MAE was
`0.027212` versus neutral `0.020485`. Contrast changes were materially worse.

Visual observations:

The `-0.010 EV` still is only slightly closer in overall brightness to the
reference. It does not recover missing fine detail or remove the remaining
softness, so tone is not being used to hide the spatial defect. No new halo,
ringing, or color-fringe behavior was introduced.

Performance:

The fine sweep stayed within run variance of the stable base: `-0.010 EV`
measured approximately `1.477 ms` GPU and `1.597 ms` pipeline CPU. Tone is a
small part of the existing postpass and caused no multi-millisecond regression.

Conclusion:

SUPPORTED for a small corrective exposure compensation. REJECTED for the
large exposure, contrast, and gamma changes as corrective fixes. The tone
discrepancy is bounded but not the source of the softness.

Decision:

Carry `tone.exposureEV=-0.010`, neutral contrast, pivot `0.5`, and gamma
`1.0` into Stage F. Keep every rejected tone value runtime-selectable and
preserve the exact sweep artifacts.

Next action:

Isolate the Qt/presentation scaler with a forced display size and separate
presented-image capture, then run the local neighborhood and temporal gates.

## 2026-08-21 — P1 presentation-path isolation

Hypothesis:

The presentation filter setting was being parsed but was not a reliable
quality control. In the normal window path, the FSR output and display target
were also coupled, so the presentation scaler could be skipped entirely. A
separate capture and a fixed neural target are required before judging the
display filter.

Implementation:

`GpuImageUploader::readbackPresentation()` now captures the post-presentation
image separately from the native reconstruction. `PlaybackEngine` maps the
Quality Lab presentation enum into the GPU scaler, keeps the neural target
fixed across ordinary window resizes, and fits only the presentation target
to the video aspect. The corrected EASU shader now has distinct nearest,
bilinear, bicubic, and Lanczos branches; the previous nearest branch
accidentally fell through to bicubic and is retained only as a rejected bug
artifact.

Configuration:

The four runtime controls are preserved in
`benchmarks/quality_sweeps/stage_f/presentation_nearest.json`,
`presentation_linear.json`, `presentation_bicubic.json`, and
`presentation_lanczos.json`. The comparison used `base_only`, bilinear base,
disabled residual and sharpening, and corrective exposure `-0.010 EV`.
The source was the high-quality 426x240 corpus clip at frame 48, with the
neural image fixed at 1920x1080 and a 1278x720 fitted presentation target.

Measurements:

The first corrected GPU capture proved both boundaries independently: native
output was 1920x1080 and presented output was 1278x720. Warm presentation
CPU/wait time was approximately `0.107 ms`; the neural path remained in the
observed `0.98–1.48 ms` GPU range. On the three-frame scene screen, the
presentation filters were close and scene-dependent. Sintel rooftop gave
linear/bicubic/Lanczos PSNR `21.704861 / 21.681458 / 21.680964` and SSIM
`0.616545 / 0.615470 / 0.615537`. The additional Tears of Steel daylight
scene gave PSNR `23.944586 / 23.915145 / 23.914281` and SSIM
`0.710583 / 0.709528 / 0.709488`; synthetic edges/text gave PSNR
`23.263971 / 23.255517 / 23.254748` and luma edge-SSIM
`0.866993 / 0.867689 / 0.867770` for the same order. No filter materially
restored the missing fine detail; all remain presentation-only controls.

Conclusion:

SUPPORTED for the separation and runtime wiring. REJECTED as evidence for a
universal presentation-filter winner: linear leads the natural Tears of
Steel scenes, while Lanczos is marginally best on the synthetic edge screen,
and differences are below the scale of the larger reconstruction softness.
The presentation path is no longer a hidden variable. Final default choice
remains open until the broader corpus, local-neighborhood, and temporal gates.

Next action:

Run the Stage F local neighborhood around the stable reconstruction and tone
control, then validate presentation and reconstruction over multi-frame
motion/cut sequences. Keep the presentation filter runtime-selectable while
the broader evidence is collected.

---

## 2026-08-21 — F1 post-jitter-fix local neighborhood

Hypothesis:

Once the base-only path stops sampling the non-accumulated Halton jitter, the
tone finalist and nearby sharpen settings can be compared without temporal
shimmer contaminating the spatial ranking.

Configuration:

`benchmarks/quality_sweeps/stage_f_local_manifest.json` tested tone values
`-0.015`, `-0.010`, and `-0.005 EV`, plus `-0.010 EV` with adaptive sharpen
strengths `0.05` and `0.10`. All candidates used base-only composition,
bilinear base, residual disabled, and the same Quality preset. The exact run
is `/tmp/tforge-quality-stageF-local-fixed-20260821/stageF-local-fixed-20260821T220253Z/`.

Corpus subset:

Tears of Steel daylight, Sintel rooftop, and synthetic edges/text at 426x240,
frame 48, output 1920x1080.

Metrics:

The `-0.015 EV`, no-sharpen candidate ranked first at mean
PSNR/SSIM/edge-SSIM `24.101126 / 0.737582 / 0.713305`, luma MAE `0.014732`,
and signed bias `+0.001477`. GPU mean was `1.425 ms` and pipeline CPU mean
`1.539 ms`. The `-0.010 EV` neighbor measured `24.096957 / 0.737503 /
0.713210`; sharpen candidates were effectively tied or lower and cost about
`0.18 ms` more GPU.

Visual observations:

The post-fix frame strips no longer show the alternating shimmer seen in the
pre-fix local run. The stable path remains soft relative to the native
reference, but sharper kernels and adaptive sharpen did not restore detail
without worsening the controlled score or adding cost.

Conclusion:

SUPPORTED as the local winner. REJECTED for adaptive sharpen promotion. The
small `-0.015 EV` choice is carried into the broader corpus audit.

Next action:

Run the selected candidate over the available natural/synthetic source set,
resolution tiers, supersampled anti-aliasing reference, temporal sequences,
and saved AMD/Linux controls.

## 2026-08-21 — F2 supersampled anti-aliasing and broad corpus audit

Hypothesis:

The selected postpass should reduce geometric staircase persistence on a
properly antialiased reference and remain acceptable across scene types,
source resolutions, and measured frame-time tiers.

Configuration:

The selected runtime file is `config/quality_lab.json`; the sweep copy is
`stage_f_broad_manifest.json` with base-only/bilinear, neutral sharpen, and
`exposureEV=-0.015`. The anti-aliasing runner uses the 8x master SVG and a
1920x1080 reference. Broad artifacts are under
`/tmp/tforge-quality-stageF-broad-20260821/` and
`/tmp/tforge-supersampled-aa-20260821b/`.

Corpus subset:

Six 426x240 full-reference rows were eligible for the broad aggregate:
Tears of Steel daylight/debris, Sintel rooftop/cave, synthetic edges/text,
and synthetic dark. Additional quality runs covered 640x360, 854x480, and
1280x720 source tiers. Dedicated performance traces cover all four tiers.

Metrics:

The six-row broad mean was PSNR/SSIM/edge-SSIM `28.342274 / 0.751456 /
0.797694`, luma MAE `0.012008`, and bias `+0.002138`; quality-capture GPU and
pipeline means were `1.429 ms` and `1.546 ms`. Dedicated GPU means were
`0.971`, `0.966`, `3.985`, and `3.994 ms` for 426x240, 640x360, 854x480,
and 1280x720 input respectively. The supersampled candidate measured
PSNR/SSIM/edge-SSIM `19.604901 / 0.786898 / 0.734911`; edge-SSIM exceeded
Lanczos by `0.019074`, while bicubic and Lanczos retained higher overall SSIM.

Visual observations:

The supersampled master is cleanly antialiased. The low-resolution candidate
has softer diagonals but no severe discontinuous staircase or obvious halo;
this supports keeping bilinear rather than promoting a ringing kernel.

Conclusion:

SUPPORTED for the selected runtime configuration and performance gate.
The result is not a universal spatial-control win, so no public superiority
claim is made.

Next action:

Complete the multi-frame and competitor-control gates, then update the report
and best-known configuration.

## 2026-08-21 — F3 temporal stability and direct-control audit

Hypothesis:

Frame-to-frame instability should be evaluated against a direct bilinear
control so normal spatial low-pass behavior is not misclassified as shimmer.

Configuration:

`run_temporal_quality.sh` now emits FSR, reference, Lanczos, and direct
bilinear frame-delta means and preserves all sequences when
`TFORGE_TEMPORAL_ARTIFACT_DIR` is set. The post-fix runs are under
`/tmp/tforge-temporal-stageF-postfix-v2-20260821/` and
`/tmp/tforge-temporal-stageF-dynamic-v2-20260821/`.

Corpus subset:

Tears of Steel daylight and Sintel natural motion, plus a newly generated
moving synthetic control whose frame hashes were verified to change. The
existing `synthetic_motion` corpus sequence was excluded after its generator
was found to evaluate its drawbox expression only once.

Metrics:

FSR-vs-bilinear temporal-delta absolute error was `0.005165` for Tears of
Steel, `0.291354` for Sintel, and `0.014183` for the moving control. The FSR
sequence SSIM means were `0.850707`, `0.709049`, and `0.996468`; Lanczos
means were `0.855568`, `0.791811`, and `0.995892` respectively. The larger
Sintel gap is spatial softness relative to Lanczos, not evidence of added
temporal oscillation; the direct-bilinear control is much closer.

Visual observations:

The saved ToS and moving-control strips show stable edges and no visible
ghost trails. The pre-fix shimmer disappeared after removing jitter from the
base-only source coordinate and propagating the runtime jitter setting to the
side-buffer synthesizer.

Conclusion:

SUPPORTED for temporal stability of the selected causal spatial path. No
future-frame or optical-flow architecture change is justified by the evidence;
learned/recurrent modes retain their existing jittered behavior.

Next action:

Record the competitor controls, update the benchmark report, rerun build/tests,
and close the quality campaign with the documented runtime configuration.

## 2026-08-21 — F4 AMD/Linux control comparison

Hypothesis:

The selected path must be compared against strong local spatial controls
before any relative-quality statement is made.

Configuration:

The same Tears of Steel 426x240 frame 48 source and 1920x1080 output were
captured with Temporal Forge, mpv/libplacebo EWA, mpv FSRCNNX, and native
bicubic. Captures and exact command artifacts are under
`/tmp/tforge-competitors-20260821/`.

Metrics:

Temporal Forge/libplacebo EWA/FSRCNNX/bicubic SSIM was
`0.759343 / 0.766153 / 0.763241 / 0.767033`; edge-SSIM was
`0.616418 / 0.611988 / 0.609697 / 0.612062`. The installed mpv build had no
usable FSR1 scaler option, so FSR1 was recorded as unavailable rather than
replaced by a misleading substitute.

Conclusion:

The controls are materially different and the results are mixed. This is
enough to satisfy the local comparison gate, but not to support a universal
superiority claim.

Next action:

Freeze the evidence, leave the presentation filter runtime-selectable, and
use the post-jitter-fix configuration as the documented quality default.

## 2026-08-21 — F5 promoted-config runtime verification

Hypothesis:

The documented winning configuration must work when loaded persistently by the
rebuilt player, not only when injected by a sweep override.

Configuration:

`config/quality_lab.json` is enabled with `base_only`, bilinear base,
residual/sharpen disabled, `exposureEV=-0.015`, neutral contrast/pivot/gamma,
and bicubic presentation. The rebuilt `build-fast/temporal_forge_player` was
run through `run_quality.sh` with no Quality Lab override.

Corpus subset:

Tears of Steel daylight, high-quality 426x240 input, frame 48, 1920x1080
output. The exact CSV is `/tmp/tforge-post-campaign-final.csv`; the player log
and captured still are tagged `post-campaign-final` under
`benchmarks/video_corpus/results/`.

Metrics:

The run logged `enabled=true composition=base_only base=bilinear sharpen=none
toneEV=-0.015`, produced the expected 1920x1080 output, and measured PSNR
`25.597192`, SSIM `0.759343`, edge-SSIM `0.616418`, and low-frequency luma
MAE `0.017579`.

Conclusion:

SUPPORTED. The persistent default path launches, produces a complete image,
and matches the intended runtime configuration. Final build and CTest gates
also pass: 10 enabled tests passed, one optional test skipped, and three
existing GPU/harness tests remained disabled by configuration.

Next action:

No further quality code changes are justified by the completed evidence. Keep
the review HTML and benchmark artifacts available for handoff.

## 2026-08-29 — M6-EV1 candidate-linked cave event capture

Hypothesis:

The M6 event gap can be closed at the evidence boundary by retaining an
authoritative runtime event trace and its threshold provenance beside a
candidate-linked real-corpus sequence, without deriving the event from residuals,
stills, or the motion `reset` field.

Configuration:

`base_only_bilinear`, Sintel cave, captured frames 42–59, 426x240 → 1920x1080.
The real corpus frames were not synthesized; a 500 ms PTS offset was applied from
captured frame 9 onward so the runtime PTS-gap detector saw a known event. The
exact commands and runtime environment are retained in
`benchmarks/video_corpus/results/m6_event_sintel_cave_20260829T060839Z/capture_commands.txt`.

Runtime evidence:

The authoritative event is captured frame 9, transition index 8, PTS 875000 us.
The PTS delta is 541.666992 ms against an expected interval of 41.6667633 ms,
triggering only `pts_gap` under the runtime rule
`ptsGapMs > expectedFrameIntervalMs * 2.5`. Histogram delta 0 and motion
confidence 0.960797012 did not trigger their detector thresholds. The raw runtime
label remains `detector_scene_cut`; classification does not overwrite it. Ghost
and reset metric thresholds are explicitly 0.02 from the capture arguments.

Metrics:

`static_flicker=0.000403991`, `edge_variance=0.000000538`,
`motion_compensated_error=0.002451593`, `ghost_duration_frames=0`, and
`reset_recovery_frames=1`. The full event validator is
`benchmarks/video_corpus/results/m6_event_sintel_cave_20260829T060839Z/capture_validation.json`.

Validation:

The assembled trace and sidecars passed the official M6 event-matrix field
validators. Focused event, temporal, motion, and runner checks passed with 53
Python tests and three subtests. The configured build-fast CTest suite passed 18
enabled tests; one was skipped and three remained disabled by build
configuration. `git diff --check` passed. The capture wrapper exited 0 and
retained 18 candidate and 18 reference 1920x1080 frames beside the evidence.
As an explicit gate check, `tools/verify_quality_matrix.py` was run against an
intentionally empty combined-matrix stub and exited 2 with
`spatial matrix must be a non-empty list`; the status is retained at
`benchmarks/video_corpus/results/m6_event_sintel_cave_20260829T060839Z/matrix_verifier_gate_status.txt`.

Conclusion:

SUPPORTED for the requested event-slice capture. This is one coverage key and is
not strict-matrix completion; `tools/verify_quality_matrix.py` has not passed the
complete spatial-plus-temporal matrix. No default promotion and no M7 work are
authorized by this evidence.

A supplementary ToS faces-hair-skin capture is retained under
`benchmarks/video_corpus/results/m6_event_tos_faces_hair_skin_20260829T022730Z`.
It also produced a runtime PTS-gap trace at captured frame 9 and measured
`static_flicker=0.008755100`, `edge_variance=0.000029068`,
`motion_compensated_error=0.014858980`, `ghost_duration_frames=0`, and
`reset_recovery_frames=1`. It is not part of the cave-only separate event-matrix
contract and is retained as exploratory evidence only.

Next action:

Capture the remaining four Sintel-cave candidates with the same grounded
trace/provenance procedure, then produce and verify the complete real
candidate × scene × class matrix.

## 2026-08-29 — Motion/jitter ablation correction on Sintel rooftop and ToS debris

Hypothesis:

The previous “zero-motion” Sintel-rooftop control was incomplete because its
default variable Halton jitter remained active. A true no-correspondence
control must remove both motion vectors and synthetic color jitter before the
motion arms can be interpreted.

Configuration:

Sintel rooftop, 1280x720 medium-CRF23 input, 3840x2160 output, 12 frames,
native INT8 graph, current composition, software decode, zero motion-vector
ablation, and explicit `TFORGE_FSR4_JITTER_MODE=off`. The authoritative
capture and exact command are retained under
`benchmarks/video_corpus/results/temporal_sintel_rooftop_4k_zero_mv_zero_jitter_swdecode_20260829/`.
Its environment file records the zero ablation, disabled jitter, forced viewport,
current Quality Lab configuration, software decode, and profiling controls.
Matched codec and refined arms with jitter off were then captured under
`temporal_sintel_rooftop_4k_codec_mv_zero_jitter_20260829` and
`temporal_sintel_rooftop_4k_refined_mv_zero_jitter_20260829`. Each run retained
exactly 12 PPM frames and its exact command/environment provenance.
`validation.txt` in the zero-control directory checks all six frame counts and
environment combinations.

A second complete motion/jitter matrix used the same runner, frame count,
current composition, and 1280x720 → 3840x2160 dimensions on Tears of Steel
debris. Its six artifacts and exact commands are retained under
`temporal_tos_debris_4k_{zero_mv,codec_mv,refined_mv}_{zero_jitter,variable_jitter}_20260829`.
`motion_jitter_ablation_summary.csv` and `validation.txt` beside the zero
control record the paired metrics and environment checks.

Metrics:

| Arm | SSIM mean | SSIM minimum | Temporal absolute error |
|---|---:|---:|---:|
| Zero MV + zero jitter | 0.937722 | 0.934426 | 0.346464 |
| Codec MV + zero jitter | 0.937730 | 0.934426 | 0.347528 |
| Refined MV + zero jitter | 0.937729 | 0.934426 | 0.347482 |
| Zero MV + variable Halton jitter | 0.939435 | 0.916825 | 0.502146 |
| Codec MV + variable Halton jitter | 0.939270 | 0.915637 | 0.504655 |
| Refined MV + variable Halton jitter | 0.939274 | 0.915664 | 0.504618 |

Per-frame SSIM range was 0.005882 with jitter off versus 0.042779–0.044085
with variable jitter. The corrected control's steady native-graph sample was
5.65 ms GPU in this capture; this is one timing sample rather than a new
performance average. A comparison CSV is retained beside the corrected capture
as `motion_jitter_ablation_summary.csv`.

Conclusion:

The earlier interpretation is corrected: the apparent correspondence gain was
confounded with variable synthetic jitter. With jitter off, codec and refined
motion differ from true zero motion by only `+0.000008` and `+0.000007` mean
SSIM respectively, while their temporal errors are marginally worse. Variable
jitter raises mean SSIM slightly but sharply increases frame-to-frame SSIM
variation and temporal-delta error. Codec and refined motion are effectively
equivalent on this scene. No estimator or jitter promotion is justified by this
result. Future promotion decisions require this paired motion/jitter matrix on
additional real-motion scenes; variable-jitter arms must not be compared against
a nominally zero-motion arm that still jitters.

ToS debris results were:

| Arm | SSIM mean | SSIM minimum | Temporal absolute error |
|---|---:|---:|---:|
| Zero MV + zero jitter | 0.983422 | 0.982923 | 0.105120 |
| Codec MV + zero jitter | 0.983447 | 0.982951 | 0.106164 |
| Refined MV + zero jitter | 0.983447 | 0.982951 | 0.106162 |
| Zero MV + variable Halton jitter | 0.983588 | 0.981006 | 0.060997 |
| Codec MV + variable Halton jitter | 0.983552 | 0.980890 | 0.057905 |
| Refined MV + variable Halton jitter | 0.983554 | 0.980898 | 0.058005 |

On ToS debris, variable jitter improves both mean SSIM and temporal-delta error,
unlike Sintel rooftop. Motion with variable jitter slightly lowers mean SSIM
but improves temporal error by about `0.003`; codec and refined remain
effectively identical. The two scenes therefore show scene-dependent jitter
behavior and no consistent motion/refinement gain, so neither is promoted from
these 12-frame samples.

---

# 23. Motion/Jitter Isolation Update (2026-08-29)

The previous motion A/B control was not fully isolated. Motion ablation was
applied before `SideBufferSynth::update()`, so clearing the vector list also
changed the confidence passed to scene-cut detection. That could restart the
variable jitter sequence and make the two arms use different temporal phases.

The benchmark boundary now preserves the pre-ablation confidence for the
zero-motion arm while still clearing the uploaded vector payload. This keeps
reset and jitter scheduling aligned between motion arms. Focused motion,
temporal, jitter, and side-buffer contract tests pass.

Matched Sintel rooftop capture, 1280x720 -> 3840x2160, 12 frames, native INT8:

```text
zero motion + zero jitter:
  SSIM 0.937735
  temporal delta absolute error 0.347582

codec-refined motion + zero jitter:
  SSIM 0.937729
  temporal delta absolute error 0.347482
  refinement CPU cost approximately 0.3-0.6 ms per frame
```

Conclusion: the cheap refinement stage is active and inexpensive, but this
scene does not yet show a material quality gain from refinement itself. The
short tested default-jitter run improved SSIM versus jitter-off, but increased
temporal delta error, so jitter remains an experiment rather than a proven
universal default. Longer and more varied motion footage is still required.

## 23.1 Variable-jitter lifecycle correction (2026-08-29)

The matched captures exposed a real integration defect even though the
rendered frames did not show a whole-frame shift: SideBufferSynth::update()
selected the current variable-jitter phase before the FSR model/output pair
for that frame was installed. A scale or viewport change therefore selected
the phase count from the previous pair, then reset the sequence after the
frame had already consumed its sample.

The sizing calculation is now shared by the pre-update setup and dispatch
path. The current model/output pair is installed before update() chooses
the phase. This changes jitter bookkeeping only; it does not change motion
vectors, FSR graph behavior, color processing, or reconstruction parameters.
The source-order contract test explicitly guards this boundary.

Validation:

    cmake --build build-fast -j8                         PASS
    5 focused motion/temporal/jitter tests               PASS
    git diff --check                                     PASS

The existing visual inspection remains applicable: variable jitter changes
fine edges without a visible whole-frame translation, and the retained
Sintel rooftop frames do not show a warm-tone shift. This fix makes future
resolution/scale A/B captures causally comparable; it is not evidence that
variable jitter is a universal quality win.

## 23.2 Controlled post-fix capture (2026-08-29)

The first post-fix rerun was rejected because it used the repository default
Quality Lab configuration instead of the campaign control. Both arms were
then rerun with the exact control configuration: current composition,
Catmull-Rom base, model color space, no sharpening, and tone EV 0.000.

Sintel rooftop, 1280x720 input, 3840x2160 output, 12 frames, native INT8,
codec-refined motion:

| Arm | SSIM mean | SSIM minimum | Temporal absolute error | GPU sample |
|---|---:|---:|---:|---:|
| Refined motion + zero jitter | 0.937729 | 0.934426 | 0.347482 | 8.409 ms |
| Refined motion + variable jitter | 0.939926 | 0.916488 | 0.478100 | 5.699 ms |

The zero-jitter result is identical to the earlier matched control, confirming
that the pair-install change did not alter the zero-jitter path. Variable
jitter gains mean SSIM by 0.002197 relative to zero jitter, but has a lower
worst-frame SSIM and a larger temporal error. Independent visual inspection
found the two arms aligned, with identical warm/amber tone, no visible crop or
double contour, and no clear visible quality advantage for variable jitter in
frames 0000, 0005, or 0011.

Conclusion: the pair-order correction is validated as a lifecycle/integration
fix, but variable jitter remains scene- and metric-dependent and is not
promoted as a universal default. The capture runner now also retains the exact
Quality Lab JSON beside future artifacts so temporary config paths cannot
silently undermine provenance.

## 23.3 Controlled post-fix ToS debris capture (2026-08-29)

The same corrected binary and exact campaign control were tested on the
30-frame Tears of Steel debris sequence at 1280x720 input and 3840x2160
output.

| Arm | SSIM mean | SSIM minimum | Temporal absolute error | GPU sample |
|---|---:|---:|---:|---:|
| Refined motion + zero jitter | 0.982880 | 0.980966 | 0.125245 | 8.177 ms |
| Refined motion + variable jitter | 0.982686 | 0.978358 | 0.091246 | 8.148 ms |

Variable jitter improves temporal-delta agreement by 0.034, but lowers mean
SSIM by 0.000194 and worst-frame SSIM by 0.002608. Independent visual review
found no visible difference in alignment, color, edge detail, ghosting, or
shimmer in frames 0000, 0015, and 0029.

Conclusion: across the two corrected real-motion scenes, variable jitter is
not a universal spatial-quality win. It can improve temporal agreement on the
debris sequence while reducing spatial scores, and it can raise spatial SSIM
on the rooftop sequence while worsening temporal agreement. Keep it
runtime-selectable and continue investigating the temporal contract; do not
make it the unconditional default.

The cave event control was then recaptured with the variable mode explicitly
recorded rather than relying on the implementation default:

```text
benchmarks/video_corpus/results/m6_event_sintel_cave_current_variable_explicit_20260829T144922/
```

This artifact contains `TFORGE_FSR4_JITTER_MODE=current`, codec-refined motion,
the enabled `current_control.json`, and 18 frames at 1920x1080. Its measured
event values are static flicker `0.000573600`, edge variance `0.000000723`,
motion-compensated error `0.002623443`, ghost duration `0`, and reset recovery
`1` frame. This is the authoritative explicit variable-jitter cave capture;
the otherwise equivalent run whose mode was implicit from the runtime default
is retained only as provenance history.

## 23.4 Deep-research report review (2026-08-29)

`docs/deep-research-report(14).md` was reviewed as external hypothesis input.
Its useful contract checks agree with this implementation: motion supplied to
FSR must be current-to-previous screen-space displacement, motion estimation
must use unjittered decoded frames, and codec vectors are approximate block
seeds rather than renderer ground truth. Its recommendation to use zero or
minimal synthetic jitter is not accepted as a campaign conclusion: the
corrected captures already show scene-dependent spatial and temporal tradeoffs.
Keep variable jitter runtime-selectable and compare it directly against zero
jitter on the same frames/configuration before changing the default.

The motion-reference filter was also tightened after a test-first audit:
`pastReferenceMotion()` now accepts only FFmpeg `source == -1`. Ambiguous
`source == 0`, older past references such as `-2`, and positive future
references fail closed because the causal player owns only the immediately
previous history image. The real captured corpus contained only `source=-1`,
so this does not discard any observed usable vectors. The new regression test
failed against `source <= 0`, then passed after the narrow production change.

## 23.5 Transfer-function A/B (2026-08-29)

The FSR input-transfer hypothesis was tested on the matched 18-frame
Sintel-cave current-control sequence at 426x240 input and 1920x1080 output,
with codec-refined motion and jitter explicitly off. Only the input transfer
override changed:

| Arm | SSIM mean | SSIM minimum | Motion-compensated error | Static flicker |
|---|---:|---:|---:|---:|
| Default transfer | 0.970817 | 0.936259 | 0.002386432 | 0.000042868 |
| Explicit Rec.709 | 0.951834 | 0.919241 | 0.002589626 | 0.000059210 |

The Rec.709 arm also produced a visible magenta/purple cast in near-black cave
regions without revealing additional reliable detail. It is rejected as a
quality fix. The default transfer remains the better arm for this corpus;
the input-transfer override remains available only for controlled diagnostics.

Artifacts:

```text
benchmarks/video_corpus/results/m6_event_sintel_cave_current_zero_postref_fix_20260829T154716/
benchmarks/video_corpus/results/m6_event_sintel_cave_current_zero_rec709_postref_fix_20260829T154811/
```

The matched Sintel-cave event A/B was then completed with the same
codec-refined motion, current-control configuration, 18 frames, and
1920x1080 output. Only jitter mode changed:

| Arm | Static flicker | Edge variance | Motion-compensated error | Reset recovery |
|---|---:|---:|---:|---:|
| Variable jitter (`current`) | 0.000573600 | 0.000000723 | 0.002623443 | 1 frame |
| Zero jitter (`off`) | 0.000045359 | 0.000000128 | 0.002387623 | 1 frame |

Artifacts:

```text
benchmarks/video_corpus/results/m6_event_sintel_cave_current_variable_explicit_20260829T144922/
benchmarks/video_corpus/results/m6_event_sintel_cave_current_zero_explicit_20260829T153308/
```

The first result is better on all three measured cave signals, while reset
behavior is unchanged. This is evidence against variable jitter for this
low-light/shadow-detail scene, not yet a global default decision; the rooftop
and debris captures remain scene-dependent.

Artifacts:

```text
benchmarks/video_corpus/results/temporal_sintel_rooftop_4k_zero_motion_zero_jitter_fixed_20260829/quality.csv
benchmarks/video_corpus/results/temporal_sintel_rooftop_4k_refined_zero_jitter_long_20260829/quality.csv
benchmarks/video_corpus/results/temporal_sintel_rooftop_4k_refined_variable_jitter_pairfix_control_20260829/quality.csv
benchmarks/video_corpus/results/temporal_sintel_rooftop_4k_refined_zero_jitter_pairfix_control_20260829/quality.csv
```

## 23.6 — Causal reference and jitter submission lifecycle (2026-08-29)

The dense replay adapter now accepts only `source=-1`, the immediately
previous-reference marker used by the causal player. Ambiguous `source=0`,
older negative references, and future positive references are rejected. Motion
components are also bounded by the replay's declared source dimensions before
conversion to the runtime float representation.

Synthetic jitter is now transactionally staged: `SideBufferSynth::update()`
selects a candidate phase, an in-scope guard rolls it back on wait, abort,
initialization, upload, or dispatch failure, and the existing commit occurs
only after a successful submitted FSR chain. This prevents variable-jitter
phase skips when a decoded frame never reaches FSR. The guard does not alter
motion estimation or FSR dispatch semantics.

Validation:

```text
ctest --test-dir build-fast --output-on-failure -R \
  '^(fsr4_motion_contract_tests|fsr4_temporal_contract_tests|jitter_policy_tests|jitter_tests|sidebuffer_tests|motion_estimator_tests|color_metadata_contract_tests)$'
python3 -m unittest tests/test_motion_sidecar.py tests/test_temporal_runner_contract.py
```

Result: 7/7 CTest targets passed; 41/41 Python tests passed; `git diff --check`
passed. The change is a correctness fix, not a quality promotion. The
variable-jitter cave A/B remains rejected by the measured and visual evidence
already recorded above.

## 23.7 — Validity-aware temporal metrics (2026-08-29)

The temporal metric adapter now honors the per-pixel validity mask attached to
`MotionFieldWithValidity`. Uncovered pixels are excluded from both NumPy and
pure-Python calculations, while plain legacy motion fields remain compatible
and are treated as fully valid. A transition with no valid in-bounds samples
still fails rather than becoming fabricated identity-motion evidence.

This corrects campaign measurement only; it does not change the runtime motion
texture or the FSR reconstruction path. The new contract is covered by
`tests/test_motion_validity_metrics_contract.py`.

Validation: 53 Python tests passed, including the new partial-coverage and
all-invalid transition cases; `git diff --check` passed.

## 23.8 — Post-guard 720p jitter confirmation (2026-08-29)

A fresh matched 12-frame Sintel-cave capture was run after the submission
guard, using the same 1280x720 high-quality input, 3840x2160 output, refined
codec-motion mode, and lossless reference. Only jitter mode differed:

```text
benchmarks/video_corpus/results/m6_event_sintel_cave_jitter_guard_20260829/zero/
benchmarks/video_corpus/results/m6_event_sintel_cave_jitter_guard_20260829/variable/
```

The variable arm measured mean SSIM `0.992667` versus `0.991885` with zero
jitter, and temporal absolute error `0.083655` versus `0.221824`. Both runs
completed with 12 frames and identical output dimensions. This is a guarded
positive on this short cave slice, not a global jitter promotion: earlier
rooftop, debris, and cave captures remain mixed, so the mode stays runtime
selectable pending the full temporal matrix.

## 23.9 — Causal source-policy alignment (2026-08-29)

The motion estimator and Python sidecar validator now use the same strict
source policy as the runtime replay adapter: only `source=-1` is admitted for
the causal current-to-previous path. Ambiguous `source=0` and older past
references such as `source=-2` are rejected before refinement or metric
assembly. This prevents different pipeline stages from interpreting the same
codec vector differently.

The native passthrough upload boundary was also corrected so synthetic jitter
is disabled when no FSR jitter metadata is submitted. Temporal FSR paths retain
the existing matched sampling/metadata behavior.

Validation: source-policy estimator, sidecar, motion-contract, and native
jitter-boundary tests pass. The full CTest suite then passed 19 runnable tests;
one test was skipped and three GPU diagnostics remain disabled by design. The
repository-wide Python discovery run is not green because several older M6
tests reference deleted historical `/tmp` artifacts and missing legacy review
images; those are evidence-retention failures, not failures of this change.

## 23.10 — Known-translation decoder probe (2026-08-29)

The rebuilt player decoded a small H.264 P-frame clip containing a white block
translated 2 source pixels to the right per frame. Its exported, filtered
sidecar is retained at:

```text
benchmarks/video_corpus/results/motion_contract_probe_20260829_run2/player_motion.json
```

In the translated region, the observed X vectors include `-2.0`, matching the
expected current-pixel-to-previous-pixel direction for rightward motion. The
probe also shows block quantization and zero vectors in parts of the simple
scene, so it validates the sign convention only provisionally; it does not
prove dense boundary accuracy or exact FSR behavior. A deterministic vertical
translation and GPU warp-residual check remain required before claiming the
full motion contract empirically validated.

The companion vertical probe is retained at:

```text
benchmarks/video_corpus/results/motion_contract_probe_20260829_run3/player_motion.json
```

It moves a white block 2 source pixels downward per frame. The moving-region
sidecar contains negative Y vectors (including `-2.0`), which is consistent
with current-to-previous sampling toward the block's prior, higher position.
Together the horizontal and vertical probes provisionally confirm both axis
signs through FFmpeg extraction, normalization, filtering, and sidecar export.
They still do not replace a GPU warp-residual test.

## 23.11 — Known-translation GPU dense-field residual (2026-08-29)

The controlled horizontal-translation clip was replayed with the dense GPU
motion and validity textures dumped from the runtime. The dump is the actual
320x180 RG16F field plus an R8 validity texture, not a reconstruction from the
sparse sidecar. Comparing the warped previous frame against a zero-motion
control produced:

```text
frame 1: full 0.656 vs zero 0.633; moving ROI 5.672 vs 5.479
frame 2: full 0.417 vs zero 0.417; moving ROI 3.894 vs 3.894
frame 3: full 0.288 vs zero 0.633; moving ROI 2.490 vs 5.479
```

The field therefore improves the final probe frame but worsens the first and
does not improve the second. This is evidence that extraction/sign handling is
not the only remaining problem: dense-field initialization, coverage, or
boundary reconstruction is still inconsistent on a simple known translation.
No temporal-quality promotion is made from this probe. The raw artifacts are
retained under:

```text
benchmarks/video_corpus/results/motion_contract_probe_20260829_run2/gpu_motion_artifacts/
```

The next motion gate is to isolate seed rasterization/refinement from dense
upsampling and compare each stage against the known translation before using
the field for a broader FSR quality claim.

## 23.12 — Jitter coordinate reset on render-size changes (2026-08-29)

The jitter audit found that a decoded render-size change reset the phase
policy but could leave `hasPreviousJitter` set. That allowed the next frame to
subtract a prior jitter expressed in a different source-pixel coordinate
space. The decode loop now detects a render-width/height change, clears the
stored prior jitter, and includes the change in the existing temporal reset
decision. This is a lifecycle correctness fix; it adds no sampling pass and
does not alter reconstruction algorithms.

Validation: the new failing-first contract assertion now passes in
`fsr4_motion_contract_tests`; the rebuilt player target and `git diff --check`
also pass. Async fence failure rollback remains an open lifecycle audit item.

## 23.13 — Reduced-model motion magnitude contract (2026-08-29)

The FSR audit found that sparse motion coverage was remapped into model
coordinates while `mvX/mvY` remained in decoded source-pixel units. The
prepass subsequently multiplied those values by `output/model`, which
over-scaled motion whenever the neural model was smaller than the decoded
frame. `scaleMotionCoverageToModel()` now scales vector magnitudes by the same
source-to-model factors as block positions and extents. The prepass remains
unchanged and still performs the model-to-output conversion exactly once.

This affects only temporal motion input preparation; no FSR shader, model, or
reconstruction algorithm was changed. The new contract is covered by the
failing-first motion test and the rebuilt target. A reduced-model real-video
capture is still required before claiming a quality improvement.

## 23.14 — Reduced-model runtime validation (2026-08-29)

A four-frame real Sintel-cave capture was run with a 1280x720 decoded input,
forced 4x scale, zero jitter, and a 3840x2160 target. The active runtime
remained at `960x540 -> 3840x2160` after initialization; it no longer rebuilt
the uploader back to `1280x720` on each frame. The capture completed with four
complete output frames and measured FSR SSIM `0.995105` versus Lanczos
`0.993943` on this short slice. The output's temporal absolute-error metric
was `0.049226` against the reference, versus `0.187593` for Lanczos.

Artifact:

```text
benchmarks/video_corpus/results/m6_reduced_model_motion_contract_20260829_final/
```

This validates the reduced-model resource wiring and provides a positive
slice, but it is not a broad quality promotion; the dense boundary-motion
gate and async-failure lifecycle gate remain open.

## 23.15 — GPU/CPU and aligned-block motion probes (2026-08-29)

The known horizontal probe was captured twice with the actual dense motion
texture dumped: once through GPU expansion and once through the explicit CPU
motion path. GPU and CPU behaved differently, confirming that the remaining
error is not explained by a global sign alone. GPU residuals versus zero-motion
for frames 1/2/3 were `0.655/0.633`, `0.417/0.417`, and `0.288/0.633`;
the CPU path was `0.417/0.417`, `0.417/0.417`, and `0.413/0.633`.

An aligned-block P-frame control was also created and its frame types were
verified as `I P P P`. The dense field covered only half of the moving
rectangle in each measured frame, so its apparent zero residual is not a
valid full-object success; the uncovered boundary pixels were excluded by
validity. This confirms the next fix must address codec-block coverage and
boundary confidence before dense motion can be promoted. No boundary rule was
changed from this probe.

Artifacts:

```text
benchmarks/video_corpus/results/motion_contract_probe_20260829_run5_cpu_motion/
benchmarks/video_corpus/results/motion_contract_probe_20260829_aligned_blocks_pframes/
```

## 23.16 — Dense replay source-dimension guard (2026-08-29)

The replay adapter previously validated the dimensions declared by a motion
sidecar, but did not compare them with the decoded frame dimensions supplied at
lookup time. A sidecar captured at another input resolution could therefore be
accepted and applied to a different source frame. The loader now stores the
validated sidecar dimensions and rejects the replay whenever either dimension
differs from the actual decoded source. The rejection is fail-closed and leaves
the caller on its existing no-replay path.

Validation was written first as a failing assertion in
`fsr4_motion_contract_tests`, then the loader was changed to pass it. The
focused contract test, rebuilt player, full CTest suite (19 runnable tests; one
skipped and three disabled GPU diagnostics), relevant Python tests (54/54), and
`git diff --check` all pass. This closes a data-identity defect but does not
resolve the separate dense block-boundary coverage problem.

## 23.17 — Async synthetic-jitter commit boundary (2026-08-29)

The async two-slot path returned success at queue submission, while the jitter
transaction and `previousJitterX/Y` state were committed before the GPU fence
was known to have completed. A later asynchronous fence failure could therefore
consume a Halton phase and leave the next FSR frame paired with stale jitter
metadata. The existing rollback guard could not repair that case because it had
already been marked committed.

Synthetic-jitter frames now opt out of async slot overlap and use the existing
blocking dispatch path. This keeps the color sample, jitter metadata, previous
jitter state, and commit point inside one completed submission boundary. It is a
small lifecycle guard, not a reconstruction or shader change. Async overlap is
still available for paths that do not synthesize jitter; the longer-term
per-in-flight-slot transaction remains a future optimization if needed.

Validation was written first in
`tests/test_temporal_async_state_contract.py`, then the `asyncSlots` predicate
was changed to require `!syntheticJitterApplied`. The contract test passes,
followed by the rebuilt player, full CTest (19 runnable tests; one skipped and
three disabled GPU diagnostics), 55 relevant Python tests, and
`git diff --check`.

## 23.18 — Raw FFmpeg seed audit for the aligned-block fixture (2026-08-29)

The raw FFmpeg side-data diagnostic was extended to print non-zero vectors
instead of only the first twelve entries. On the valid `known_pan.mp4` probe,
FFmpeg exports `source=-1` vectors with `motion_scale=4`, raw `motion_x=-8`,
and derived displacement `-2.0` pixels, matching the controlled rightward
translation. This confirms the extraction and the single source-to-pixel
conversion for that fixture.

The later `aligned_pan.mp4` fixture is not a valid motion-seed control: all 236
P-frame vectors are zero even though the decoded white block moves between
frames. Because the encoder did not emit motion for that synthetic clip, its
later `-8` dense-field observation cannot identify an expansion or boundary
ownership defect. It remains useful only as a warning that visual movement in
decoded frames does not guarantee codec motion metadata. No production motion
or shader behavior was changed from this audit.

The diagnostic source is `tools/inspect_ffmpeg_mvs.cpp`; it is compiled and
run against both fixtures as part of the investigation. The valid known-pan
probe remains the extraction-direction control, while a new boundary test must
use a fixture whose raw side data contains non-zero vectors before ownership or
coverage changes are justified.

## 23.19 — Variable-jitter timing audit (2026-08-29)

The default jitter is intentionally variable: `JitterSequence::Halton23`,
cadence one, and a resolution-dependent phase count. The phase advances only
for a dispatched FSR frame, resets on seek/cut/timestamp discontinuity and
source-size changes, and rolls back when dispatch does not complete. This is
not a random timing source or a per-pixel quality operation.

The current timing cost comes from the correctness guard around that state. A
nonzero synthetic-jitter frame uses the blocking dispatch path so color
sampling, reported jitter, prior-jitter metadata, and phase commit share one
completed submission boundary. Existing 4K logs measure approximately 8.38 ms
GPU and 8.58 ms CPU wait for variable jitter, with uploads around 0.28 ms and
motion refinement around 0.3–0.66 ms. The matched jitter-off run is also about
8.41 ms GPU, so generating Halton samples is not the cost; waiting for the
temporal dispatch is.

One multi-pass lifecycle defect remains: `previousJitterX/Y` is published after
the first pass while the transaction is committed only after the full chain.
That path must be repaired before restoring overlap for any multi-pass temporal
configuration. No performance relaxation is promoted from this audit.

## 23.20 — Pre-upload seed attribution (2026-08-29)

The new gated pre-upload trace was exercised on the known two-pixel horizontal
translation. With `TFORGE_FSR4_MOTION_ESTIMATOR=codec` and
`TFORGE_FSR4_DISABLE_BEST_FINDINGS=1`, the runtime uploads only the expected
`-2` source-pixel vectors. With the ordinary best-findings refinement enabled,
the runtime uploads additional `-4` and `+2` vectors. With
`TFORGE_FSR4_MOTION_ESTIMATOR=codec_refined` and best-findings disabled, the
same alternate values are produced by the standalone estimator.

This isolates the regression to local refinement, not FFmpeg extraction,
synthetic jitter, source/model scaling, or the GPU expander. The likely
mechanism is that a quarter-resolution integer search cannot represent a
two-source-pixel seed exactly, then accepts a neighboring four-source-pixel
correction from a small patch. That mechanism still needs a failing-first unit
fixture and a matched real-scene quality check before changing the acceptance
rule. The raw-codec path remains the trustworthy control.

Artifacts:

```text
benchmarks/video_corpus/results/motion_contract_probe_20260829_run7_seed_trace/
benchmarks/video_corpus/results/motion_contract_probe_20260829_run8_codec_seed_trace/
benchmarks/video_corpus/results/motion_contract_probe_20260829_run9_raw_codec_seed_trace/
benchmarks/video_corpus/results/motion_contract_probe_20260829_run10_estimator_refine_seed_trace/
```

## 23.21 — Conservative refinement correction bound (2026-08-29)

The failing-first estimator test exposed that a quarter-resolution integer
search could replace a valid codec seed with a multi-source-pixel jump. The
controlled known-pan trace then confirmed the provenance: raw codec mode
uploaded the expected `-2` source-pixel vectors, while both refinement entry
points could inject `-4` or `+2` values.

Both refinement implementations now accept a correction only when its length
in source-pixel units is within `TFORGE_FSR4_MOTION_MAX_CORRECTION`, whose
default is `1.0`. An oversized apparent improvement preserves the decoder seed
and reduces confidence instead of promoting the reduced-grid result. This is a
conservative correctness guard; it is not yet a universal quality promotion.

The guarded known-pan capture uploaded only the expected `-2` vectors across
the traced frames. The runner also now clears inherited
`TFORGE_FSR4_DISABLE_BEST_FINDINGS` state before constructing the child
environment, then records an explicitly requested value. This prevents raw,
refined, and best-findings arms from being silently mixed by the caller's
shell. A separate source-policy correction makes the legacy refiner admit only
the documented immediate-previous marker, `source=-1`.

Validation:

```text
build-fast motion_estimator_tests, sidebuffer_tests, and
fsr4_motion_contract_tests: passed
CTest: 19 passed, 1 skipped, 3 disabled
Python focused contracts: 55 passed
Known-pan guarded trace:
benchmarks/video_corpus/results/motion_contract_probe_20260829_run11_guarded_refined_seed_trace/
```

Remaining gate: compare the guarded refinement behavior on representative
real-world motion footage before deciding whether refinement should remain in
the promoted temporal path. The raw codec path remains the control.

## 23.22 — Explicit global-fallback provenance (2026-08-29)

The failing-first motion-estimator test also exposed an ambiguity in the
empty-codec-seed path: the luma-derived global fallback was tagged
`source=0`, even though it was not copied from a codec reference-picture
entry. That marker could let a downstream adapter treat a weak internally
estimated vector as an unverified codec vector.

The fallback now uses the same explicit immediate-previous-frame marker as
the causal motion contract, `source=-1`, while retaining its confidence cap
of `0.5`. This changes provenance only; displacement calculation, fallback
search, FSR units, and reconstruction behavior are unchanged.

Validation:

```text
motion_estimator_tests: passed, including the source=-1 fallback assertion
CTest: 19 passed, 1 skipped, 3 disabled
git diff --check: passed
```

The aligned-block fixture still cannot serve as a raw-codec motion control:
its encoder emitted zero motion side-data for a visibly moving block. A new
post-fix capture is required before judging the fallback's dense-field
quality.

## 23.23 — Jitter provenance in event traces (2026-08-29)

The runtime event trace now records `jitterX`, `jitterY`, and
`jitterApplied` for every captured frame. These are the source/render-space
values selected by the side-buffer stage, so a capture can prove whether a
variable Halton phase was actually submitted rather than inferring it from
the final image. The fields are diagnostic only; they do not change color
sampling, motion vectors, or temporal dispatch.

Validation was written first in
`tests/test_event_trace_runtime_contract.py`; the contract test passed, then
the player rebuilt successfully. The existing jitter tests and full CTest
remain green. The measured campaign bottleneck is still the blocking temporal
dispatch, not jitter generation.

## 23.24 — Reduced-model jitter coordinate correction (2026-08-29)

The jitter audit identified a correctness issue in the variable-jitter
metadata path: history reprojection was multiplying a model-space
jitter delta by the source-to-output ratio. The prepass now uses its existing
`modelToOutputScale`, so reduced neural models do not receive a second model
scale. The color sampler and motion estimator remain unchanged.

Validation was written first:

```text
fsr4_motion_contract_tests: passed
jitter_tests: passed against the checked-in FidelityFX floor rule
player rebuild: passed; prepass shader compiled to SPIR-V
```

The external sign convention still needs a controlled GPU impulse/gradient
test before variable jitter can be treated as fully proven rather than
internally consistent.

The rebuilt live proof harness was also run manually on the RX 7900 GRE:

```text
fsr4_harness_tests: OK
FSR4 INT8 structural proof: PASSED
dispatch: 13.579628 ms at 1280x720 → 3840x2160
NaN/Inf: 0; non-zero samples: 100%
```

This proves the rebuilt dispatch remains operational, but it is not the
missing jitter sign test: the existing proof harness uses seeded synthetic
input and does not exercise `GpuImageUploader::setInputJitter()`.

## 23.25 — Hardware jitter sampling-direction probe (2026-08-29)

A separate disabled GPU probe now exercises the actual decoded YUV420 →
RGB10/A2 model-color upload path. It uploads a deterministic bright stripe,
reads the model image back, and compares zero jitter with a fixed positive
0.5 source-pixel X offset. On the RX 7900 GRE it measured:

```text
zero jitter centroid X: 15.500000
+0.5px X jitter centroid X: 15.000000
directional delta: -0.500000 pixels
```

This proves the production sampler's `p + jitter` direction and its
half-pixel displacement on hardware, including the RGB10/A2 quantization
boundary. The probe is intentionally disabled in ordinary CTest because it
requires a live Vulkan device; run it manually with:

```text
./build-fast/tests/jitter_gpu_contract_tests
```

The probe does not yet cover temporal history reprojection or a reduced-model
source→model→output pair, so those remain separate integration gates.

## 23.26 — Failed-dispatch analysis-history rollback (2026-08-29)

The temporal lifecycle audit found that `SideBufferSynth::update()` advanced
the previous-luma, histogram, cadence, and validity state before FSR
submission completed. If upload, dispatch, presentation, or a later chained
pass failed, jitter was rolled back but the luma history was not. The next
motion estimate could therefore compare the next successful frame against a
decoded frame that had never reached FSR history.

The existing abort boundary, `rollbackJitter()`, now restores a transaction
snapshot of every analysis state component advanced by `update()`. Successful
dispatches retain the new state as before. A failing-first side-buffer test
reproduced the stale-frame pairing and now passes after the fix.

Validation:

```text
sidebuffer_tests: passed, including failed-frame history rollback
temporal_forge_player: rebuilt successfully
ctest: 19 runnable tests passed; 1 skipped; 3 GPU/diagnostic tests disabled
```

This closes a concrete failure path in history pairing, but it does not by
itself prove that the temporal output beats the spatial baseline on the full
real-world sequence matrix.

## 23.27 — Pending-frame seek quarantine (2026-08-29)

The history/reset audit also found that the decode loop could opportunistically
buffer one decoded frame, then consume it after `seekPending_` was raised. That
frame belongs to the pre-seek sequence and could bypass the intended decoder
flush/reset boundary.

The pending-frame consumption branch now checks the seek flag before moving the
frame into the active decode variable. If a seek is pending, it discards the
buffered frame and exits the current decode batch; the subsequent flush starts
the new sequence normally. A failing-first source contract test now requires
this quarantine and passes.

Validation:

```text
test_temporal_runner_contract.py pending-frame test: passed
sidebuffer_tests: passed
temporal_forge_player: rebuilt successfully
ctest: 19 runnable tests passed; 1 skipped; 3 GPU/diagnostic tests disabled
```

This closes the stale-pending-frame path. A live interactive seek capture is
still required before the complete seek/history gate can be called proven.

## 23.28 — Reduced-model motion conversion (2026-08-29)

The reduced-model audit found that motion vectors were uploaded in model-pixel
units but multiplied in the prepass by `slot1.xy`, the decoded-source→output
ratio. For a smaller model this under-scaled history displacement by
`model/source`. The prepass now multiplies uploaded motion by the already
derived `modelToOutputScale`; source→model remains the lookup/coverage
conversion used before upload.

Validation:

```text
failing-first fsr4_motion_contract_tests: failed against slot1.xy
corrected fsr4_motion_contract_tests: passed
prepass shader: compiled to SPIR-V
full runnable CTest: 19 passed; 1 skipped; 3 GPU/diagnostic tests disabled
```

This fixes a concrete reduced-model coordinate defect. A fresh reduced-model
real-sequence capture is still needed to measure its visual effect.

The fresh real rooftop capture after this fix (`1280x720 → 3840x2160`, eight
frames, refined codec motion, zero jitter) completed successfully. It measured
SSIM `0.938986` for FSR versus `0.936530` for Lanczos, but temporal-delta
absolute error was `0.571658` versus `0.093115` for Lanczos. This is a small
spatial win with a clear temporal-stability loss, so the path remains
unpromoted.

## 23.29 — In-flight temporal submission is opt-in (2026-08-29)

The lifecycle audit found that the two-slot asynchronous path could publish
jitter, luma, and frame-continuity state at submission time, before the fence
and presentation scaler reported success. Until that path has a completion-time
commit boundary, it is now enabled only by the explicit
`TFORGE_FSR4_ENABLE_INFLIGHT` diagnostic switch. The blocking path remains the
default for correct temporal state pairing; this is a scheduling guard, not an
image-quality algorithm change.

Validation:

```text
failing-first runner contract: passed after the explicit opt-in guard
temporal_forge_player: rebuilt successfully
full runnable CTest: 19 passed; 1 skipped; 3 GPU/diagnostic tests disabled
```

The timed repeat recorded approximately `4.89–4.98 ms` GPU dispatch,
`0.64–1.30 ms` motion-estimator CPU time, and `8.3–12.0 ms` total dispatch /
pipeline CPU time after warm-up on the RX 7900 GRE. The retained artifact is
`benchmarks/video_corpus/results/temporal_rebuild_reduced_motion_rooftop_20260829_timed/`.

## 23.30 — Seek-generation reset and guarded in-flight default (2026-08-29)

The seek audit showed that the demux thread flushes the decoder directly, so
the decode thread cannot rely on a queued `Packet::isFlush` marker to clear its
CPU analysis state. A monotonic `seekGeneration_` is now incremented by
`seekUs()` and consumed by the decode thread. When it changes, the decode loop
discards any pending old frame, clears `SideBufferSynth` analysis state, resets
frame continuity/jitter provenance, and starts the next source frame on the
existing reset path. The reset stays on the decode thread to avoid a data race.

The same audit found that asynchronous two-slot submission committed state
before fence/presentation completion. It is now disabled by default and can be
enabled only with `TFORGE_FSR4_ENABLE_INFLIGHT` for a separate throughput
probe. This preserves the blocking path's correctness while that completion
boundary remains unimplemented.

Validation:

```text
failing-first seek-generation contract: passed
failing-first in-flight opt-in contract: passed
temporal_forge_player: rebuilt successfully
full runnable CTest: 19 passed; 1 skipped; 3 GPU/diagnostic tests disabled
```

# 24. Best-Known Configuration

Update this section after every stage.

```text
Stage:
F post-jitter-fix local winner; broad corpus, anti-aliasing, performance,
temporal, and local competitor-control gates completed

Commit:
45272e22cbd0964314cd3e07289b83b808a9aa61 baseline plus uncommitted Quality Lab changes

Composition:
base_only

Base filter:
bilinear

Learned strength:
0.00 direct blend contribution

Residual:
disabled

Sharpen:
disabled

Tone:
corrective exposure -0.015 EV, 0 contrast, pivot 0.5, gamma 1.0

Presentation:
Quality Lab presentation selection is wired and captured separately; current
filters remain close and no universal winner is promoted yet

GPU ms:
1.425 ms local finalist mean; 1.429 ms six-row broad quality-capture mean

Pipeline ms:
1.539 ms local finalist mean; 1.546 ms six-row broad quality-capture mean

Quality summary:
Stage F broad six-row mean FSR PSNR/SSIM/edge-SSIM 28.342274 / 0.751456 /
0.797694, low-frequency luma MAE 0.012008 and bias +0.002138. The local
three-clip finalist mean is 24.101126 / 0.737582 / 0.713305. The
supersampled anti-aliasing candidate measured 19.604901 / 0.786898 / 0.734911
and exceeded the Lanczos control on edge-SSIM by 0.019074.

Known remaining defects:
The stable path remains softer than native high-resolution detail and is mixed
against libplacebo/FSRCNNX controls; no universal superiority claim is made.
The installed mpv build lacks an FSR1 scaler option. The checked-in
`synthetic_motion` generator is static and is excluded from temporal acceptance
until corrected; a fresh moving control was used instead. Future-frame data
was not added to the causal path because the measured defect was base-only
jitter sampling, not proven temporal-model failure. Presentation filters remain
runtime-selectable because no universal filter winner was measured.
```

---

# 25. 2026-08-29 temporal-control and off-screen-history follow-up

The matched rooftop controls were rerun after the lifecycle and reduced-model
motion fixes. Both the zero-motion and codec-refined arms used
`TFORGE_FSR4_JITTER_MODE=off`, so variable jitter was not part of these
measurements. The debug artifacts are retained at:

```text
benchmarks/video_corpus/results/temporal_motion_ablation_zero_debug_20260829/
benchmarks/video_corpus/results/temporal_motion_ablation_refined_debug_20260829/
benchmarks/video_corpus/results/temporal_offscreen_history_fix_rooftop_20260829/
```

The refined arm uploaded non-zero motion seeds on frames 1 and 2 (2583 and
2428 seeds); the zero arm uploaded zero seeds. Despite that, the matched
three-frame 1920x1080 quality summaries were identical, so motion was proven
to reach the upload boundary but not to produce a measurable output change in
that short control. The eight-frame 3840x2160 post-fix capture retained the
prior result: FSR SSIM `0.938986` versus Lanczos `0.936530`, while temporal
delta error was `0.571658` versus `0.093115`. Temporal promotion remains
blocked.

The read-only shader audit found a concrete history defect: a covered motion
block whose reprojection landed off-screen left `historyModel` at zero, then
the prepass blended that zero as black history. The prepass now requires both
motion coverage and an on-screen reprojection before sampling/blending history;
the existing invalid-history policy handles the rejected case. A failing-first
source contract was added and passes, and the shader recompiles successfully.

The capture runner previously forced 24-fps source and reference material
through a 30-fps conversion and the temporal metric ignores PTS. The runner
now preserves the input cadence; older artifacts remain warning-only because
they were captured before that fix.

## 2026-08-29 postpass temporal-bypass audit

The generic graph's host constants were setting postpass bit `1024` whenever
best-findings mode was active. In `postpass_composite.comp`, that bit selects
`upscaledColor` directly and bypasses `u_reprojectedColor`, so motion/history
could not affect the final image even though the prepass tensor changed.
The host condition is now diagnostic-only:
`TFORGE_FSR4_EXPERIMENTAL_SINGLE_HISTORY_BLEND` must be explicitly set. The
regression is covered by `fsr4_postpass_contract_tests`.

The runner also now forwards `TFORGE_FSR4_DISABLE_NATIVE_INT8`; before this
fix, generic-graph controls silently used the native INT8 graph. The matched
post-fix short captures still matched at encoded output, so no quality
promotion is claimed yet. Pre-final tensor statistics differ and the dispatch
trace confirms the generic graph; the remaining investigation is whether the
history blend is quantized away or the published output path bypasses the
changed postpass result.

## 2026-08-29 learned-blend provenance correction

The first postpass trace after the bypass fix exposed a capture-harness error:
the runner was explicitly loading `config/quality_lab.json`, whose checked-in
configuration is `base_only` with `learnedStrength: 0.0`. Those runs were
valid spatial controls, but were mislabeled as temporal output for this A/B.

The harness now has a failing-first contract ensuring that the aggressive
reactive learned-strength suppression is enabled only by the explicit
`TFORGE_FSR4_ADAPTIVE_LEARNED_STRENGTH` opt-in. Best-findings mode no longer
silently enables that diagnostic gate. This prevents a reactive average of
`1.0` from forcing the final learned blend to zero.

After rerunning with an explicitly disabled Quality Lab configuration, the
dispatch trace reports `learnedStrength: 0.075`, effective confidence `0.5`,
and final learned blend `0.0375` on all eight frames. The matched refined and
zero-motion captures are retained at:

```text
benchmarks/video_corpus/results/temporal_actual_refined_20260829/
benchmarks/video_corpus/results/temporal_actual_zero_20260829/
```

Their dumped FSR frame hashes differ for every frame, proving that the guarded
motion path now changes the actual published output. This is a wiring and
provenance correction, not a quality promotion; representative-scene quality,
temporal stability, and frame-time comparisons remain to be measured.

The first variable-jitter rerun was compared with a command-line alias that
the runner does not consume (`TFORGE_FSR4_JITTER=0`), so it was not a valid
jitter-off control. The corrected control uses
`TFORGE_FSR4_JITTER_MODE=off` and is retained at:

```text
benchmarks/video_corpus/results/temporal_actual_trueoff_refined_20260829/
benchmarks/video_corpus/results/temporal_actual_variable_refined_20260829/
```

Future jitter conclusions must use the canonical mode variable and record the
per-frame jitter samples separately from frame-time measurements.

A four-frame trace using the corrected variable-jitter control recorded the
expected changing render-space samples:
`(0.00000,-0.08519)`, `(-0.12778,0.08519)`, `(0.12778,-0.19877)`, and
`(-0.19167,-0.02840)`. The same trace kept learned blend at `0.0375` and
completed in the normal capture path. This confirms that jitter is changing
the FSR input phase per dispatched frame; it does not by itself establish a
frame-time regression or a quality win.

Timing isolation at the same 640x360 input and 1920x1080 output shows the
slow result was graph-selection dependent, not jitter dependent. The forced
generic graph measured about `37.1–37.6 ms` GPU per frame with jitter on and
off. The native INT8 graph measured about `1.31–1.43 ms` GPU per frame with
variable jitter and `1.33–1.43 ms` with jitter off. The generic-graph timing is
retained as diagnostic evidence; it must not be used as the production
frametime claim. The native path remains within the fast-path budget in this
sample.

## 2026-08-29 learned-strength isolation

With jitter off, native INT8, disabled Quality Lab, and the same 640x360 →
1920x1080 Tears of Steel daylight sequence, a five-point learned-strength
sweep was captured at
`benchmarks/video_corpus/results/temporal_learned_strength_sweep_tos_daylight_20260829/`.
Mean SSIM was `0.894940`, `0.894904`, `0.894850`, `0.894675`, and `0.894301`
for strengths `0.00`, `0.075`, `0.15`, `0.30`, and `0.50`; the Lanczos control
was `0.893764` for all rows. The lowest temporal absolute error in this short
sample was at `0.30` (`1.262657`), while `0.075` was `1.261194` and `0.00`
was `1.260087`. The differences are small and the metrics trade off, so no
new strength is promoted from this eight-frame slice. The existing `0.075`
best-findings default remains reproducible while longer-sequence validation is
pending.

## 2026-08-29 thirty-frame native temporal matrix

The short four-scene check was extended to 30 frames per arm at 640x360 →
1920x1080 on the native INT8 path, with separate true-off and variable-jitter
controls. All 16 captures completed and are retained under
`benchmarks/video_corpus/results/temporal_native_matrix_640_to_1080_30f_20260829/`.

FSR mean SSIM exceeded the matched Lanczos control in every row: cave
`0.9569–0.9592` vs `0.9515`, rooftop `0.8295–0.8343` vs `0.8264`, daylight
`0.8962–0.8977` vs `0.8954`, and debris `0.9557–0.9570` vs `0.9546`.
Temporal-delta absolute error was scene-dependent: variable-jitter daylight
was better than Lanczos (`0.8798` vs `1.1530`), but variable-jitter rooftop
(`0.8399` vs `0.3086`), cave (`0.4042` vs `0.3494`), and debris (`0.1661` vs
`0.0970`) were worse. Refinement itself was small: it improved the 30-frame
temporal error over zero motion on rooftop and debris, was nearly neutral on
cave, and was worse on daylight.

Conclusion: the corrected path is a spatial-quality win in this matrix, but
the full temporal system is not yet a universal stability win. Motion and
jitter remain selectable evidence arms; further work must target the scene-
dependent temporal error rather than adding sharpening or another resampler.

## 2026-08-29 deep-research review and clean motion-control probe

`docs/deep-research-report(14).md` was reviewed against the current Linux/
Vulkan implementation. Its central caution matches the campaign evidence:
synthetic jitter can satisfy FSR's input contract when the color sample is
actually shifted, but it cannot recover renderer-level subpixel information
from an already rasterized video frame. Variable jitter therefore remains an
opt-in evidence arm, while true zero jitter remains a required control and
fallback. No jitter policy was promoted from this report.

A clean four-frame known-pan probe was run with identical settings for
`codec` and `codec_refined`: best-findings disabled, jitter off, software
decode, disabled Quality Lab, motion-seed dumps enabled, and bounded
correction limited to one source pixel. The captures are retained at:

```text
benchmarks/video_corpus/results/motion_contract_probe_20260829_clean_codec/
benchmarks/video_corpus/results/motion_contract_probe_20260829_clean_codec_refined/
```

Both arms admitted the expected current-to-previous seeds (`mv=(-2,0)`) on
the moving blocks. Refined mode changed confidence on some blocks but did not
change the four-frame encoded output metrics (`SSIM 0.354757`, temporal error
about `0.1056`). This is not a quality win and does not prove dense-field
correctness: the supplied reference is not a valid quality reference for this
contract fixture, so the result is used only for seed/sign/wiring evidence.
The next required motion gate is direct dense-field and warped-history
residual validation on the same fixture.

The first real-world causal A/B after the boundary fix used Sintel rooftop
`640x360 -> 1920x1080`, eight scored frames, identical disabled Quality Lab,
`TFORGE_FSR4_JITTER_MODE=off`, and the actual zero-motion ablation
(`TFORGE_FSR4_MOTION_ABLATION=zero`) versus `codec_refined`. The captures are
retained at:

```text
benchmarks/video_corpus/results/motion_quality_ab_rooftop_640_1080_20260829_correct_zero/
benchmarks/video_corpus/results/motion_quality_ab_rooftop_640_1080_20260829_correct_codec_refined/
```

The outputs now differ, proving the A/B boundary reaches published pixels.
Zero motion scored SSIM `0.843903` and temporal absolute error `0.468114`;
refined motion scored SSIM `0.843905` and temporal absolute error `0.468571`.
The refined trace confirmed substantial non-zero motion seeds (941, 890, 606,
458, 389, 537, 299, and 459 on successive captured frames). On this rooftop
slice refinement is therefore a tiny spatial gain but a tiny temporal loss;
it is not promoted as a universal policy. The earlier identical A/B was
invalid because estimator `off` preserved baseline codec vectors rather than
zeroing them.

As a lower-level check, the dumped GPU dense field from the known-pan capture
was applied directly to the previous decoded luma frame. On frame 1, whole-
frame MAE fell from `0.002847` with zero motion to `0.002049` with the GPU
field; on the valid-motion region it fell from `0.002483` to `0.001618`, and
on the motion-bearing region it fell from `0.044922` to `0.000000`. Frame 2
was neutral because the adjacent source pair had no measurable object change;
frame 3 repeated the frame-1 improvement. This proves the current dense field
is useful correspondence on the fixture even though that benefit does not yet
translate into a universal FSR-quality win on real footage.

## 2026-08-29 publication-boundary and provenance fixes

The follow-up audit found two lifecycle defects that could make a valid
comparison look wrong even when the reconstruction dispatch itself completed:
neural publication did not clear the prior native-passthrough selector, and
history ping-pong advanced before the final presentation scaler had succeeded.
Both are now fixed in `PlaybackEngine.cpp`. Native passthrough is cleared at
neural publication, and every pass commits history only after the complete
chain and presentation scaler succeed. The in-flight retirement path follows
the same ordering.

The dispatch trace was also corrected: it now reports `native-int8 graph
complete` or `generic-split graph complete` instead of labeling both paths as
native. This matters because the generic path is roughly 37 ms in the retained
diagnostic profile while the native path is roughly 1.3--1.4 ms.

The failing-first temporal contract test caught all three regressions before
the implementation changes. The rebuilt binary passed the full runnable CTest
set: 19/19 passed, with the existing GPU/tensormap tests skipped or disabled.
A live rebuilt known-pan capture retained dense motion readback with frames
1--3 having median `mv=(-2,0)` and the corrected `native-int8` trace label:

```text
benchmarks/video_corpus/results/motion_contract_probe_20260829_lifecycle_fixed/
```

The dumped `RG16F` dense field was compared against the exact accepted seed
rectangles from that same capture, not against a differently tiled optical-flow
sidecar. Frames 1--3 matched the CPU seed expansion exactly over all covered
pixels (median and P95 vector error `0.0`; 100% of covered pixels within
`0.01` px). This proves sparse-to-dense GPU expansion and sign preservation on
the fixture. It still does not prove that the resulting correspondence is the
best history field for real-world FSR output, so the quality gate remains open.

## 2026-08-29 dense-replay fail-closed capture boundary

The temporal runner previously allowed an explicitly supplied dense replay
sidecar to omit a requested capture-relative frame. `PlaybackEngine` correctly
cleared replay motion on that miss, but the runner continued and emitted normal
metrics, making the diagnostic capture look complete while no longer measuring
the requested replay. The runner now validates the sidecar before launching the
player and rejects missing or duplicate required relative frame indices. This
is diagnostic-only: normal captures and the player's normal motion/FSR path are
unchanged. A failing-first runner contract test proves the fake player is not
started for a partial sidecar.

The previously retained complete dense replay requested eight scored frames and
contained relative frames `0..7`; its player log's later frame-9 warning came
from an extra decode after the runner had already observed the requested eight
PPMs and is not part of the scored capture. Its metrics remain diagnostic
evidence only: FSR SSIM `0.843911` versus Lanczos `0.841515`, with temporal
absolute error `0.469600` versus `0.205071`.

The clean rerun extended the sidecar through decoder read-ahead and completed
the same eight-frame comparison without a replay-miss warning:

```text
benchmarks/video_corpus/results/motion_quality_rooftop_dense_replay_20260829_clean/
```

Variable synthetic jitter was also checked against true jitter-off on the same
640x360 -> 1920x1080 path. The synchronous GPU means were `1.319 ms` with
variable jitter and `1.326 ms` with jitter off, a `0.008 ms` difference within
measurement noise; both used one FSR pass. This does not establish throughput
parity for the optional in-flight path, because the current implementation
disables in-flight overlap when synthetic jitter is active. That scheduling
question remains a separate performance gate, not evidence that jitter adds
extra reconstruction work.

## 2026-08-29 — stateless jitter throughput guard

The no-readback throughput probe isolated the scheduling cost left open above.
With two-slot in-flight submission enabled and persistent color/recurrent
history disabled, jitter-off measured a median CPU pipeline time of `0.086 ms`;
variable jitter measured `0.074 ms`. GPU time stayed near `1.35 ms` in both
arms. The old unconditional `!syntheticJitterEnabled` predicate was therefore
needlessly serializing stateless playback.

The async predicate now blocks only when persistent temporal state is enabled;
stateless jitter may use the existing explicit in-flight opt-in. A short
stateful probe still measured serialized presentation/wait work (pipeline
median `7.511 ms`), so this change does not alternate independent history
slots or weaken the stateful temporal contract. The failing-first async
contract test and full runnable CTest pass. This is a scheduling fix only; no
shader, reconstruction, motion, or jitter algorithm changed.

## 2026-08-29 — clean 24-fps motion/jitter comparison

The metric audit rejected the older rooftop/debris pairfix results because
they were retimed to 30 fps, included startup frames, and used only the legacy
whole-frame `tblend` signal. A clean replacement was captured at the source
24 fps with 36 warmup frames and 24 scored frames, using the same enabled
current-composition control at 1280x720 -> 3840x2160. The four arms are kept
separate under:

```text
benchmarks/video_corpus/results/temporal_audit_clean_20260829/
```

For rooftop, codec-refined motion and zero motion were effectively tied:
SSIM `0.915101` vs `0.915100`, and temporal-delta error `0.176348` vs
`0.176259`; neither is a motion win. Variable jitter raised mean SSIM to
`0.917562` but worsened temporal-delta error to `0.211743`, so it is not a
stable improvement on this scene.

For Tears of Steel debris, refined and zero motion were again effectively tied:
SSIM `0.980434` for both and temporal-delta error `0.129253` vs `0.129274`.
Variable jitter slightly improved temporal-delta error to `0.128541` but
reduced SSIM to `0.979823`. The result is scene-dependent, not a universal
promotion. These captures are valid legacy temporal evidence, but still do not
claim validity-masked motion quality because no scene-specific validity masks
exist for these two clips.

## 2026-08-29 — stateful variable-jitter frame-time attribution

A focused audit separated synthetic-jitter work from temporal-state scheduling.
After warmup, variable jitter on the stateless two-slot path measured `0.074 ms`
median CPU pipeline time and `1.366 ms` median GPU time; jitter-off measured
`0.088 ms` and `1.360 ms`. The earlier slow variable-jitter run measured
`9.582 ms` CPU pipeline time while GPU work remained `1.347 ms`, proving that
the delay was synchronization rather than jitter generation or reconstruction.

The stateful path correctly disables slot alternation because color/recurrent
history must remain causal. It then waits for the FSR submission and submits a
separate presentation scaler, waiting again. The presentation image is reused
between frames and normal playback performs no readback, so neither allocation
nor readback explains the delay. The remaining performance work is to fuse the
presentation dispatch into the FSR submission while preserving the existing
FSR→presentation ordering and publishing history only after both complete.
Until that has pixel/hash and history-order validation, no stateful async bypass
will be enabled.

The fused record-only seam was then implemented for the stateful single-pass
path. The uploader owns a dedicated presentation descriptor set, and the FSR
harness records `FSR postpass -> presentation scaler -> publication barrier`
before its existing fence submission. Chained passes and stateless in-flight
dispatch retain their existing paths. A 640x360 -> 3840x2160 variable-jitter
pixel A/B produced identical presentation PPM hashes:

```text
6852fde244926276479519a5028423c7d98b93272199af82e48c4da702e03980
```

The short runtime probe measured approximately `7.0–8.4 ms` total for the
fused 4K stateful path versus `8.3–8.5 ms` unfused on the same machine. The
gain is modest at 4K because the scaler GPU work remains real, but the second
submission/wait is removed without changing pixels. The explicit
`TFORGE_FSR4_DISABLE_FUSED_PRESENTATION` switch remains available for future
matched A/B captures.

## 2026-08-29 — post-pause clean cadence cave probe

After the interactive workload pause was lifted, a bounded real-corpus probe
was run before resuming broader campaign work. It used the existing `current`
configuration on Sintel cave at 640x360 input and 1920x1080 output, with 36
warmup frames followed by 24 scored frames, source cadence preserved, hardware
decode disabled, refined motion selected, and synthetic jitter explicitly off.
The capture completed without failure under:

```text
benchmarks/video_corpus/results/temporal_cadence_sintel_cave_current_20260829/
```

The FSR arm measured mean SSIM `0.902643` versus `0.899246` for the matched
Lanczos control. That spatial result is accompanied by worse temporal-delta
absolute error: `0.637098` for FSR versus `0.510511` for Lanczos. The result is
therefore retained as cadence-clean evidence only; it does not justify
promoting the temporal path. The runtime trace reports one FSR pass at about
`2.571 ms` GPU dispatch time. No reconstruction or image-quality algorithm was
changed for this probe.

## 2026-08-29 — confidence-gate and second-scene probe

The cave capture was repeated with the same 36-frame warmup, 24 scored frames,
source cadence, 640x360 input, 1920x1080 output, refined motion, jitter off,
history/recurrent state, and CAS `0.20`, changing only the history-confidence
threshold. The default best-findings arm uses the confidence map and its
`0.85` scalar gate. A threshold of `0` changed every retained FSR frame and
raised cave SSIM from `0.902643` to `0.903015`, but worsened the legacy temporal
delta error from `0.637098` to `0.650557`. This confirms the gate affects the
published image, but does not justify removing or weakening it.

The same default-versus-pre-campaign comparison was then run on Tears of Steel
daylight under:

```text
benchmarks/video_corpus/results/temporal_cadence_tos_daylight_current_20260829/
benchmarks/video_corpus/results/temporal_cadence_tos_daylight_pre_campaign_20260829/
```

Both arms used one FSR pass and the same source cadence/warmup protocol. The
default arm measured SSIM `0.899036` and legacy temporal error `1.139774`; the
pre-campaign arm measured SSIM `0.899094` and temporal error `1.160375`.
The small spatial difference and scene-dependent temporal loss provide no
evidence for changing the default confidence policy. These legacy delta values
remain diagnostic because they are not motion-compensated and do not carry
per-frame source-PTS identity.

The existing candidate-linked cave event captures were also rechecked before
using them as campaign evidence. All five arms use the same real input and
reference files, source frame range `42–59`, output dimensions `1920x1080`, and
18 assembled event records. Their stronger class-attributed metric rows are
therefore directly joinable for this slice: base-only bilinear
`motion_compensated_error=0.002451593`, current `0.002577616`, Mitchell
`0.002539297`, Catmull-Rom `0.002603184`, and Lanczos2 `0.002604329`.
These values remain one scene/class evidence, not a campaign-wide promotion.

The previously promising learned-strength `0.55` arm was rechecked on the same
fresh cadence protocol at 640x360 input and 1920x1080 output. It measured cave
SSIM `0.903044` versus the fresh default `0.902643`, but daylight SSIM
`0.896574` versus `0.899036`; its daylight legacy temporal error was `1.135140`
versus `1.139774`, while cave temporal error was `0.634350` versus `0.637098`.
The spatial regression at this lower input tier means the earlier 426x240 and
1280x720 sweep cannot support global promotion. This is concrete evidence that
learned strength is resolution-dependent; no default was changed.

The next M6 event slice was then captured for the `current` arm on Tears of
Steel daylight, using the existing real 42–59 PTS-gap input/reference pair and
the grounded `faces-hair-skin` mask. The 18-frame capture retained causal
motion and event sidecars and completed at `1920x1080` under:

```text
benchmarks/video_corpus/results/m6_event_tos_faces_hair_skin_current_20260829/
```

Its class-attributed motion-compensated error is `0.014851517`, compared with
`0.014858980` for the existing bilinear baseline slice. This is effectively a
tie at the precision of this evidence, so it is recorded as a non-regression
only; it does not establish a temporal quality gain.

The matched ToS daylight `base_only_mitchell` event slice is also now present
under:

```text
benchmarks/video_corpus/results/m6_event_tos_faces_hair_skin_base_only_mitchell_20260829/
```

It uses the same real input/reference, frame range `42–59`, output dimensions,
quality class, event metadata, causal motion sidecar, and grounded static mask.
Its class-attributed motion-compensated error is `0.014503483`, versus
`0.014858980` for the matched bilinear baseline and `0.014851517` for current.
This is useful evidence for the Mitchell base-filter candidate on this one
ToS class slice, but it is not a global promotion until the remaining matched
classes and scenes are assembled.

The motion-sidecar validator also now requires `ptsUs` to increase strictly in
presentation order. Before this guard, duplicate or backward timestamps could
pass numeric validation and associate a valid vector with the wrong displayed
transition, contaminating replay and motion-compensated metrics. The new
failing-first test covers both cases in
`tests/test_motion_sidecar.py`; the validator rejects them before expansion.
This is a campaign-integrity fix only and does not change runtime motion or
FSR behavior.

The capture label contract was then corrected: when the dedicated estimator
environment variable is absent, `TFORGE_FSR4_MOTION_ABLATION=refined` now
selects `MotionEstimatorMode::CodecRefined`. The dedicated estimator variable
still has precedence, while `zero` and `block` remain payload ablations. A
failing-first estimator test and the motion contract test verify this mapping.
This makes future refined captures truthful; it does not retroactively relabel
or reuse earlier captures.

An offline `ffprobe` audit of the matched ToS event input confirms that the
source declares `color_range`, `color_space`, `color_transfer`, and
`color_primaries` as `unknown` (`yuv420p`, 426x240). Its nominal frame rate is
24 fps while the average rate is `72/5`, consistent with the intentionally
inserted PTS gap in this event fixture. The next 720p color A/B must therefore
retain the selected fallback matrix/transfer and the decoded metadata in its
provenance; this audit does not select a color path retroactively.

The temporal capture runner now retains `source_stream_metadata.json` beside
successful artifacts. It records the exact source stream geometry, pixel
format, cadence, and declared color metadata used by `ffprobe`; this is
provenance only and does not alter decoding or reconstruction. The metadata is
distinct from decoder-resolved fallback values, which must still be recorded
during the next GPU-safe 720p color A/B.

While GPU capture is paused, the jitter policy tests were expanded to cover
phase holding/advance for an explicit cadence and runtime sequence selection
(zero and alternating). The focused jitter, side-buffer, and motion tests all
pass. End-to-end GPU propagation and real-scene temporal evidence remain open.

The opt-in decoder diagnostic now also logs the resolved metadata of the exact
frame passed to the FSR upload boundary (`avFormat`, color range, matrix,
transfer, primaries, dimensions, and PTS). This closes the provenance gap
between container declarations and the actual runtime frame without changing
the image path. The color metadata contract test passes.

That 720p color A/B is now captured for Tears of Steel daylight, 1280x720
medium input to 1920x1080, with 36 warmup and 24 scored frames, jitter off,
software decode, color history/recurrent state enabled, and identical current
path settings. Default color handling produced SSIM `0.961324` versus
`0.950383` for the explicit Rec.709 input override; minimum SSIM was `0.955924`
versus `0.944211`. GPU dispatch time was effectively unchanged (`2.040 ms`
versus `2.029 ms`). The matched artifacts are retained under
`benchmarks/video_corpus/results/m6_color_tos_daylight_720_to_1080_20260829/`.
The short decoder-diagnostic rerun recorded resolved metadata for both arms:
format `0`, range `0`, matrix `2`, transfer `2`, primaries `2`, and PTS
`166667 us`. Conclusion: explicit Rec.709 override is rejected for this
sample; this is not a global color-path promotion and runtime defaults remain
unchanged.

The dependency-free temporal metric tests now also cover fractional horizontal
and vertical sampling plus the explicit no-correspondence mask failure. These
tests protect the bilinear boundary and validity semantics used by later
motion-compensated captures; they do not constitute real-scene evidence.

# 26. Final Principle

We are **not** trying to make a prettier image by piling random filters on top of it.

We are trying to discover which stage is responsible for each remaining visual error and then correct that stage.

Use:

```text
hypothesis
→ controlled implementation
→ measurement
→ visual inspection
→ conclusion
→ evidence-driven next experiment
```

Change one class of behavior at a time.

Every retained quality decision must be reproducible from configuration and must not require recompilation or source edits.
## 2026-08-29 — scale-matched temporal comparisons

Temporal reconstruction quality must be evaluated at the specific input/output
resolution pair under test. Do not assume that one reconstruction ratio is the
correct target for every scene or source resolution: modern temporal upscalers
may select different internal or effective scale levels by content and quality
mode. Every A/B decision therefore records and matches scene, input dimensions,
output dimensions, frame cadence, temporal configuration, and presentation
filter. Results from one ratio are evidence only for that ratio until a separate
capture validates another ratio.

The first 720p → 1440p scale probe accidentally used CAS `0.04`; those two
arms are retained as configuration-specific diagnostics only. The corrected
matched capture uses the required display CAS `0.20` and completed 24 scored
frames for both zero-motion and refined-motion arms under
`benchmarks/video_corpus/results/m6_scale_tos_daylight_720_to_1440_cas20_20260829/`.
At this ratio, zero motion measured SSIM `0.942012` and temporal-delta
absolute error `0.921578`; refined motion measured SSIM `0.943107` and error
`0.882531`. Lanczos measured SSIM `0.950394` and error `1.080898` in the same
pair. Refined motion therefore improves both metrics relative to zero motion
for this scene/ratio, but remains below Lanczos on SSIM; this is scale-specific
evidence, not a global promotion. The measured GPU dispatch was approximately
`79.47 ms` (zero motion) and `79.63 ms` (refined motion), so this 1440p path is
not currently real-time practical on the test system.

The matched 720p → 1080p rerun was also corrected to display CAS `0.20` under
`benchmarks/video_corpus/results/m6_scale_tos_daylight_720_to_1080_cas20_20260829/`.
Zero motion measured SSIM `0.960012` and temporal-delta absolute error
`0.661112`; refined motion measured SSIM `0.960011` and error `0.663110`.
Lanczos measured SSIM `0.962088` and error `0.824440`. At this lower output
ratio, refined motion is effectively neutral to slightly worse than zero motion
on this slice, while the 1440p pair showed a small refined-motion gain. Both
results reinforce that motion and reconstruction decisions must remain
resolution/ratio-specific. The 1080p arms completed in approximately `2.04 ms`
GPU dispatch time.

## 2026-08-29 — FP16 weight-path status

The current model is not an all-FP16-weight reconstruction. The checked-in
convolution table marks FP16 weights for pass 0 only; the remaining learned
weights are stored as recovered FP8-like/codebook values and, on this RDNA3
GPU, are executed through the INT8 DOT4 path after runtime repacking and
scaling. This GPU is not executing native FP8 arithmetic. The runtime also
builds an FP16-derived buffer and has selectable FP16 arithmetic/direct-dispatch
paths, but the active native graph in the current captures is INT8 and does not
consume that FP16 path. Earlier FP16/INT8-boundary and direct FP16 arithmetic
probes were diagnostic and produced no meaningful quality gain; they do not
answer the separate question of whether a complete FP16-weight conversion would
improve quality. That experiment remains unrun and must be treated as a
distinct, scale-matched candidate if pursued.

## 2026-08-29 — Corrected live FP16 fallback A/B

The first attempted FP16 capture was invalid: playback had hard-coded a
minimal INT8 capability record, so the generic fallback saw
`fp16Fallback=false`. Startup now passes the actual Vulkan instance into
`GpuCapabilityProbe::probe(physical, instance)` and forwards the resulting
capability record. The new handoff contract test passes, as do the existing
motion and temporal contract tests.

On the RX 7900 GRE, the corrected diagnostic run was verified from the runtime
trace with `fp16Fallback=true` and `fp16Upscale=true`. Matched Tears of Steel
daylight 1280x720 → 1920x1080 captures used CAS `.20`, jitter off, zero motion,
36 warmup frames, and 24 scored frames:

| execution path | SSIM | temporal-delta abs error | GPU time |
|---|---:|---:|---:|
| native INT8 DOT4 | `0.971711` | `0.572539` | `1.749 ms` |
| generic FP16 fallback | `0.971711` | `0.572539` | `10.672 ms` |

The aggregate metrics are identical on this slice, while the FP16 fallback is
about 6.1× slower. It is retained as a verified diagnostic path, not promoted
over the production INT8 path. This is not evidence of native FP8 execution:
this GPU does not provide that path. The raw model contains FP8-like/codebook
values; active production execution is INT8 DOT4, with FP16 fallback available
only when explicitly selected.

Artifacts:

- `benchmarks/video_corpus/results/m6_int8_matched_tos_daylight_720_to_1080_cas20_20260829_live/int8/`
- `benchmarks/video_corpus/results/m6_fp16_fallback_tos_daylight_720_to_1080_cas20_20260829_live/fp16_fallback/`

As a publication-path sanity check, a one-frame generic FP16 capture with
`TFORGE_FSR4_DISABLE_RELU=1` changed the published output relative to the
normal generic FP16 frame (`ImageMagick compare` RMSE `0.137982`), and its
trace still identified the generic FP16 final step. This confirms that the
FP16 fallback result is not being silently replaced by a stale native or
readback image. The matched INT8/FP16 equality above is therefore an actual
result for that configuration, not a capture-harness artifact.

Diagnostic artifact:

- `benchmarks/video_corpus/results/m6_generic_relu_probe_tos_daylight_720_to_1080_20260829/generic_no_relu/`

## 2026-08-29 — 720p-to-4K color A/B

To check the reported warmer/brighter 720p temporal output at a larger target,
Tears of Steel daylight 1280x720 → 3840x2160 was captured with the same CAS
`.20`, jitter-off, zero-motion, 36-warmup/24-scored protocol. The default
metadata/fallback color path measured SSIM `0.933967` and temporal-delta
absolute error `1.547249`. An explicit Rec.709 input-transfer override measured
SSIM `0.924981` and error `1.624710`, with identical GPU cost (`6.982 ms` vs
`7.003 ms`). The Rec.709 output was also consistently brighter by approximately
10.34 mean 8-bit RGB levels across matched frames. Rec.709 is rejected for this
scene/scale; the default color path remains unchanged and no broad color
promotion is claimed.

Artifacts:

- `benchmarks/video_corpus/results/m6_color_tos_daylight_720_to_2160_cas20_20260829/default/`
- `benchmarks/video_corpus/results/m6_color_tos_daylight_720_to_2160_cas20_20260829/rec709/`

## 2026-08-31 — metrics-only evidence policy and external aggregate audit

The campaign now explicitly supports `evidenceMode=metrics_only`: retained
image payloads are optional when numeric results, configuration identity,
binary identity, timing, and temporal provenance are preserved. This changes
storage requirements only; it does not waive matrix coverage or finite-metric
validation.

The external locked run
`/mnt/external/Temporal Forge/quality-campaign/locked-current-best-findings-zero-jitter-20260830`
was checked as data-only evidence. It contains one finite 12-frame row for
each of Sintel cave, Sintel rooftop, Tears of Steel daylight, and Tears of
Steel debris at 640x360 -> 3840x2160. The recorded FSR temporal absolute
error is better than Lanczos on cave, but worse on rooftop, daylight, and
debris; therefore this evidence confirms a scene-dependent tradeoff and does
not authorize universal temporal promotion. The strict candidate/class matrix
and event-sidecar joins remain open.

The repository corpus is available for completing the data-only capture: all
four required real 426x240 inputs and their lossless references exist locally.
The fresh data-only M6 capture completed all 20 required candidate/scene/class
rows (five candidates across the four selected classes) at 426x240 -> 1920x1080,
with 18-frame PTS-gap event fixtures. The strict spatial and event-backed
temporal join passed `tools/verify_quality_matrix.py`. Each row retains its CSV
metrics, candidate configuration identity, binary identity, timing, causal-motion
sidecar, static mask, event trace, and finite required metrics. No rendered image
payload is required under `evidenceMode=metrics_only`.

The resulting matrix is under
`benchmarks/quality_sweeps/m6_final_matrix_20260831.json`; the supporting spatial
matrix is `m6_spatial_matrix_20260831.json`, and the reproducible temporal capture
inputs/references are under `m6_temporal_data_only_20260831/`. Aggregate means
favor `base_only_bilinear` over the current learned path on this matrix
(`SSIM 0.597513 vs 0.566602`; motion-compensated error `0.013610 vs
0.014074`), while Mitchell is close but slightly worse on the aggregate temporal
error. This identifies the current matrix winner as a selectable control, not an
automatic default promotion.

Remaining campaign gates are finalist performance/default suitability, cadence-
matched temporal confirmation beyond the 18-frame PTS-gap fixture, and the
decision record for whether any candidate is suitable to become the default.
M7 remains gated on those decisions; the strict M6 matrix gate itself is closed.

## 2026-08-31 — cadence and finalist performance closure

The current path and `base_only_bilinear` were rechecked on all four real
426x240 clips at source cadence (24 fps), with 24 warm-up frames and 24 scored
frames, zero jitter, and matched 1920x1080 output. The capture retained CSV
metrics only. The cadence-clean temporal absolute-error means were:

| arm | mean temporal-delta absolute error | mean FSR SSIM |
|---|---:|---:|
| current | `0.864033` | `0.724131` |
| base_only_bilinear | `1.119196` | `0.721626` |

The current arm was temporally better on this cadence-clean four-scene sample,
while the completed M6 spatial matrix favored `base_only_bilinear` on spatial
SSIM and motion-compensated error. The result is a real spatial/temporal
tradeoff, not a universal candidate win; no default change is justified.

The steady-state performance gate was also run against the same four real
426x240 clips on the rebuilt RX 7900 GRE path, with jitter off and hardware
decode disabled for reproducibility:

| arm | mean GPU | mean pipeline |
|---|---:|---:|
| current | `2.2653 ms` | `2.7029 ms` |
| base_only_bilinear | `2.1159 ms` | `2.5632 ms` |

Both arms remain within the established real-time budget, and the bilinear
control carries no material performance penalty. It is retained as a selectable
diagnostic/control configuration. The current path remains the default because
the only candidate with stronger spatial metrics has a cadence-clean temporal
regression; this is an evidence-backed no-promotion decision, not a claim of
universal superiority.

Artifacts:

- `benchmarks/quality_sweeps/m6_performance_20260831_current_v2.csv`
- `benchmarks/quality_sweeps/m6_performance_20260831_base_only_bilinear_v2.csv`
- cadence-only CSVs under `/tmp/tforge-cadence-final.fm0fCS/`

Validation:

- `tools/verify_quality_matrix.py` passes for `5 candidates × 4 scenes × 4
  classes`.
- CTest passes `20/20` runnable tests, with one skipped and four disabled.
- Focused campaign/temporal tests pass (`109` tests).
- `git diff --check` passes.

Conclusion:

The M6 quality campaign is closed with no default promotion. The data supports
keeping the current path as default, retaining `base_only_bilinear` as a
runtime-selectable spatial control, and avoiding a universal temporal-quality
claim. M7 may proceed only as a separately scoped equivalence/default-policy
review; no quality candidate is promoted by this campaign.

## 2026-08-31 — FSR 4.1 output supersampling for fixed 2x delivery

Hypothesis:

Reconstructing at 2.25x–3.00x and reducing to a fixed 2.00x delivery grid may
improve temporal stability and discard unstable high-frequency errors relative
to direct 2.00x reconstruction.

Configuration:

The existing current Temporal Forge architecture was held constant. Each arm
used `TFORGE_FSR4_FORCE_SCALE` and `TFORGE_FSR4_FORCE_VIEWPORT` to produce a
real intermediate FSR output, with jitter off, software decode, and no changes
to motion, history, color, sharpening, residual, generic-graph/FP8, or weights.
Supersampled arms were reduced with Lanczos to 2560x1440. The direct arm was
captured directly at 2560x1440. Exact runners and CSVs are recorded in
`docs/FSR4_SUPERSAMPLING_REPORT_20260831.md`.

Corpus subset:

Tears of Steel daylight and debris, Sintel rooftop and cave, all high-quality
1280x720 inputs with a 2560x1440 final output. A synthetic edge/text fixture
was additionally tested at 640x360 to 1280x720. Spatial frame 48 was measured
for all arms; temporal captures used 12 warm-up and 12 scored frames at source
cadence.

Metrics:

Across the four real scenes, 3.00x had the best aggregate spatial SSIM
(`0.877217`) and temporal SSIM (`0.878683`) and the lowest temporal delta
error signal (`6.128250`). However, Sintel cave did not follow that result,
and the synthetic edge/text fixture favored 2.25x on edge-SSIM and direct 2x
on SSIM. The 3x aggregate edge-SSIM (`0.837970`) was below every lower-scale
arm. Peak discrete-GPU VRAM was approximately 4.12, 4.46, 4.82, 5.26, and
4.29 GB for 2.00x through 3.00x.

Visual observations:

The campaign used the data-only evidence policy. Frame and motion identities,
dimensions, hashes, and metric outputs are retained; image payloads are not
required, and no visual claim is made from absent image inspection.

Performance:

Representative steady-state GPU samples on Tears of Steel daylight were
22.6, 27.4, 32.8, 38.7, and 8.4 ms for 2.00x through 3.00x. The 3x result is
the existing fixed native shape; 2.25x–2.75x use the slower generic path.

Conclusion:

INCONCLUSIVE as a universal quality promotion. The core hypothesis is
partially supported on the real-scene aggregate but fails the repeatability,
difficult-material, edge-detail, and broad performance gates.

Decision:

Reject automatic overscaled-FSR promotion and keep direct 2x as the production
default. Retain independent reconstruction/delivery dimensions as diagnostic
controls. The provisional 3x winner was followed by a reduction-filter sweep:
bicubic and Lanczos were effectively tied, so no filter promotion was made.
Longer foliage/hair/repeating-texture coverage remains future evidence if this
mode is revisited.

## 2026-08-31 — replacement portable review harness

The obsolete generated/embedded review implementation was removed. The new
`review_harness/` was built from blank as a portable, direct-file viewer with a
single self-contained `index.html`, sibling `images/` result store, canonical
filename construction, independent left/right experiment controls, aligned
sweep comparison, synchronized zoom/pan, fixed 1920x1080 inspection geometry,
and explicit `NO IMAGE` fallback. `tools/export_review_image.py` is the
canonical validating result writer and records actual PNG dimensions at export.

The harness now contains six validated representative PNGs: the original
daylight current/base-only 1080p pair plus direct-2x and 3x-reduced CAS 0.20
1440p outputs for daylight and cave. Other combinations remain selectable and
show `NO IMAGE` when only data-only evidence exists.

Static, JavaScript syntax, Firefox headless rendering, and Marionette checks
passed for panel/control construction, independent selection state, loaded
asset status, missing-asset fallback, and 720p zoom clamping. The harness is
portable by design: it has no framework, build step, backend, database, or
network dependency.

## 2026-08-31 — true-downscaling NativeAA control

The first matched reduction probe used 1920x1080 high-quality inputs and a
1280x720 delivery target. NativeAA 1.0x with CAS 0.20 was compared against
the runner's conventional Lanczos and bicubic controls. Spatial frame-48
results and four 12-frame temporal captures (12 warmup frames) are retained
under `benchmarks/quality_sweeps/fsr_downscale_nativeaa_20260831/` with the
exact configuration and binary/source hashes in its README.

NativeAA lost spatial SSIM to both conventional controls on all four real
scenes. It also lost temporal SSIM and had higher temporal-delta absolute
error than Lanczos on every scene. This rejects NativeAA-assisted downscaling
for 1920x1080 → 1280x720; it does not close the remaining resolution-ratio
matrix or broader output-resolution generalization.

The follow-up placement runner then tested the 2.00x-above-source pipeline with
CAS .20 before reduction, CAS .20 after the FSR resolve but before reduction,
CAS .20 after reduction via an external FFmpeg CAS filter, and no CAS, using
both Lanczos and bicubic. Across 32 finite rows, pre-CAS scored below the other
placements after reduction; resolve-side CAS and no-CAS were effectively tied,
while external post-reduction CAS remained below no-CAS in this slice.
The retained CSV is `benchmarks/quality_sweeps/fsr_downscale_cas_placement_20260831.csv`.
This closes the still-image placement diagnostic for this condition, but the
post-reduction arm is not renderer-integrated and temporal placement coverage
remains open.

A lower-source-resolution spatial slice is retained in
`benchmarks/quality_sweeps/fsr_resolution_640_to_1280_20260831.csv`. It uses
the same four real scenes at 640x360 input and 1280x720 delivery, comparing
2.00x and 3.00x reconstruction. Mean frame-48 PSNR/SSIM/edge-SSIM were
27.372917/0.778014/0.687223 for 2.00x and 27.705603/0.782593/0.688645 for
3.00x. This supports the 3.00x spatial result at this lower source resolution
only; temporal and broader output-resolution coverage remain open.

The paired temporal slice is retained in
`benchmarks/quality_sweeps/fsr_resolution_640_to_1280_temporal_20260831.csv`.
It uses 12 warm-up and 12 scored frames per scene. Mean temporal SSIM was
0.708025 for 2.00x and 0.756272 for 3.00x, with temporal-delta signals of
6.828083 and 5.874093 respectively. This supports the same 3.00x direction
for this 640x360→1280x720 condition only; broader output-resolution coverage
and performance isolation remain open.

A second spatial slice is retained in
`benchmarks/quality_sweeps/fsr_resolution_1280_to_1920_20260831.csv`. At
1280x720 input and 1920x1080 delivery, mean frame-48 PSNR/SSIM/edge-SSIM were
31.202741/0.880638/0.823576 for 2.00x and 31.479691/0.882228/0.818113 for
3.00x. This is mixed evidence—higher SSIM but lower edge-SSIM—and does not
support a universal 3.00x promotion.

The paired temporal slice is retained in
`benchmarks/quality_sweeps/fsr_resolution_1280_to_1920_temporal_20260831.csv`.
It uses 12 warm-up and 12 scored frames per scene. Mean temporal SSIM was
0.803889 for 2.00x and 0.851838 for 3.00x, with temporal-delta signals of
7.128723 and 6.795694 respectively. This supports the 3.00x temporal result
for this 1280x720→1920x1080 condition only; edge and performance tradeoffs
remain.

The temporal placement follow-up is retained in
`benchmarks/quality_sweeps/fsr_downscale_cas_placement_temporal_20260831.csv`.
It covers 12 scored frames after a 12-frame warmup for all four scenes and
both reducers. Resolve-side CAS and no-CAS were close in mean SSIM, while the
external post-reduction CAS arm was lower for both reducers and had a much
larger temporal-delta signal. This rejects the external post-reduction arm for
the tested condition; it does not claim renderer-integrated post-downsample
CAS coverage.

The paired CAS-strength spatial capture is retained in
`benchmarks/quality_sweeps/cas_strength_pair_20260831.csv`. It contains 16
rows for `current` and `base_only_bilinear` at CAS 0.04 and 0.20, using the
same four 1280x720 sources, frame 48, 1920x1080 output, configs, and binary.
Across the four-scene means, current changed from PSNR 31.053447 / SSIM
0.879059 / edge-SSIM 0.829815 at 0.04 to 31.053407 / 0.879062 / 0.829798 at
0.20. Base-only bilinear was identical at the reported precision: 31.279136 /
0.881645 / 0.826906 at both strengths. This closes the spatial CAS-strength
delta for this paired condition.

The companion temporal capture is retained in
`benchmarks/quality_sweeps/cas_strength_pair_temporal_20260831.csv`. It has 32
rows covering both candidates, both strengths, four scenes, 12 scored frames
after 12 warmup frames, and Lanczos plus bicubic reducers. At 0.04, mean
temporal SSIM was 0.879464 for current and 0.881754 for base-only bilinear;
at 0.20 it was 0.877150 and 0.880125. The 0.20-minus-0.04 deltas were
−0.002314 and −0.001629 SSIM, with temporal-delta increases of +0.120523 and
+0.112248. The paired CAS-strength comparison is now closed for this
condition: increasing CAS to 0.20 is not retained.

## 2026-08-31 — 720p input to 1440p delivery generalization

The existing supersampling runners were hardened to require an explicit
`--cas-strength` argument and to write that value into every output row. The
first attempt at this slice was rejected because it had no explicit CAS
override; its artifacts were removed. The valid rerun used CAS `0.20`, zero
jitter, software decode, frame 48 for spatial scoring, and 12 warm-up plus 12
scored source-cadence frames for temporal scoring.

The retained spatial and temporal CSVs are
`benchmarks/quality_sweeps/resolution_720_to_1440_20260831.csv` and
`benchmarks/quality_sweeps/resolution_720_to_1440_temporal_20260831.csv`.
They cover all four real scenes, direct 2.00x versus 3.00x reconstruction, and
2560x1440 final delivery. Spatial means were 31.035636 / 0.875081 / 0.841841
(PSNR / SSIM / edge-SSIM) for 2.00x and 31.334488 / 0.877216 / 0.837970 for
3.00x. Temporal means were 0.799095 / 7.199238 (SSIM / temporal-delta signal)
for 2.00x and 0.878683 / 6.128250 for 3.00x. Peak VRAM averaged 10.98 GB and
11.10 GB spatially, and 11.48 GB and 11.69 GB temporally. The result repeats
the aggregate 3.00x temporal/spatial direction at 1440p but retains the edge
and memory regressions; it does not justify a universal 3.00x promotion.
Rendered media was removed after metric extraction under the accepted data-only
policy; CSVs, raw logs, hashes, and configuration provenance remain.
