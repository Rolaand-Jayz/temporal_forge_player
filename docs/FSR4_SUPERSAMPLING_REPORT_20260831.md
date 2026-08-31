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
deliberate `NO IMAGE` state for results that have not been rendered. Two
matched frame-48 PNGs are currently populated for the daylight current and
base-only controls; additional campaign writers should use
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

## Winner and runner-up

The aggregate numerical winner is 3.00x with Lanczos reduction. The practical
runner-up is 2.75x: it is close on spatial means and has the best non-native
temporal result, but is slower and more memory-intensive than direct 2x, and
still loses edge detail on some scenes. 2.25x is the synthetic edge/text
edge-SSIM winner but has no aggregate quality advantage. Direct 2x remains the
production-safe default because no candidate wins consistently across spatial,
temporal, difficult-material, artifact, and performance gates.
