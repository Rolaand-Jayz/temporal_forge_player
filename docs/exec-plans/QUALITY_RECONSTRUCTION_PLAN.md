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
- [x] Preserve representative stills.
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

---

# 23. Best-Known Configuration

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

# 24. Final Principle

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
