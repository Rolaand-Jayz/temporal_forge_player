# FSR 4.1 reconstruction-scale experiment — 2026-08-31

## Decision

The core hypothesis is partially supported on the four real 1280x720 scenes:
larger FSR output grids generally improved temporal SSIM and reduced the
frame-delta error after reduction. It is rejected as a universal first-class
quality mode at this time. The 3.00x arm won the aggregate real-scene temporal
and spatial SSIM means, but lost on the Sintel cave spatial and temporal slices,
lost edge-SSIM on the Tears of Steel daylight slice, and required materially
more output memory than direct 2x. The 640x360 synthetic edge/text fixture also
did not favor 3x.

The current player should therefore retain independent reconstruction and
delivery dimensions as diagnostic controls, but should not promote an
automatic overscaled-FSR mode or a universal 3x setting from this evidence.

## Reproducibility

- Player binary capture commit: `5814e70` (`Complete quality campaign evidence and hardening`).
- Experiment harness/report commit: `d214de3` (`Evaluate FSR supersampling for fixed 2x delivery`).
- Player: `build/temporal_forge_player`.
- Controls: current Temporal Forge architecture, native/non-generic path where
  available, software decode for repeatability, jitter off, identical source
  frame/cadence, current-control JSON, no generic graph/FP8 or simultaneous
  sharpening/residual/motion/color tuning.
- Final real-scene output: 2560x1440 from 1280x720 input.
- Intermediate outputs: 2560x1440, 2880x1620, 3200x1800, 3520x1980, and
  3840x2160 for 2.00x through 3.00x.
- First-pass reduction: Lanczos for every supersampled arm; direct 2x has no
  reduction. Every reference was generated from the lossless 2160p master at
  the same final resolution before scoring.
- Temporal protocol: 12 warm-up and 12 scored frames at source cadence,
  matched to the same 2560x1440 reference sequence.
- Runner scripts: `benchmarks/quality_sweeps/run_fsr_supersampling.py` and
  `benchmarks/quality_sweeps/run_fsr_supersampling_temporal.py`.

## Scale matrix — four real scenes

Means across Tears of Steel daylight/debris and Sintel rooftop/cave. Metrics
are computed only after all outputs are at 2560x1440.

| FSR scale | PSNR dB | SSIM | edge-SSIM | temporal SSIM | worst-frame SSIM | temporal delta error signal |
|---:|---:|---:|---:|---:|---:|---:|
| 2.00x | 31.035636 | 0.875081 | 0.841841 | 0.799095 | 0.730057 | 7.199238 |
| 2.25x | 31.065134 | 0.874749 | 0.842022 | 0.825874 | 0.775518 | 7.008871 |
| 2.50x | 31.084337 | 0.874515 | 0.841861 | 0.836122 | 0.794111 | 6.948461 |
| 2.75x | 31.095585 | 0.874748 | 0.841962 | 0.842738 | 0.805963 | 6.909451 |
| 3.00x | 31.334488 | 0.877217 | 0.837970 | 0.878683 | 0.863335 | 6.128250 |

The complete rows are in `benchmarks/quality_sweeps/fsr_supersampling_20260831.csv`
and `benchmarks/quality_sweeps/fsr_supersampling_temporal_20260831.csv`.

## Difficult-material check

The synthetic edge/text fixture was tested at 640x360 input and 1280x720
final output with the same five scales. Its SSIM means were 0.773239, 0.772696,
0.772868, 0.772651, and 0.771844 respectively. Edge-SSIM was highest at
2.25x (0.966840); 3x was lowest on SSIM. This is a direct counterexample to
selecting the largest scale solely from the aggregate real-scene result.

The real corpus slices cover architecture/diagonals, actor and debris motion,
occlusion/disocclusion, and low-light shadow detail. The edge/text fixture
covers thin geometry and text. No claim is made that this is exhaustive
coverage of every named material class; the result is intentionally a
no-promotion decision pending longer hair/foliage/repeating-texture coverage.

## Performance and memory

Steady-state GPU timing was sampled on Tears of Steel daylight at 1280x720
with the same controls: 2.00x 22.6 ms, 2.25x 27.4 ms, 2.50x 32.8 ms, 2.75x
38.7 ms, and 3.00x 8.4 ms. The 3x sample uses the existing fixed native
shape; the other scales use the generic path, so this is a path-boundary
effect and not evidence that 3x is intrinsically cheaper.

Peak discrete-GPU VRAM samples across the four real scenes were approximately
4.12, 4.46, 4.82, 5.26, and 4.29 GB for 2.00x through 3.00x. These are
system-level `mem_info_vram_used` peak samples, not allocator attribution;
the per-row before/peak values are retained in the spatial CSV. Sustained
real-time viability is therefore clear for direct 2x on the tested path, but
not established for the 2.25–2.75 generic arms at 24 fps; their measured
single-frame GPU times already exceed the 41.67 ms frame budget at 2.75x.

## Reduction-filter follow-up on the provisional 3x winner

| Reduction filter | spatial PSNR dB | spatial SSIM | temporal SSIM | temporal delta signal |
|---|---:|---:|---:|---:|
| Bicubic | 48.025052 | 0.967441 | 0.878632 | 6.126935 |
| Lanczos | 48.145315 | 0.967289 | 0.878683 | 6.128250 |

The filter result is a tie at this sample size: bicubic has a tiny SSIM
advantage, Lanczos a tiny PSNR advantage, and temporal behavior is effectively
unchanged. Full rows are in
`benchmarks/quality_sweeps/fsr_supersampling_filter_20260831.csv` and
`benchmarks/quality_sweeps/fsr_supersampling_filter_temporal_20260831.csv`.

## Artifact and comparison policy

The portable review surface is `review_harness/index.html`. It is a blank,
self-contained file-based viewer with deterministic canonical filenames and a
deliberate `NO IMAGE` state for results that have not been rendered. The
harness currently contains 125 validated frame-48 PNGs: the four real scenes
across the five reconstruction scales and 720p/1080p/1440p/2160p delivery
sets, finalist controls, NativeAA and conventional downscale controls, and
CAS-placement controls. Additional campaign writers should use
`tools/export_review_image.py` rather than hand-building names.

The requested evidence mode is data-only. Representative spatial comparison
is frame 48 for each real scene and the synthetic edge/text fixture; motion
comparison is the 12-frame post-warm-up sequence for each real scene. Their
dimensions, metrics, hashes, configuration, and timing provenance are
retained in the CSVs and runner artifacts. Rendered image payloads are not
required by this campaign, so no visual judgment or visual-artifact claim is
made from absent images. Flicker/shimmer and halo/ringing are represented by
the temporal delta and edge-SSIM signals; they are not treated as visually
verified findings.

## Pre-reduction CAS diagnostic

The placement runner evaluated 2.00x reconstruction above the 1920x1080
source, reduction to 1280x720, and both Lanczos and bicubic reducers across
the four real scenes. It compared CAS 0.20 before reduction, CAS 0.20 in the
Temporal Forge post-resolve pass (still before reduction), CAS 0.20 applied by
FFmpeg after reduction, and no CAS. The 32 finite rows are in
`benchmarks/quality_sweeps/fsr_downscale_cas_placement_20260831.csv` and the
runner records the effective placement in every row. The post-reduction arm is
an external CAS diagnostic, not the renderer's CAS implementation.

| placement | Lanczos mean SSIM | bicubic mean SSIM |
|---|---:|---:|
| CAS .20 before reduction | 0.928103 | 0.929559 |
| CAS .20 after FSR resolve, before reduction | 0.930604 | 0.931502 |
| CAS .20 after reduction (external FFmpeg) | 0.929713 | 0.931300 |
| no CAS | 0.930656 | 0.931505 |

On this source/output condition, CAS-before is worse after reduction. The
resolve-side CAS arm is effectively tied with no-CAS, so the data does not
support promoting either renderer-side placement. The external post-reduction
arm is below no-CAS on Lanczos and
bicubic in this slice, so it does not support adding a final sharpening stage.
This is still a still-image diagnostic; renderer-integrated post-reduction CAS
and a longer temporal placement matrix remain open.

The temporal follow-up is retained in
`benchmarks/quality_sweeps/fsr_downscale_cas_placement_temporal_20260831.csv`.
It covers 12 scored frames after a 12-frame warmup for the same four scenes,
four placements, and two reducers. Resolve-side CAS and no-CAS remained close:
mean SSIM was 0.943144/0.944414 for resolve-side CAS and 0.943366/0.944384
for no-CAS with Lanczos/bicubic respectively. External post-reduction CAS was
lower at 0.940866/0.942596 and produced a much larger temporal-delta signal
(44.938459/44.839386 versus 6.183832/6.147938 for no-CAS). This metric result
does not support post-reduction sharpening and does not substitute for a
renderer-integrated post-downsample stage.

## True downscaling follow-up

The first controlled true-downscaling slice is retained under
`benchmarks/quality_sweeps/fsr_downscale_nativeaa_20260831/`. It uses four real
1920x1080 inputs reduced to 1280x720, with NativeAA 1.0x and CAS 0.20 compared
against conventional Lanczos and bicubic controls. NativeAA was below both
controls on spatial SSIM for all four scenes; the matched 12-frame temporal
rows also had lower SSIM and greater temporal-delta absolute error than the
Lanczos control on every scene. This rejects NativeAA-assisted downscaling for
this tested condition while leaving other input/output ratios open.

## Resolution generalization follow-up

The first lower-source-resolution slice is retained in
`benchmarks/quality_sweeps/fsr_resolution_640_to_1280_20260831.csv`. It uses
the same four real scenes at 640x360 input and 1280x720 delivery, comparing
direct 2.00x reconstruction with 3.00x reconstruction reduced to the same
delivery size. Mean frame-48 PSNR/SSIM/edge-SSIM were 27.372917/0.778014/
0.687223 for 2.00x and 27.705603/0.782593/0.688645 for 3.00x. This slice
supports the 3.00x spatial advantage at this lower source resolution, but it
is spatial-only and does not establish a universal ratio or a
quality/performance winner.

The paired temporal slice is retained in
`benchmarks/quality_sweeps/fsr_resolution_640_to_1280_temporal_20260831.csv`.
It uses 12 warm-up and 12 scored frames per scene. Mean temporal SSIM was
0.708025 for 2.00x and 0.756272 for 3.00x; the temporal-delta signal was
6.828083 and 5.874093 respectively. This supports the same 3.00x direction
for this 640x360→1280x720 condition, while broader output-resolution coverage
and performance isolation remain open.

A second spatial slice is retained in
`benchmarks/quality_sweeps/fsr_resolution_1280_to_1920_20260831.csv`. At
1280x720 input and 1920x1080 delivery, mean frame-48 PSNR/SSIM/edge-SSIM were
31.202741/0.880638/0.823576 for 2.00x and 31.479691/0.882228/0.818113 for
3.00x. The SSIM gain and edge-SSIM loss make this a mixed result; it does not
support a universal 3.00x promotion.

The paired temporal slice is retained in
`benchmarks/quality_sweeps/fsr_resolution_1280_to_1920_temporal_20260831.csv`.
It uses 12 warm-up and 12 scored frames per scene. Mean temporal SSIM was
0.803889 for 2.00x and 0.851838 for 3.00x; the temporal-delta signal was
7.128723 and 6.795694 respectively. This supports the 3.00x temporal result
for this 1280x720→1920x1080 condition, while the edge-SSIM loss and
performance cost remain part of the tradeoff.

A matched 1440p delivery slice is retained in
`benchmarks/quality_sweeps/resolution_720_to_1440_20260831.csv` and
`benchmarks/quality_sweeps/resolution_720_to_1440_temporal_20260831.csv`.
It covers all four real 1280x720 scenes at 2560x1440 delivery, with direct
2.00x and 3.00x reconstruction, CAS 0.20 explicitly supplied to both runners,
and the same 12-warm-up/12-scored temporal protocol. Spatial means were:

| scale | PSNR dB | SSIM | edge-SSIM |
|---:|---:|---:|---:|
| 2.00x | 31.035636 | 0.875081 | 0.841841 |
| 3.00x | 31.334488 | 0.877216 | 0.837970 |

Temporal means were 0.799095 / 7.199238 for 2.00x and 0.878683 / 6.128250
for 3.00x (SSIM / temporal-delta signal). Peak VRAM averaged approximately
10.98 GB and 11.10 GB in the spatial captures, and 11.48 GB and 11.69 GB in
the temporal captures. The 3.00x direction therefore repeats at 1440p for
aggregate SSIM and temporal stability, while its edge-SSIM remains lower and
its memory cost higher. The evidence supports resolution-dependent diagnostic
controls, not automatic 3.00x promotion. Rendered media was removed after
metric extraction under the campaign's data-only evidence policy; raw CSVs,
logs, hashes, and provenance remain.

A 2160p delivery slice is also retained in
`benchmarks/quality_sweeps/resolution_720_to_2160_20260831.csv`,
`benchmarks/quality_sweeps/resolution_720_to_2160_temporal_20260831.csv`, and
`benchmarks/quality_sweeps/review_images_720_to_2160_20260831.csv`. The spatial
data covers all four real scenes at 2.00x, 2.25x, 2.50x, 2.75x, and 3.00x;
the paired temporal data currently covers 2.00x and 3.00x. For those temporal
rows, mean SSIM / temporal-delta signal were 0.880952 / 6.133414 at 2.00x and
0.827792 / 6.725866 at 3.00x. Spatial mean SSIM across the five scales was
0.891968, 0.892311, 0.892365, 0.892589, and 0.892540 respectively. This is
mixed 2160p evidence and does not support a universal 3.00x claim. The
five-ratio 2160p temporal matrix and clean performance isolation remain open.

## Required-question closure matrix

The following answers are limited to the retained evidence. “Open” means the
brief requires a broader or differently instrumented experiment than the
current data provides.

| # | answer | evidence status |
|---:|---|---|
| 1 | In the paired 1280×720→1920×1080 capture, CAS 0.04→0.20 changed spatial SSIM by 0.000000 for base-only bilinear and +0.000003 for current; over 12 scored temporal frames it changed mean SSIM by −0.001629 and −0.002314 respectively. | Supported by paired spatial and temporal captures; the change is not an improvement. |
| 2 | Yes, current remains the temporal default in the cadence-clean M6 evidence. | Supported by the M6 cadence/performance record. |
| 3 | Yes, base-only bilinear remains the strongest spatial control in M6. | Supported by the M6 spatial matrix. |
| 4 | No corrected-CAS evidence justifies changing the M6 no-promotion conclusion. | Supported for the tested controls; broader CAS delta remains open. |
| 5 | Above-delivery reconstruction can improve aggregate metrics, but not consistently. | Supported by the 2×–3× scale matrix. |
| 6 | Yes, the earlier 3× temporal advantage survives at CAS 0.20 in the 1280×720→2560×1440 matrix. | Supported, with scene exceptions. |
| 7 | No, 3× is not a universal best ratio. | Supported by cave, edge, and performance regressions. |
| 8 | 2.75× is the practical runner-up in the original matrix; a new cross-resolution cost frontier is open. | Partially supported. |
| 9 | CAS before reduction is not better in the tested 1920×1080→1280×720 slice. | Supported by spatial and temporal placement CSVs. |
| 10 | External CAS after reduction is not better; renderer-integrated post-CAS is not implemented/validated. | Supported for the external diagnostic; integrated arm open. |
| 11 | No edge recovery from post-CAS was demonstrated. | Supported by the placement edge/SSIM outcome; broader edge classes open. |
| 12 | The no-CAS control is effectively tied with resolve-side CAS, while pre-CAS is lower in the tested reduction. | Supported for this condition. |
| 13 | The filter contribution is small and condition-dependent in the retained sweeps. | Supported; no universal reducer winner. |
| 14 | Neither filter wins universally: bicubic has tiny SSIM advantages in some rows, Lanczos tiny PSNR/temporal advantages elsewhere. | Supported by filter follow-up. |
| 15 | Yes, the preferred ratio can change with source resolution: 3× helps at 640×360, while 1280×720→1920×1080 is mixed on edge quality. | Supported for two tested source conditions. |
| 16 | The 3.00x aggregate direction repeats at 1440p, while the 2160p paired 2.00x/3.00x slice is mixed and does not establish a universal ratio. | 1440p paired coverage and partial 2160p coverage supported; a complete five-ratio 2160p temporal matrix remains open. |
| 17 | NativeAA-assisted 1920×1080→1280×720 did not beat conventional controls. | Supported for the tested true-downscale condition. |
| 18 | Above-source reconstruction followed by below-source reduction has not shown a broad advantage over ordinary downsampling. | Open beyond the tested condition and controls. |
| 19 | No single highest-quality configuration is justified across all gates. | Supported by conflicting spatial, temporal, edge, and performance results. |
| 20 | Direct 2× remains the best-supported quality/performance balance on the RX 7900 GRE. | Supported by current performance evidence; new arms lack complete timing isolation. |
| 21 | Yes, source, reconstruction, and delivery dimensions should remain independent diagnostic controls. | Supported by the matrix behavior; product-mode promotion remains deferred. |

This matrix is a status report, not a claim that the open gates are complete.

## CAS-strength isolation

`benchmarks/quality_sweeps/cas_strength_pair_20260831.csv` contains a matched
16-row capture of `current` and `base_only_bilinear` at CAS 0.04 and 0.20.
All rows use the same four 1280x720 high-quality sources, frame 48, 1920x1080
output, Quality Lab configs, and binary identity. Across the four-scene means,
the current path changed from PSNR 31.053447 / SSIM 0.879059 / edge-SSIM
0.829815 at 0.04 to 31.053407 / 0.879062 / 0.829798 at 0.20. Base-only
bilinear was identical at the reported precision: 31.279136 / 0.881645 /
0.826906 at both strengths. The corrected CAS policy therefore does not alter
the spatial finalist tradeoff in this paired condition.

The companion temporal capture
`benchmarks/quality_sweeps/cas_strength_pair_temporal_20260831.csv` contains
32 rows: the same two candidates and strengths across four scenes, 12 scored
frames after 12 warmup frames, and both Lanczos and bicubic reducers. At 0.04,
mean temporal SSIM was 0.879464 for current and 0.881754 for base-only
bilinear; at 0.20 it was 0.877150 and 0.880125. Thus 0.20 reduced mean
temporal SSIM by 0.002314 for current and 0.001629 for base-only bilinear,
while increasing the corresponding mean temporal-delta signals by 0.120523
and 0.112248. This closes the paired CAS-strength comparison for this
condition and provides no basis for increasing CAS to 0.20.

## Winner and runner-up

The aggregate numerical winner is 3.00x with Lanczos reduction. The practical
runner-up is 2.75x: it is close on spatial means and has the best non-native
temporal result, but is slower and more memory-intensive than direct 2x, and
still loses edge detail on some scenes. 2.25x is the synthetic edge/text
edge-SSIM winner but has no aggregate quality advantage. Direct 2x remains the
production-safe default because no candidate wins consistently across spatial,
temporal, difficult-material, artifact, and performance gates.
