# Real-world corpus results

Measured 2026-07-17 through 2026-07-18 on an RX 7900 GRE using the native RDNA3
INT8 path. Quality measurements use frame 48 and the matching lossless native-4K
frame.

## 2026-08-25 tone and base-resolve applicability checks

Two apparently live controls were checked with the retained lead: tone
`exposureEV=-0.015` through an explicit `mode=current` configuration, and
`TFORGE_FSR4_EXPERIMENTAL_BASE_UNJITTERED=1`. Both four-scene captures used
36 warm-up and 24 scored frames at 426x240 -> 1920x1080.

The source audit shows both controls are consumed only by the experimental
composition branch. The maintained `current` branch bypasses that branch, so
neither control affects the output. The two captures were byte-identical to
each other at the reported metrics, confirming a no-op rather than a quality
win. They are not candidates for promotion. Raw captures are under
`.quality-tmp/tone-minus015-*-20260825/` and
`.quality-tmp/base-unjittered-*-20260825/`.

## 2026-08-25 confidence-blend midpoint

The live confidence path was tested at `TFORGE_FSR4_LEARNED_CONFIDENCE_BLEND=0.5`
with the retained `.075` linear-blend, RCAS `.12`, and CAS `.01` settings. This
is the midpoint between the gated path and the retained ungated lead. Against
the lead, SSIM changed by `+0.000360`, `-0.000417`, `+0.011346`, and `+0.003561`
for Tears daylight, Tears debris, Sintel rooftop, and Sintel cave. Temporal
absolute-error changes were `+0.075774`, `+0.035416`, `-0.021186`, and
`+0.106928`. Because three scenes regressed temporally, the midpoint is
rejected and remains opt-in. Raw captures are under
`.quality-tmp/confidence-blend050-*-20260825/`.

The lighter `TFORGE_FSR4_LEARNED_CONFIDENCE_BLEND=0.75` point was then
checked with the same protocol. Relative to the ungated lead, SSIM changed
by `+0.000375`, `-0.000401`, `+0.011369`, and `+0.003582`; temporal-error
changes were `+0.072787`, `+0.035957`, `-0.025495`, and `+0.107315` for the
same four scenes. It is rejected for the same three-scene temporal
regression pattern. Raw captures are under
`.quality-tmp/confidence-blend075-*-20260825/`.

## 2026-08-25 learned-strength 0.10 final-lead recheck

The older learned-strength `0.10` result was rerun with the retained final
settings: linear blending, ungated confidence, RCAS `0.12`, and CAS `0.01`.
Relative to the `.075` lead, SSIM changed by `+0.000429`, `-0.000336`,
`+0.011502`, and `+0.003762`; temporal-error changes were `+0.034144`,
`+0.038686`, `-0.061004`, and `+0.110583` for Tears daylight, Tears debris,
Sintel rooftop, and Sintel cave. The three-scene temporal regression rejects
`.10` despite its spatial gains. Raw captures are under
`.quality-tmp/learned010-*-20260825/`.

## 2026-08-25 learned-kernel radius/sigma probe

The maintained path was tested with learned-kernel `radius=0.75` and
`sigma=0.35`, while retaining learned strength `.075`, linear blending,
ungated confidence, RCAS `.12`, and CAS `.01`. Relative to the retained lead,
SSIM changed by `+0.000377`, `-0.000407`, `+0.011369`, and `+0.003596`;
temporal-error changes were `+0.068865`, `+0.035062`, `-0.031809`, and
`+0.106873` for Tears daylight, Tears debris, Sintel rooftop, and Sintel
cave. The three-scene temporal regression rejects this kernel point. Raw
captures are under `.quality-tmp/kernel075-*-20260825/`.

## 2026-08-25 input color-path checks

Two isolated input-path candidates were captured on the maintained native
INT8 path using software decode, the clean `learnedStrength=0.05` baseline,
linear-light current blending, 36 warm-up frames, and 24 scored frames for
each of the four real-world scenes. No playback default was changed.

The bilinear chroma-filter candidate was mixed spatially and worse temporally
on three scenes. Relative to the matched control, its SSIM deltas were
`-0.000054`, `-0.000024`, `+0.000171`, and `+0.000047`; temporal-error deltas
were `+0.000863`, `+0.000539`, `+0.003323`, and `-0.000667` for Tears daylight,
Tears debris, Sintel rooftop, and Sintel cave respectively. GPU timing stayed
within normal run-to-run variation at about `2.44-2.46 ms`. It is rejected.
Raw captures and logs are under
`.quality-tmp/chroma-bilinear-linear005-20260825/`.

The Rec.709 input-transfer candidate was also rejected. It improved temporal
error on rooftop and cave, but reduced SSIM on all four scenes by
`0.008044`, `0.018113`, `0.007749`, and `0.023907`; it also worsened temporal
error on daylight and debris by `0.048250` and `0.106627`. GPU timing remained
about `2.45 ms`, so this is a quality regression rather than a performance
tradeoff. Raw captures and logs are under
`.quality-tmp/input-rec709-linear005-20260825/`.

## 2026-08-25 edge-adaptive learned blend check

The live native composition was tested with
`TFORGE_FSR4_EDGE_ADAPTIVE_LEARNED=1` and an edge suppression strength of
`0.70`, against the matched clean `0.05` linear-blend control. The candidate
reduced SSIM by `0.000020`, `0.000038`, `0.000025`, and `0.000029` on Tears
daylight, Tears debris, Sintel rooftop, and Sintel cave. Temporal error also
worsened on the first three scenes by `0.012621`, `0.002114`, and `0.014166`;
cave improved by only `0.001797`. GPU timing increased from roughly
`2.45 ms` to `2.51 ms`. It is rejected and remains diagnostic-only.
Raw captures and logs are under
`.quality-tmp/edge-adaptive-linear005-20260825/`.

The same edge-adaptive probe was then paired with the strongest retained
linear-blend screen, `TFORGE_FSR4_LEARNED_STRENGTH=0.075`. Against its matched
`.075` linear control, SSIM fell by `0.000020`, `0.000048`, `0.000022`, and
`0.000040`; temporal error worsened on daylight, debris, and rooftop by
`0.022895`, `0.003473`, and `0.025242`, while cave improved by only
`0.002393`. GPU timing was about `2.51 ms`. This combination is rejected too.
Raw captures and logs are under
`.quality-tmp/edge-adaptive-linear0075-20260825/`.

## 2026-08-25 confidence-ungated learned blend candidate

The live native path was tested with
`TFORGE_FSR4_LEARNED_STRENGTH=0.075`, linear-light blending, and
`TFORGE_FSR4_DISABLE_LEARNED_CONFIDENCE_GATE=1`. A 24-frame screen improved
mean SSIM by `+0.000088` and mean temporal error by `-0.006190` against its
matched gated control. The longer 48-frame confirmation held the direction:
mean SSIM improved from `0.870897` to `0.871083`, and mean temporal error
improved from `0.726122` to `0.721020`.

The long run improved SSIM on all four scenes. Temporal error improved on
Tears daylight (`-0.011563`) and Sintel rooftop (`-0.015367`), while changing
slightly worse on Tears debris (`+0.003487`) and Sintel cave (`+0.003035`).
No measured GPU cost increase was observed. This is retained as an opt-in
candidate for visual review, not promoted as a global default. Raw captures
and logs are under
`.quality-tmp/confidence-ungated-linear0075-long48-20260825/`.

Adding `TFORGE_FSR4_MOTION_CONFIDENCE_REACTIVE=1` on top of the ungated
`.075` candidate was a no-op. Across the four 24-frame pairs, SSIM was
unchanged to the reported precision and temporal-error changes were at most
`0.000032`; GPU timing remained about `2.45-2.46 ms`. This stack is rejected.
Raw captures and logs are under
`.quality-tmp/confidence-ungated-reactive-linear0075-20260825/`.

## 2026-08-25 confidence-ungated blend plus CAS 0.02

The confidence-ungated `.075` candidate was paired with a restrained final
CAS pass using `TFORGE_FSR4_CAS_STRENGTH=0.02`. The 24-frame screen showed
slightly lower SSIM but lower temporal error on all four scenes. A longer
48-frame confirmation held the temporal result: mean temporal error improved
from `0.721019` to `0.718570`. Per-scene temporal improvements were daylight
`-0.002340`, debris `-0.002819`, rooftop `-0.003696`, and cave `-0.000943`.

Mean SSIM moved from `0.870967` to `0.870949`, with small per-scene losses of
`0.000011-0.000024`. The extra pass remained inexpensive in the measured
captures. This is retained as an opt-in temporal-stability candidate, not a
global default, because its spatial tradeoff still needs human review. Raw
captures and logs are under
`.quality-tmp/confidence-ungated-cas002-linear0075-long48-20260825/`.

An adjacent CAS sweep found a better point at strength `0.01`. In the 24-frame
screen, CAS `.01` improved temporal error on all four scenes with a mean
change of `-0.001028`, while costing only `-0.000008` mean SSIM; CAS `.03`
had a larger spatial cost. The 48-frame confirmation held the result:
mean temporal error improved from `0.721012` to `0.719968`, with per-scene
improvements of `-0.001024`, `-0.001189`, `-0.001499`, and `-0.000464`.
Mean SSIM moved from `0.870967` to `0.870958`; every per-scene loss was at
most `0.000012`. This is the best measured opt-in tradeoff so far, retained
for visual review rather than promoted globally. Raw captures and logs are
under `.quality-tmp/confidence-ungated-cas001-linear0075-long48-20260825/`.

The same lead was checked at 1280x720 input to 3840x2160 output. In the
24-frame four-scene run, mean SSIM changed from `0.944685` to `0.944675`,
while mean temporal error improved from `0.389677` to `0.388791`. Temporal
error improved on daylight, rooftop, and cave, with a small debris regression
of `+0.000433`. The candidate therefore generalizes to the larger upscale
tier without a meaningful spatial or timing penalty. Raw captures and logs
are under `.quality-tmp/confidence-ungated-cas001-1280-20260825/`.

A fine CAS sweep around `.01` confirmed the tradeoff. At 24 frames, `.005`
changed mean SSIM by `-0.000004` and temporal error by `-0.000437`; `.015`
changed them by `-0.000013` and `-0.001681`. The `.005` setting preserves a
little more spatial score but gives back about half the temporal benefit,
while `.015` costs roughly three times as much SSIM as `.005`. The previously
confirmed `.01` point remains the best balanced setting. Raw captures are
under `.quality-tmp/confidence-ungated-cas-fine-linear0075-20260825/`.

The independent legacy RCAS stage was then swept around its existing `0.08`
strength while holding the `.01` lead constant. RCAS `0.04` worsened mean
temporal error by `+0.000195`; RCAS `0.12` improved it by `-0.000194` in the
screen. A 48-frame confirmation of RCAS `0.12` held the direction: mean
temporal error improved from `0.719971` to `0.719771`, with per-scene changes
of `-0.000099`, `-0.000248`, `-0.000294`, and `-0.000160`. Mean SSIM changed
only from `0.870958` to `0.870955`. The opt-in lead is now
`.075 ungated + linear blend + legacy RCAS .12 + CAS .01`; it remains
unpromoted pending visual review. Raw captures are under
`.quality-tmp/confidence-ungated-rcas012-linear0075-cas001-long48-20260825/`.

An opt-in postpass current-frame weight of `0.10` was tested on that full
lead. It produced mixed temporal behavior: daylight worsened by `+0.000282`,
debris improved by `-0.000583`, rooftop was effectively unchanged, and cave
improved by `-0.000369`. SSIM changes were also mixed and all below
`0.000006`. This is not a corpus-wide improvement and is rejected. Raw
captures are under
`.quality-tmp/confidence-ungated-currentweight010-linear0075-rcas012-cas001-20260825/`.

## 2026-08-21 reconstruction-quality campaign addendum

The current quality finalist is `base_only` with a bilinear stable base,
disabled learned residual and adaptive sharpening, corrective exposure
`-0.015 EV`, neutral contrast/pivot/gamma, and bicubic presentation. The
staircase investigation found that pure learned output retained stronger
colored stepped edges, while the stable base was the strongest controlled
path; sharper kernels, residuals, and adaptive sharpening did not remove the
artifact at an acceptable quality/performance tradeoff. A separate post-fix
shader change removes Halton jitter from the base-only spatial resolve, which
eliminated the measured shimmer without changing the neural graph.

The post-fix six-row full-reference 426x240 finalist run measured mean FSR
PSNR/SSIM/edge-SSIM `28.342274 / 0.751456 / 0.797694`, luma MAE `0.012008`,
and signed bias `+0.002138`; the quality-capture timing was `1.429 ms` GPU and
`1.546 ms` pipeline CPU. The three-clip local neighborhood winner measured
`24.101126 / 0.737582 / 0.713305` and `1.425 / 1.539 ms`. These aggregates
are reported with their exact source subsets and are not a claim of universal
superiority over spatial controls.

Dedicated performance traces for the same stable path recorded GPU means of
`0.971 ms` at 426x240 input, `0.966 ms` at 640x360, `3.985 ms` at 854x480,
and `3.994 ms` at 1280x720, all below the 5 ms target in the measured runs.
The supersampled anti-aliasing run and same-frame libplacebo/FSRCNNX controls
are preserved under `/tmp/tforge-supersampled-aa-20260821b/` and
`/tmp/tforge-competitors-20260821/`. The installed mpv build did not provide
an FSR1 scaler, so no FSR1 result is represented.

The temporal runner now includes a direct bilinear control. Post-fix
FSR-vs-bilinear temporal-delta error was `0.005165` on Tears of Steel,
`0.291354` on Sintel, and `0.014183` on a moving synthetic control; visual
strips showed no new shimmer or ghost trails. The checked-in synthetic-motion
generator currently produces a static sequence and is excluded from this
temporal conclusion until its generator is corrected.

## Quality correction

The checkerboard-like edge blending had two spatial causes:

- GPU YUV conversion used approximate normalized limited-range offsets instead
  of the exact 8-bit code values `16/255` and `128/255`. This primarily damaged
  red/blue structural similarity.
- The learned anisotropic postpass fully replaced the stable current-frame
  reconstruction. A 55% learned contribution retains the INT8 model's edge
  recovery while suppressing unstable alternating filter directions.

Centered bicubic 4:2:0 chroma reconstruction now covers both the regular
planar-YUV and DRM/NV12 GPU conversion paths; it replaces nearest 2x2 chroma
replication and the former DRM-only bilinear fallback. Forced-reset testing changed the worst pre-fix SSIM by only
`-0.00034`, ruling out temporal history as the primary defect.

The current preset sweep also verifies that the multiplier changes the neural
input size while keeping the presentation target fixed. On a 1920x1080 source
with a 1280x720 output, the measured inputs were Native 1280x720, Quality
854x480, Balanced 754x424, Performance 640x360, and Ultra Performance 428x240.
On the daylight CRF-12 frame-60 sample, the corrected runner selected the
expected dimensions and kept the 1280x720 presentation target fixed. The
measured FSR SSIM values were Quality 0.887572, Balanced 0.893219,
Performance 0.894729, Ultra Performance 0.870226, and NativeAA 0.879589;
edge SSIM was strongest for NativeAA (0.767988) and Quality (0.772393), while
Ultra Performance fell to 0.653599. This is a single-frame diagnostic, not a
default-selection recommendation; the full corpus remains the acceptance
gate. The corresponding corrected outputs are tagged `*PostFix` under
`results/quality_frames` and should be repeated after model or postpass
changes.

Decoded color range and matrix metadata now reach the GPU conversion pass.
Limited/full range and BT.601, BT.709, BT.2020, FCC, and SMPTE 240M matrices
are converted with per-frame coefficients instead of treating every video as
limited-range BT.709. A real Sintel 240p scene transcoded to full-range
BT.601 produced `0.985667` raw-frame SSIM against FFmpeg's decoded RGB. The
media integration fixture is explicitly tagged BT.709 limited and now fails
if the decoder drops either value.

An attempted switch from centered chroma reconstruction to the corpus's
declared left siting was rejected after the full 240p matrix: mean SSIM fell by
`0.000736`, and direct decoded-frame comparison also favored the existing
centered reconstruction. The corpus tags are therefore not being used as
evidence for a chroma-location output change without an independently verified
source.

## Quality chart

`Delta` is FSR minus Lanczos. Positive is better.

| Scene | Compression | Before SSIM delta | Corrected SSIM delta | Corrected edge delta |
|---|---:|---:|---:|---:|
| Tears of Steel daylight | CRF 12 | -0.1284 | -0.0011 | -0.0115 |
| Tears of Steel daylight | CRF 23 | -0.1241 | -0.0020 | -0.0056 |
| Tears of Steel daylight | CRF 35 | -0.1164 | -0.0019 | +0.0007 |
| Tears of Steel debris | CRF 12 | -0.1316 | -0.0007 | -0.0008 |
| Tears of Steel debris | CRF 23 | -0.1287 | -0.0016 | +0.0009 |
| Tears of Steel debris | CRF 35 | -0.1239 | -0.0034 | +0.0024 |
| Sintel rooftop | CRF 12 | -0.1618 | +0.0024 | +0.0025 |
| Sintel rooftop | CRF 23 | -0.1607 | -0.0013 | +0.0022 |
| Sintel rooftop | CRF 35 | -0.1551 | -0.0057 | +0.0025 |
| Sintel cave | CRF 12 | -0.1585 | +0.0830 | +0.0026 |
| Sintel cave | CRF 23 | -0.1598 | +0.0844 | +0.0028 |
| Sintel cave | CRF 35 | -0.1767 | +0.0640 | +0.0008 |
| **Mean** | | **-0.1438** | **+0.0180** | **-0.00004** |

The corrected full matrix is `results/quality_720p_corrected.csv`. The earlier
matrix is retained as `results/quality_720p.csv` for before/after comparison.

## Performance chart

The original 18-clip matrix showed that GPU execution is content-independent
while upload/preparation varies by scene and compression. GPU means were
`6.14-6.20 ms`; full pipeline means were `8.85-9.94 ms`.

Representative before/after measurements show no GPU regression from the
quality correction:

| Scene, CRF 23 | Before GPU ms | Corrected GPU ms | Corrected pipeline ms |
|---|---:|---:|---:|
| Tears of Steel daylight | 6.1700 | 6.1639 | 9.6742 |
| Big Buck Bunny branches | 6.1522 | 6.1500 | 8.8387 |
| Sintel cave | 6.1590 | 6.1578 | 9.3559 |

The full baseline is `results/performance_720p.csv`; the corrected subset is
`results/performance_corrected_subset.csv`.

## Low-resolution quality

At Ultra Performance, near-16:9 sources through 360p now use the generated
1920x1080 native graph and sources above 360p through 1080p use a generated
3840x2160 native graph. The graph model matches the requested scale: 3x inputs
use Ultra Performance and 2x inputs use Performance, including that model's
distinct initializer. Other aspect ratios and player scale settings retain the
dimension-preserving generic path. The quality runner detects the actual output
size and scales both the native reference and Lanczos control to it.

The fixed 55% learned blend overweights unstable anisotropic directions on
small sources. The first correction used a 15% learned contribution through
480p, preserving INT8 model detail while strongly reducing checkerboard-like
edge blending.

| Input | Output | 55% mean SSIM delta | 15% mean SSIM delta | 55% mean edge delta | 15% mean edge delta |
|---:|---:|---:|---:|---:|---:|
| 426x240 | 1278x720 | -0.023463 | **-0.018763** | +0.002360 | **+0.005433** |
| 640x360 | 1920x1080 | -0.037322 | **-0.032450** | +0.001713 | **+0.004093** |
| 854x480 | 2562x1440 | -0.029245 | **-0.022212** | -0.010828 | **-0.003590** |

Each row averages four scenes at CRF 12, 23, and 35. Full results are in
`results/quality_240p_blend015.csv`, `results/quality_360p_blend015.csv`, and
`results/quality_480p_blend015.csv`. Matching FSR, Lanczos, reference, and
difference frames are under `results/quality_frames`; tagged sweep artifacts
use `stable`, `blend015`, and strength tags such as `s015`.

The new 1920x1080 native graph was also measured across all twelve 240p
reference cases. Its mean SSIM delta against same-size Lanczos is `-0.016472`
and its mean edge-SSIM delta is `+0.004652`. Edge delta is positive in every
scene/compression case. The full matrix is
`results/quality_240p_native1080.csv`; tagged output, Lanczos, reference, and
difference frames use the `native1080full` suffix.

Decoder diagnostics at frame 48 confirmed that channels 0-3 are correctly
read as packed NHWC filter parameters. The recovered prepass contract now
uses `0.1 * center_weight` rather than a fixed 0.1 current-frame history
weight, and reset/offscreen history follows the recovered shader. The fused
postpass also reconstructs the recovered direct history blend without adding
another pass or target-sized intermediate.

Full learned replacement still regresses real video, while 0% would hide the
model. A scale-aware nonzero contribution performs better: 15% at 3x, ramping
to 5% at 4x and above for sources through 480p. Both 240p and 480p use about
4.5x native output and select 5%; 360p uses 3x and retains 15%.

| Input | Previous SSIM delta | Scale-aware SSIM delta | Previous edge delta | Scale-aware edge delta | Positive edge cases |
|---:|---:|---:|---:|---:|---:|
| 426x240 | -0.016752 | **-0.016476** | +0.004562 | **+0.005166** | 12/12 |
| 640x360 | **-0.032450** | -0.033895 | **+0.004093** | +0.004048 | 12/12 |
| 854x480 | +0.015940 | **+0.016291** | -0.000985 | **-0.000539** | 8/12 |

The 360p 5% diagnostic measured `-0.033935` SSIM delta and `+0.004103` edge
delta. Its negligible edge gain does not justify the SSIM loss, so the 3x path
retains 15%. New full matrices are
`results/quality_240p_strength005_full.csv`,
`results/quality_360p_scaleaware_default.csv`, and
`results/quality_480p_strength005_full.csv`. The 5% 360p diagnostic remains in
`results/quality_360p_strength005_full.csv`; prior metadata-aware and native
matrices remain available for direct comparison.

The source model-color and display-history images now use packed RGB10_A2
instead of RGBA32F/RGBA16F. Prepass display-color feedback is disabled by
default because it adds target-resolution Catmull-Rom sampling without a
measurable video-quality benefit; the causal postpass model history remains
active. Across the twelve 240p cases, the final configuration measures mean
SSIM delta `-0.016498`, mean edge delta `+0.005170`, and positive edge delta in
12/12 cases. The unpacked-source baseline was `-0.016476` and `+0.005166`, so
the changes are metric-neutral within `0.000022`. The complete matrix and
comparison frames are in `results/quality_240p_final_round2.csv`.

### Low-resolution performance

Low-resolution Ultra Performance output now bypasses the generic 39-dispatch
graph. The 1080p and 2160p fixed-shape packs each execute the generated 14-pass
INT8 graph with shape-specific dispatch dimensions and scratch allocation.

| Input | Native output | Prior GPU ms | Current GPU ms | Current GPU p95 ms | Current pipeline ms |
|---:|---:|---:|---:|---:|---:|
| 426x240 | 1920x1080 | 1.0639 | **0.9615** | 1.265 | 1.1094 |
| 640x360 | 1920x1080 | 1.0873 | **0.9643** | 1.261 | 1.1316 |
| 854x480 | 3840x2160 | 4.2542 | **3.8522** | 3.886 | 4.1236 |
| 1280x720 | 3840x2160 | 4.7624 | **3.8635** | 3.887 | 4.1988 |

The prior 240p corpus matrix covers all six scenes at CRF 12, 23, and 35. Its
pipeline means span only `1.2250-1.2536 ms`; the worst pipeline p95 is
`1.612 ms`. Compression level has no material effect: high, medium, and low CRF
groups average `1.2472`, `1.2430`, and `1.2367 ms`, respectively. The current
rows above are controlled Tears of Steel daylight CRF 23 samples. The generic baseline is
`results/performance_lowres.csv`; current measurements are in
`results/performance_240p_full.csv` and
`results/performance_lowres_native_tiers.csv`. The scale-aware fused-postpass
smoke data is `results/performance_lowres_scaleaware_smoke.csv`; its 240p GPU
mean is `1.0642 ms`, effectively unchanged from the full-matrix `1.0639 ms`.
The final packed-image and exact-source-tile smoke measurements are in
`results/performance_rgb10_final.csv`. Against the prior native path, GPU mean
improves by 9.6% at 240p, 11.3% at 360p, 9.4% at 480p, and 18.9% at 720p.
Every measured low resolution remains comfortably inside a 16.67 ms frame
budget at p95.

## 1080p Performance tier

Near-16:9 1920x1080 input at Ultra Performance now resolves to the native
3840x2160 Performance graph instead of requesting a generic 5760x3240 graph.
The 14-pass Performance shader pack and its exact initializer are loaded
together. Shader packs are loaded lazily, and Vulkan pipeline data is retained
in `${XDG_CACHE_HOME:-$HOME/.cache}/temporal-forge-player/native-int8.bin`.
Warm graph readiness measured approximately 235 ms; this startup cost is not
included in steady-state frame timings.

Frame 48 quality across four scenes and CRF 12, 23, and 35:

| Metric versus same-size Lanczos | Mean | Worst | Positive samples |
|---|---:|---:|---:|
| SSIM delta | **+0.019630** | -0.005943 | 6/12 |
| Edge-SSIM delta | **+0.001974** | +0.000613 | 12/12 |

The complete matrix is `results/quality_1080p_performance.csv`. A Tears of
Steel daylight smoke result is retained in
`results/quality_1080p_performance_smoke.csv`; tagged output and difference
frames use the `perf2160` and `perf2160full` suffixes.

Steady-state timing across all six CRF 23 scenes:

| Metric | Mean | Minimum scene mean | Maximum scene mean | Worst scene p95 |
|---|---:|---:|---:|---:|
| Full pipeline | 12.7634 ms | 12.0358 ms | 13.4955 ms | **15.295 ms** |
| GPU graph | 6.2800 ms | 6.2706 ms | 6.2922 ms | **6.331 ms** |

Every measured scene remains inside a 16.67 ms 60 fps frame budget at p95.
The full timing matrix is `results/performance_1080p_medium.csv`.

## Quality preset native tiers

The Quality preset now uses model-matched 14-pass INT8 graphs for the two exact
generated shapes: 1280x720 to 1920x1080 and 2560x1440 to 3840x2160. Other
Quality-preset dimensions retain the generic graph rather than running a shader
pack with incorrect tensor strides or scratch offsets.

For 720p to 1080p, a controlled Tears of Steel daylight CRF 23 comparison
measured the following steady-state frame times:

| Path | GPU mean | Full pipeline mean | Full pipeline p95 |
|---|---:|---:|---:|
| Generic 39-dispatch | 37.8055 ms | 41.7507 ms | 43.378 ms |
| Native 14-pass Quality | **1.6397 ms** | **4.8278 ms** | **5.507 ms** |

Across all six CRF 23 scenes, the native Quality graph remained
content-independent: GPU mean was `1.6446 ms`, the scene-mean range was
`1.6352-1.6519 ms`, and the worst scene GPU p95 was `1.675 ms`. Full pipeline
mean was `4.7552 ms`, with a worst scene p95 of `5.657 ms`. The controlled
comparison is in `results/performance_720p_quality_native_smoke.csv` and
`results/performance_720p_quality_generic_smoke.csv`; the six-scene result is
`results/performance_720p_quality_native.csv`.

Frame 48 quality across four scenes and CRF 12, 23, and 35:

| Metric versus same-size Lanczos | Mean | Range | Positive samples |
|---|---:|---:|---:|
| SSIM delta | -0.029588 | -0.122530 to -0.001991 | 0/12 |
| Edge-SSIM delta | -0.000544 | -0.008019 to +0.003626 | 7/12 |

The complete matrix is `results/quality_720p_quality_native.csv`. Most non-cave
samples are within `0.002-0.007` SSIM of Lanczos. Sintel cave accounts for the
large overall-SSIM tail while retaining positive edge SSIM and higher PSNR than
Lanczos at every tested compression level. Manual comparison of the tagged FSR
and Lanczos frames found stable detail and no green or checkerboard output, so
this is recorded as a dark-scene tone/color discrepancy rather than hidden as
an aggregate quality win. Learned-blend sweeps from 15% through 55% changed the
cave delta by less than `0.001`, ruling out blend strength as the primary cause.

### GPU codec-motion expansion

Stage profiling identified full-resolution CPU expansion of codec block motion
as the high-resolution bottleneck. The replacement uploads compact codec
vectors, stamps deterministic per-pixel ownership with GPU atomics, and resolves
the RG16F motion texture in compute. Color conversion and motion expansion share
one submission. Atomic maximum preserves the prior path's ordered "last vector
wins" behavior for overlapping blocks.

Controlled same-build measurements after twelve warm-up samples:

| Input and preset | CPU-motion pipeline | GPU-motion pipeline | GPU-motion p95 | Speedup |
|---|---:|---:|---:|---:|
| 426x240 Ultra Performance to 1080p | 2.098 ms | **1.239 ms** | 1.580 ms | 1.69x |
| 1280x720 Quality to 1080p | 4.842 ms | **1.538 ms** | 1.873 ms | 3.15x |
| 2560x1440 Quality to 4K | 22.137 ms | **5.547 ms** | 5.719 ms | 3.99x |

At 1440p, CPU motion preparation fell from `14.836 ms` to `0.035 ms`; the
batched upload submission costs `0.585 ms`. The resulting `5.719 ms` pipeline
p95 is inside both 60 fps and 120 fps frame budgets. The complete timing table
is `results/performance_gpu_motion.csv`.

The GPU-motion and retained CPU-reference paths produce an identical warmed 4K
frame: SSIM is `1.0` and PSNR is infinite. Against the native 4K reference, the
FSR frame measures `0.907792` SSIM versus `0.907475` for Lanczos; edge SSIM is
`0.858179` versus `0.860743`. This confirms the speedup without hiding the
small `-0.002564` edge-metric tradeoff already present in the model output.

The complete 240p matrix is unchanged within metric noise: mean SSIM delta
moves from `-0.016472` to `-0.016471`, mean edge delta moves from `+0.004652`
to `+0.004632`, and all 12 edge samples remain positive. At 720p, mean SSIM
delta changes by `-0.000001`, mean edge delta by `-0.000097`, and the positive
edge count remains 7/12. The accelerated matrices are
`results/quality_240p_gpu_motion.csv` and
`results/quality_720p_quality_gpu_motion.csv`.

After dynamic range/matrix conversion, the current 240p matrix measures mean
SSIM delta `-0.016752` and mean edge-SSIM delta `+0.004562`. Relative to the
previous retained matrix, the changes are `-0.000280` and `-0.000070`, small
enough to preserve the same quality conclusion and all 12 positive edge
samples. The current matrix is `results/quality_240p_dynamicmatrix.csv`.
A same-clip timing check improved from `1.2536` to `1.2380 ms` pipeline mean
and from `1.0773` to `1.0619 ms` GPU mean; the result is in
`results/performance_240p_dynamicmatrix_smoke.csv`.

## 4K Performance tier

Exact 3840x2160 Performance input now uses the recovered 7680x4320 14-pass
INT8 graph. Its shader pack is built reproducibly with
`tools/build_native_int8_pack.sh`; the builder compiles each generated HLSL
entry point, repairs DXC's invalid storage-buffer pointer aliases, reassembles
the module, and requires Vulkan 1.2 validation before accepting it.

Controlled Tears of Steel lossless measurements after warm-up:

| Path | Working scratch/accum/features | GPU mean | GPU p95 | Pipeline mean | Pipeline p95 |
|---|---:|---:|---:|---:|---:|
| Generic 39-dispatch | 3.46 GiB | 643.821 ms | 645.492 ms | 647.393 ms | 648.801 ms |
| Native 14-pass baseline | **316.4 MiB** | 18.051 ms | 18.193 ms | 19.992 ms | 20.256 ms |
| Native, recurrent output gated | **316.4 MiB** | 17.713 ms | - | 19.713 ms | - |
| Native, packed images and history sampling optimized | **316.4 MiB** | 16.428 ms | 16.575 ms | 18.042 ms | 18.362 ms |
| Native, batched upload and exact video fast paths | **316.4 MiB** | **16.024 ms** | **16.161 ms** | **17.598 ms** | **17.832 ms** |

The optimized native path is a `40.2x` GPU speedup over the generic graph. GPU
mean and p95 now fit a 16.67 ms 60 fps budget. The software-decode headless
pipeline does not fit 60 fps; the hardware-decode path fits on mean but still
misses at p95, and both remain comfortably inside the tested source's 24 fps
budget. Video playback does not feed the model's recurrent channels back by
default, so the postpass skips their sigmoid conversion and target-sized image
write. Source model-color and display-history traffic is packed to RGB10_A2,
and the postpass caches each workgroup's exact source footprint in LDS. The
prepass display-color history remains opt-in; the strict resolution matrix
showed gains for severe upscales but spatial regressions at 1280x720 input.
Causal model history remains active and presented.

Upload/conversion and inference command buffers are now submitted together
under one fence instead of forcing a CPU round trip between them. Disabled
comparison mode no longer writes an unused 4K raw image, and the YUV conversion
pass caches its exact luma/chroma footprint in LDS. The default no-feedback
prepass skips motion/reprojection work that cannot contribute to its output,
while retaining the previous algebra exactly. Prepass and postpass use `32x8`
groups for contiguous NHWC/output writes on RDNA3.

VAAPI-supported streams now remain on the GPU through decode and preprocessing.
FFmpeg maps each VAAPI surface to DRM PRIME, Vulkan imports the DMA-BUF with its
explicit modifier, and separate luma/chroma plane views feed the same manual
range- and matrix-aware GPU conversion used by the software-decode path. Codec
profiles unsupported by VAAPI continue through FFmpeg's software fallback.

| Input | Output | Decode | Pipeline mean | Pipeline p95 | GPU mean | GPU p95 |
|---:|---:|---|---:|---:|---:|---:|
| 426x240 | 1920x1080 | VAAPI DRM PRIME | **1.013 ms** | 1.376 ms | 0.893 ms | 0.944 ms |
| 426x240 | 1920x1080 | FFmpeg software | 1.021 ms | **1.207 ms** | 0.888 ms | **0.895 ms** |
| 3840x2160 | 7680x4320 | VAAPI DRM PRIME | **16.659 ms** | **17.042 ms** | 16.062 ms | 16.203 ms |
| 3840x2160 | 7680x4320 | FFmpeg software | 17.457 ms | 17.746 ms | **15.961 ms** | **16.116 ms** |

The 4K hardware path removes `0.799 ms` from pipeline mean and `0.704 ms` from
pipeline p95. Its mean fits a 16.67 ms 60 fps budget; p95 remains `0.375 ms`
over budget. At 240p decode/upload is already negligible, so hardware decode
changes mean by only `0.008 ms`. Full measurements are in
`results/decode_path_comparison_final.csv`.

The complete twelve-case 240p quality matrix is unchanged by hardware decode
within numerical noise: mean absolute SSIM difference is below `0.000001`, and
the prior positive edge delta remains positive in all 12 cases. Results and
matching frames are in `results/quality_240p_hwdecode_planes_final.csv`.

Final stage profiling still identifies distributed required work rather than
one accidental stall: prepass averages `1.250 ms`, postpass `2.322 ms`, and the
largest native passes are pass 13 (`1.727 ms`), pass 0 (`1.671 ms`), and pass 9
(`1.272 ms`). A four-channel
pass-13 specialization was rejected: it increased pass 13 to `3.42 ms` and total
GPU time to about `19.4 ms`. The original validated eight-channel shader was
restored. Full data is in
`results/performance_4k_performance.csv` and
`results/performance_rgb10_final.csv`, with retained runtime and stage logs
under `results/logs`.

The presented 7680x4320 frame is finite, correctly colored, and free of the
prior green/checkerboard failures. No native 8K ground-truth corpus is
available, so quality is reported conservatively as 8K-to-4K cycle
consistency. On CRF 23 input, FSR measures `0.927422` SSIM and `0.882326` edge
SSIM versus Lanczos at `0.929846` and `0.883854`. The current model is close but
does not beat Lanczos on this identity-oriented metric. Learned-strength sweeps
at 25%, 55%, and 80% did not improve both overall and edge similarity, so the
55% default remains unchanged. Results are in
`results/quality_4k_performance_cycle.csv`.

## Validation and limits

### Packet-safe resolution floor (2026-07-18)

The maintained real-video matrix now has a hard 240p floor. Demux backpressure
is stream-specific and never discards a packet when a decode queue is full.
EOF sends one decoder-drain marker instead of repeatedly flushing delayed
frames. A controlled Tears of Steel daylight CRF 23 run processed every source
frame at each native INT8 tier:

| Input | Native output | Processed | Pipeline mean | Pipeline p95 | GPU mean | GPU p95 |
|---:|---:|---:|---:|---:|---:|---:|
| 426x240 | 1920x1080 | 240/240 | **1.006 ms** | 1.081 ms | **0.882 ms** | 0.891 ms |
| 640x360 | 1920x1080 | 240/240 | **1.008 ms** | 1.118 ms | **0.881 ms** | 0.892 ms |
| 854x480 | 3840x2160 | 240/240 | **4.057 ms** | 4.377 ms | **3.867 ms** | 3.916 ms |
| 1280x720 | 3840x2160 | 240/240 | **4.073 ms** | 4.429 ms | **3.878 ms** | 3.919 ms |

The source is 24 fps, so every tier is comfortably inside its 41.67 ms frame
budget. The CSV `frames` column contains 228 timed samples because the first 12
frames are excluded as warmup; direct log and `ffprobe` counts establish the
240/240 coverage. Full data is in
`results/performance_240_to_720_native_int8_packet_safe.csv`.

The separate 3840x2160 60 fps stress clip now processes 180/180 frames instead
of 165/180. Wave-sized 64x1 workgroups for Performance-4320 passes 9 and 13
reduced the three-run post-warmup pipeline mean from 16.746 ms to 16.693 ms and
GPU mean from 16.093 ms to 16.060 ms. The same frame from both variants was
byte-identical at 7680x4320. Packet loss is fixed, but the pipeline remains
0.026 ms over the 16.667 ms 60 fps mean budget. Full A/B data is in
`results/performance_4k60_workgroup_ab.csv`; the current resolution comparison
is charted in `results/benchmark_frame_budget_240p_to_4k.svg`.

- All 90 generated clips and four lossless references pass checksum validation.
- All eight enabled CTest tests pass.
- All 98 generated native-tier SPIR-V modules, including the 4320p Performance
  and generated 2160p NativeAA packs,
  pass `spirv-val` for Vulkan 1.2.
- The codec-motion compute shader passes `spirv-val`, and a validation-layer
  720p playback run reports no Vulkan validation errors. Validation-layer
  4K-to-8K Performance playback also reports no errors or warnings.
- The metadata-aware YUV conversion shader passes `spirv-val` for Vulkan 1.2,
  and validation-layer 240p playback reports no Vulkan validation errors.
- The packed RGB10 source/history shaders and exact-tile postpass pass
  `spirv-val` for Vulkan 1.2. Validation-layer playback at 240p and 4K-to-8K
  reports no warnings, VUIDs, or errors.
- The DRM plane-view conversion shader passes `spirv-val` for Vulkan 1.2.
  Validation-layer VAAPI playback at 240p and 4K-to-8K reports no warnings,
  VUIDs, or errors.
- 240p runtime validation produced finite, non-green 1920x1080 output and the
  native graph loaded successfully on the RX 7900 GRE.
- The manual GPU harness passes in `4.873 ms`, with finite non-zero output and
  no NaN/Inf samples.
- Quality results currently sample one temporally warmed frame per clip. A
  multi-frame temporal metric is still needed for flicker, ghosting, and motion
  disocclusion.
- The native policy covers near-16:9 Ultra Performance inputs through 1080p and
  exact Quality mappings from 720p to 1080p and 1440p to 4K, plus exact 4K
  Performance to 8K. Balanced, NativeAA, other Quality/Performance dimensions,
  other aspect ratios, and unmatched larger inputs still use the generic graph.
- Decode, presentation, and sustained high-frame-rate pacing still need broader
  coverage at 4K and above; the former CPU codec-motion bottleneck is removed.
## Synthetic edge and dark-scene controls

The deterministic synthetic families are now materialized in `manifest.csv`
and exercised by the quality runner rather than being silently skipped. At
Balanced, 640x360 input, frame 60, and a 1280x720 output:

| Scene | FSR SSIM | FSR edge SSIM | Lanczos SSIM | Lanczos edge SSIM | SSIM delta | Edge delta |
|---|---:|---:|---:|---:|---:|---:|
| synthetic_edges_text | 0.776000 | 0.917689 | 0.864528 | 0.975517 | -0.088528 | -0.057828 |
| synthetic_dark | 0.512300 | 0.992029 | 0.768910 | 0.998358 | -0.256610 | -0.006329 |

The native 4:3 case also completed without padding: `synthetic_4_3` at
640x480 produced a fitted 960x720 output with FSR SSIM `0.817773` and edge
SSIM `0.909072`, versus Lanczos SSIM `0.922124` and edge SSIM `0.955032`.

These are diagnostic controls, not claims of superiority: they expose text,
one-pixel edges, and dark-tone reconstruction losses that are diluted in the
natural-footage aggregate. The generated files and per-frame difference images
remain available under `results/quality_frames`.

The five-preset sweep on the same 640x360 inputs showed that the source-size
clamp correctly collapses NativeAA, Quality, Balanced, and Performance to the
decoded dimensions; only Ultra Performance actually lowers the model input:

| Preset | Edge/text SSIM | Edge/text edge SSIM | Dark SSIM | Dark edge SSIM |
|---|---:|---:|---:|---:|
| NativeAA | 0.776004 | 0.917713 | 0.512300 | 0.992029 |
| Quality | 0.776001 | 0.917841 | 0.512300 | 0.992029 |
| Balanced | 0.776005 | 0.917720 | 0.512300 | 0.992029 |
| Performance | 0.776002 | 0.917653 | 0.512300 | 0.992029 |
| Ultra Performance | 0.771596 | 0.890851 | 0.512589 | 0.996054 |

## Generic postpass tail mapping

The generic reconstructed graph now uses the recovered swapped tail mapping by
default. Native INT8 graphs retain their established mapping. In the strict
current-build comparison at 426x240 input, the generic mapping improved SSIM
on all four real scenes and improved temporal error on three; the Sintel cave
temporal change was a small `+0.006425`. A matched native smoke was unchanged.
The explicit `TFORGE_FSR4_POSTPASS_TAIL_MAPPING=swap` override remains available
for A/B captures.

The follow-up 1280x720-input check used the current generic reconstructed path
with native INT8 disabled, history disabled, and learned strength 0.55. The
short eight-frame run completed for all four real scenes and produced valid
3840x2160 outputs under `.quality-tmp/generic-tail-1280-short`. The 24-frame
daylight and debris control/candidate pairs also matched exactly, confirming
that the source default and explicit `swap` override select the same mapping.
This scale check is coverage evidence only: at 1280x720 the generic diagnostic
path is not a universal spatial win over Lanczos (daylight and debris remain
slightly behind it), so it does not justify another quality promotion. The
generic 1280x720-to-3840x2160 diagnostic costs roughly 150-165 ms per frame on
this GPU when native INT8 is deliberately disabled; that timing must not be
mistaken for the normal native INT8 runtime.

## Native INT8 tail-mapping A/B

The recovered swapped postpass tail mapping was also tested explicitly on the
native INT8 graph. Four real 426x240 scenes were captured with 36 warmup and 24
scored frames, with color history disabled in both arms. The default mapping
and `TFORGE_FSR4_POSTPASS_TAIL_MAPPING=swap` produced the same metrics to the
reported precision on all four scenes; the largest observed difference was
about 0.000001. This is therefore not the native INT8 quality regression and
was not promoted. The paired captures are retained under
`.quality-tmp/native-tail-swap/` for audit purposes.

## 2026-08-25 scale-aware composition audit

The checked-in `config/quality_lab.json` was previously disabled, so normal
experimental playback used the legacy current composition instead of the
documented quality finalist. A matched live capture at 426x240 input to the
native 1920x1080 graph showed the difference on the same Tears of Steel
daylight frame: the legacy path measured SSIM `0.765867` and edge SSIM
`0.610430`, while the tested base-only/bilinear candidate measured `0.770776`
and `0.615344`.

The candidate was then verified across the four real-world high-quality
426x240 scenes. Its FSR-minus-Lanczos SSIM deltas were `+0.012647` daylight,
`+0.008856` debris, `+0.017062` rooftop, and `+0.012462` cave. Edge-SSIM
deltas were positive in all four scenes: `+0.006101`, `+0.006117`,
`+0.004974`, and `+0.002855`, respectively.

The base-only result was not promoted globally. A follow-up matched
1280x720-to-1920x1080 capture showed the legacy composition ahead on Tears of
Steel daylight (`+0.000127` SSIM and `+0.008850` edge SSIM versus Lanczos),
while base-only measured `-0.003031` and `-0.006141`. The runtime now applies
the checked-in base-only/bilinear configuration only when the actual pass
geometry is at least 3x; below that it preserves the legacy composition.
Explicit `TFORGE_QUALITY_LAB_CONFIG` experiments bypass this policy.

Post-change live verification selected the intended branches: 426x240 to
1920x1080 used base-only and measured SSIM `0.770991` with edge delta
`+0.006434`; 1280x720 to 1920x1080 used legacy composition and measured SSIM
`0.963612` with edge delta `+0.008764`.

## 2026-08-25 native 1:1 passthrough

Exact native-size selections now bypass neural reconstruction and publish the
uploaded native presentation image. This prevents a same-size comparison from
being degraded by an unnecessary FSR pass. A live AMD/Vulkan capture of the
1280x720 source at a 1280x720 target measured SSIM `0.976282` and edge SSIM
`0.858234`, exactly matching the Lanczos and bicubic identity controls for that
frame. The requested-frame gate was also verified so diagnostics do not capture
the decoder's initial startup frame.

The native-only path was then tightened so it does not generate or upload codec
motion and neutral side buffers that the passthrough never consumes. A follow-up
live 1280x720 identity capture remained bit-for-bit equivalent at the reported
metrics (`SSIM 0.976282`, edge SSIM `0.858234`). A separate 426x240-to-1920x1080
capture remained on the intended base-only branch (`SSIM 0.771043`, edge delta
`+0.006450`), so the optimization did not change upscale reconstruction.

The current performance trace separates the bottlenecks: a normal single
1280x720-to-3840x2160 stream spends about `0.119 ms` in the measured CPU
pipeline and `4.996 ms` on the GPU. The earlier 8-way campaign saturation is
therefore process-level CPU contention from parallel capture/measurement, not
the steady-state FSR frame path.

## 2026-08-25 hardware/software motion isolation

A full four-scene temporal A/B was rerun with 36 warm-up frames and 24 scored
frames per scene. VAAPI/DRM hardware decode and explicit software decode
produced identical temporal CSV rows on Tears of Steel daylight, Tears of
Steel debris, Sintel rooftop, and Sintel cave. This was not enough to conclude
that software motion was absent, so direct per-frame sidecars were inspected.

The sidecars show the decoder distinction clearly: the VAAPI path exported zero
codec vectors, while software decode exported 340-809 filtered vectors per
frame on the daylight clip. Nevertheless, the captured FSR PPMs were
bit-for-bit identical across hardware and software decode. Inverting the
motion sign and disabling the motion-validity gate also produced the same
frame hashes.

The reason is the currently promoted scale-aware Quality Lab branch. At severe
upscale it selects `base_only`; that postpass composition intentionally resolves
from the spatial base and does not consume temporal history. Motion changes are
therefore invisible on that branch even when software decoding supplies valid
vectors. This is a composition applicability finding, not evidence that the
decoder vectors are equivalent or that the CPU path is harmless.

The matched spatial control confirmed why the branch was promoted: fresh
Tears-of-Steel daylight at 426x240 to 1920x1080 measured SSIM `0.771043` and
edge SSIM `0.615693` on the current base-only branch, versus SSIM `0.765872`
and edge SSIM `0.610523` with the disabled Quality Lab control. No temporal
candidate has been promoted from this A/B; the temporal composition remains a
separate investigation.

The full legacy-temporal rerun answers that comparison directly. With color
history enabled, the current base-only branch beat the legacy temporal branch
on both mean SSIM and temporal-delta error for all four scenes:

| Scene | Base-only SSIM / temporal error | Legacy temporal SSIM / temporal error |
|---|---:|---:|
| Tears daylight | `0.857966 / 1.942783` | `0.854232 / 2.176725` |
| Tears debris | `0.933895 / 6.331108` | `0.930578 / 6.680608` |
| Sintel rooftop | `0.821120 / 5.009525` | `0.814190 / 5.339098` |
| Sintel cave | `0.905665 / 1.578744` | `0.899575 / 1.816403` |

This does not prove the temporal implementation is correct; it proves that
re-enabling the currently implemented temporal composition is a regression on
this corpus. Keep the promoted spatial branch while temporal reconstruction is
reworked independently. Software decode in the legacy-temporal A/B changed
mean SSIM by only about `+0.00005` to `+0.00014` per scene, with mixed temporal
error changes, so its extra CPU cost is not justified as a default quality
mode yet.

## 2026-08-25 native learned-strength applicability check

The numerically promising generic candidate (`color history on` plus learned
strength `0.55`) was paired again on fresh 426x240 daylight and Sintel cave
sequences. The baseline and candidate produced identical temporal CSV metrics
and identical SHA-256 hashes for every captured FSR frame. This is expected:
the native INT8 graph does not consume the generic learned-strength postpass
control. The candidate remains a generic-path experiment only and was not
promoted or used to change native defaults.

## 2026-08-25 720p direct-blend smoke

A small `direct_blend` Quality Lab candidate was tested against a fresh
1280x720-to-1920x1080 Tears of Steel daylight baseline. The baseline measured
SSIM `0.963436` and edge SSIM `0.867809`; direct learned blend `0.05` measured
SSIM `0.943508` and edge SSIM `0.774280`. It is a clear spatial and edge
regression, so the candidate was rejected without broader capture or any
default change. The initial 1280x720 runner invocation was also corrected to
use an explicit 1920x1080 viewport so the native 1:1 bypass could not contaminate
the upscale smoke.

## 2026-08-25 reactive residual-gate audit

The opt-in motion-aware residual gate was instrumented with its actual
`reactiveAverage` input before threshold tuning. On the hardware-decoded
Tears of Steel daylight capture, the recorded histogram and luma deltas were
zero and `reactiveAverage` was exactly `0.0` for every frame. The gate therefore
cannot react to hardware-decoded frames unless the motion-confidence reactive
arm is also enabled.

The same eight-frame software-decoded capture produced reactive values from
`0.0142` through `0.0198` after the initial reset frame. The original gate
range (`smoothstep(0.02, 0.18, reactive)`) was consequently almost always
inactive. The opt-in range is now `smoothstep(0.005, 0.025, reactive)`, based
on that measured signal rather than an assumed scale.

Matched 24-frame software-decoded daylight captures with residual strength
`0.05` measured:

| Candidate | SSIM | Temporal error |
|---|---:|---:|
| Residual 0.05, gate off | 0.859020 | 0.707684 |
| Residual 0.05, tightened reactive gate | 0.859098 | 0.701584 |
| Residual 0.05, gate plus motion-confidence reactive | 0.859116 | 0.699681 |

The gate is now proven wired and measurable, but the improvement is too small
to offset the residual candidate's larger temporal instability versus the
promoted base-only path. It remains opt-in and is not promoted.

## 2026-08-25 capture scheduling note

The normal single-stream 1280x720-to-3840x2160 path remains GPU-bound in the
measured run (`0.1190 ms` pipeline CPU versus `4.9959 ms` GPU). The quality
campaign itself is CPU/coordination-bound when oversubscribed: the eight-way
probe reached roughly `96–97%` CPU and took longer than the lower-concurrency
runs. The exhaustive candidate runner now defaults to two workers; an explicit
worker count remains available for controlled experiments.

The temporal matrix runner now defaults each FFmpeg metric/encode child to one
thread when it fans out captures. On the same real Tears of Steel 426x240
eight-frame capture, this reduced wall time from `3.64 s` to `3.38 s`, and the
reported CSV was byte-identical. Direct `run_temporal_quality.sh` invocations
keep the historical FFmpeg threading unless
`TFORGE_BENCHMARK_FFMPEG_THREADS` is set explicitly.

## 2026-08-25 learned-strength applicability recheck

The older `agent_next_learned_strength` package reported `learned_055` as a
review candidate, but that result was measured against the preserved current
composition. The active severe-upscale path now selects Quality Lab
`base_only`, which consumes the experimental `slot5.x` field and does not
consume the current-path `slot1.w` learned-strength override.

Fresh 24-scored-frame / 36-warmup captures at 426x240 -> 1920x1080 confirmed
that the override is currently a no-op on the active path:

| Scene | Control SSIM / temporal error | `learned_055` SSIM / temporal error |
| --- | ---: | ---: |
| Tears of Steel daylight | `0.857966 / 1.942783` | `0.857966 / 1.942783` |
| Sintel cave | `0.905665 / 1.578744` | `0.905665 / 1.578744` |

This candidate is not promoted. The result is an applicability finding, not
evidence that learned strength itself is ineffective on a path that consumes
it.

## 2026-08-25 history motion-sign recheck

The corrected A/B was run with color history explicitly enabled. Inverting the
experimental motion sign changed the current legacy temporal path only from
SSIM `0.854309` to `0.854313` and from temporal error `2.188555` to `2.187646`
on the 24-frame Tears of Steel daylight capture. That difference is negligible
and does not justify promotion. It also confirms that the earlier motion-sign
test was not hiding a large quality win once history was actually active.

## 2026-08-25 postpass tail-mapping applicability recheck

The earlier generic-path tail-mapping probe was rerun on the current build with
12 warmup and 24 scored frames across all four real scenes at 426x240 ->
1920x1080. The control and `TFORGE_FSR4_POSTPASS_TAIL_MAPPING=swap` captures
were identical to measurement precision. For example, daylight was
`0.922841 / 1.176811` versus `0.922841 / 1.176805` SSIM / temporal error.

The source audit explains why: the current host wiring already sets the swap
bit for the generic reconstructed path by default. The recheck therefore
tested the active default against itself, not a new candidate. No promotion or
default change was made. The older probe remains historical evidence for why
that generic default mapping was selected, not a current untested candidate.

## 2026-08-25 history-confidence threshold recheck

The history-confidence cutoff was retested with software decode and color
history explicitly enabled. On the matched 24-frame / 36-warmup Tears of Steel
daylight capture, the legacy temporal control measured SSIM `0.854309` and
temporal error `2.188573`. The opt-in `0.9` confidence threshold measured
SSIM `0.854321` and temporal error `2.190176`.

The tiny SSIM increase came with worse temporal error, so this threshold is
rejected and remains opt-in. The promoted base-only path is unchanged.

The adjacent postpass-tail-presence A/B was also checked on the generic
daylight path. Keeping the recovered group produced SSIM `0.854998` and
temporal error `2.789733`; disabling it produced SSIM `0.855002` and temporal
error `2.789774`. The output movement is negligible and the temporal direction
is slightly worse, so tail presence is not a promotable quality lever either.

## 2026-08-25 display-space base probe correction

An opt-in `TFORGE_FSR4_QUALITY_LAB_DISPLAY_BASE` probe was added to isolate
model-space filtering from display-space RGB filtering. An earlier retained
artifact set appeared to show a large gain, but the result could not be
reproduced after rebuilding the current source: matched model-space and
display-space captures produced identical metrics, even though the host flag
was confirmed set and the runtime reported `colorSpace=display`.

Because that discrepancy makes the earlier positive capture set
non-authoritative, display-space base is not promoted. The checked-in
`config/quality_lab.json` remains `colorSpace: model`; the probe and its
configuration parsing remain available for a future isolated investigation.

## 2026-08-25 native/generic applicability and tiny learned-strength probe

The current severe-upscale review configuration is explicitly `base_only`.
That composition resolves the final image from the spatial base and does not
consume the native or generic learned tensor. Matched native-INT8 and generic
captures therefore produced byte-identical FSR frames; this is expected for
that configuration and is not evidence that the two neural graphs are
equivalent. Preserved player logs confirm the native run selected
`quality_1080`, while the generic run allocated the generic tensor buffers and
did not activate the native graph.

To test whether the neural output could help the active severe-upscale path, a
fresh six-arm daylight capture was run at 426x240 -> 1920x1080 with 12 warmup
and 24 scored frames. Each arm used the bilinear base and varied only direct
learned strength:

| Learned strength | SSIM | temporal error |
| ---: | ---: | ---: |
| base-only control | `0.860772` | `1.557817` |
| `0.0001` | `0.857392` | `3.826512` |
| `0.0005` | `0.857390` | `3.826621` |
| `0.001` | `0.859119` | `3.826765` |
| `0.002` | `0.857385` | `3.827046` |
| `0.005` | `0.857372` | `3.827840` |

Even the smallest learned contribution regressed both spatial and temporal
metrics on this real scene. None is promoted. A separate single-history-blend
probe changed SSIM by less than `0.000001` and temporal error by `0.000167`,
so that hypothesis is also not a useful lever at this point.

## 2026-08-25 native versus generic learned-path isolation

The first native/generic comparison accidentally selected a 1280x720 source,
so it was discarded as a no-upscale check. The corrected capture used the real
426x240 high-quality clip, forced to 1920x1080, with the same `learned_only`
configuration and frame 48 on both runs.

The native INT8 graph and the generic graph are not equivalent on this path:

| Path | SSIM | edge-SSIM | PSNR | output RGB hash |
| --- | ---: | ---: | ---: | --- |
| Native `quality_1080` | `0.761695` | `0.604463` | `25.700111` | `19a64a350f4661aa...` |
| Generic reconstructed path | `0.512189` | `0.490756` | `22.547319` | `a66311821c4827c4...` |

The matched GPU source hash was identical, so this is an output-path
difference, not a different decoded frame. The native run also reported
`pipelineCPU=1.987ms` versus `GPU=1.776ms` for the measured frame: this
confirms that this one-frame capture is CPU/dispatch-bound, although the
earlier sustained 1080p->4K profile remains GPU-bound at about 5.0ms GPU time.

This is a diagnostic isolation result, not a promotion. The active default
remains `base_only`; no reconstruction shader or model behavior was changed.
The next quality investigation must explain the native/generic learned tensor
and postpass contract before attempting to blend learned output into the
default path.

## 2026-08-25 generic decoder-zero isolation

An opt-in decoder readback showed that the generic learned path's final
8-channel tensor is zero for every sampled channel on the real severe-upscale
capture. The native path has populated, finite channel ranges on the same
input. This explains the generic `learned_only` SSIM collapse to `0.512189`.

The final-stage trace also showed the hardware fact that matters here:
`hasFp16Fallback=false` on this RADV device. The existing scalar final shader
was therefore never selected. It was made available behind the existing
`TFORGE_FSR4_FP16_FINAL_SCALAR=1` opt-in switch because that shader does not use
cooperative matrices. The rebuilt diagnostic confirmed that the scalar shader
was selected, but the final tensor remained all zero and the quality result was
unchanged (`SSIM 0.512189`). This rules out the scalar-vs-cooperative final
selector as the sole cause.

The opt-in final-stage trace is retained while the generic graph is being
isolated; it has no effect unless explicitly enabled. No default path,
reconstruction algorithm, or native graph was changed by this probe.

Disabling all cooperative-matrix dispatches also left the generic final tensor
zero while keeping the severe-upscale frame around `36ms` GPU/CPU. Therefore
the failure is not isolated to the cooperative-matrix implementation or the
optional FP16 final specialization. Generic learned-path work is paused until
the earlier generic tensor/resource contract is audited; it is not a viable
quality candidate or campaign baseline.

## 2026-08-25 generic tensor readback refresh

The earlier `generic decoder-zero isolation` conclusion was based on a stale
diagnostic state and is superseded. A fresh first-frame readback with the
current build, postpass enabled, and the same real 426x240 Tears of Steel clip
showed finite, nonzero values in all eight decoder channels. Disabling the
postpass also produced nonzero decoder values. The generic tensor is therefore
alive; the remaining failure is its reconstruction/composition quality, not a
confirmed all-zero final tensor.

The refreshed generic `learned_only` frame at 426x240 -> 1278x720 measured
PSNR `22.974196`, SSIM `0.691252`, and edge-SSIM `0.517140`, below the
Lanczos control's SSIM `0.775127`. A direct-blend sweep improved as learned
strength decreased, but the best tested arm (`0.0`) still remained below the
spatial controls. No candidate was promoted.

The quality harness also had a real control-routing defect: an explicit
`TFORGE_FSR4_LEARNED_STRENGTH` override was forwarded but then overwritten by
the experimental JSON value. The host now preserves the JSON default while
allowing an explicit override to win, so future strength captures are
semantically valid. This changes only quality-lab control routing; the default
composition remains unchanged.

The supported-size follow-up narrowed this further. At 426x240 -> 1920x1080,
the native fixed-shape graph produced populated decoder channels and measured
about `1.2ms` GPU, while the generic graph produced eight all-zero decoder
channels and measured about `36.2ms` GPU. Disabling fused residual operators
and selecting the scalar final fallback did not change the generic zero.
At 1278x720, which has no native fixed-shape graph, the generic tensor was
nonzero. This makes the failure resolution-dependent inside the generic graph,
not a postpass-only issue. The standalone `TFORGE_FSR4_DIAGNOSE` probe was
disabled after it continued to segfault on the RADV device; it is not used as
quality evidence.

## 2026-08-25 CPU-vs-GPU timing and stage probe

The performance runner now uses `build-fast` by default. Its summary function
also keeps short runs instead of discarding the first twelve already-filtered
timing samples.

A fresh single-stream run on the real 426x240 Tears of Steel clips measured
roughly `0.08ms` CPU pipeline time versus `1.17ms` GPU time. That path is
GPU-bound. The observed CPU saturation belongs to the eight-way campaign:
multiple player processes, decode, file I/O, and coordination compete for CPU
even though one render does not.

An opt-in real-graph stage limiter was added for diagnosis only. At 1920x1080,
the partial graph is finite through the early stages; later stages require
channel-aware inspection because the intermediate tensor widens. No stage
correction has been promoted and no default reconstruction behavior changed.

The stage configuration trace identifies the boundary as the `/4` encoder
expansion: step 11 is a 32→32 partial spatial mix, followed by step 12 as a
32→64 pointwise expansion, then step 13 contracts 64→32. The ping-pong slots
and dispatch grids are consistent at this boundary. Disabling cooperative
pointwise dispatch, residual adds, or ReLU independently did not restore the
full 1080p generic output, so none of those is sufficient as the correction.

## 2026-08-25 FP8 boundary correction

The synthetic generic proof isolated the zero tensor to the 32→64 pointwise
expansion. With the old FP8-boundary bypass, the full scalar generic chain
failed Stage 2 with an all-zero tensor. Enabling the existing nearest E4M3
quantizer produced a finite, non-zero full-chain tensor and passed structural
validation (`1005382/1005382` sampled values finite and non-zero).

The generic path now uses nearest FP8 boundary quantization by default. The
old bypass remains available through the explicit
`TFORGE_FSR4_FP8_ROUNDING=off` or `bypass` diagnostic setting. Native INT8
does not use this path.

A matched Tears of Steel 426x240 high-quality frame remained unchanged in the
real review capture (`PSNR 25.488720`, `SSIM 0.771043`, `Edge-SSIM 0.615693`),
so this correction is currently justified as removal of a generic-path
structural failure, not yet as a measured real-image quality gain.

The second spot check, Tears of Steel debris at 426x240 low quality, measured
`PSNR 24.928802`, `SSIM 0.831877`, and `Edge-SSIM 0.817629`; the Lanczos
control measured `24.829159`, `0.826079`, and `0.811367`, respectively. This
confirms the current generic review path remains above the spatial control on
that scene, but it is still not an A/B gain attributable specifically to the
new FP8 default because the optimized real path does not expose the scalar
boundary in the same way as the standalone proof.

The four-scene 426x240 high-quality spot set also remains above the Lanczos
control on the current review path. SSIM / Edge-SSIM were `0.771043 / 0.615693`
versus `0.758129 / 0.609243` for Tears daylight, `0.877368 / 0.823562` versus
`0.867906 / 0.817355` for Tears debris, `0.690079 / 0.617894` versus
`0.672766 / 0.613158` for Sintel rooftop, and `0.565798 / 0.826627` versus
`0.552203 / 0.823724` for Sintel cave. These are validation spot checks of
the current path, not proof that the FP8 boundary change alone caused those
gains.

The same real Tears daylight clip measured `0.0855ms` normal pipeline CPU time
against `1.1973ms` GPU time. Forced synchronous capture measured `1.3394ms`
pipeline CPU time, with `1.2543ms` spent waiting for the GPU. This confirms the
single stream is GPU-bound; the CPU saturation seen in the eight-worker
campaign is process-level capture contention, not a CPU reconstruction stage
inside one steady-state frame.

## 2026-08-25 generic tail-mapping applicability recheck

The earlier generic postpass tail-mapping candidate was rerun on fresh current
build captures for Tears of Steel daylight and Sintel cave at 426x240 ->
1920x1080, with learned strength `0.55`, color history, software decode,
12 warm-up frames, and 24 scored frames. The control and explicit
`TFORGE_FSR4_POSTPASS_TAIL_MAPPING=swap` runs produced identical CSV output for
both scenes: daylight SSIM `0.861132` and temporal error `1.574690`, cave SSIM
`0.926001` and temporal error `0.957636`.

This is expected from the current source: the generic postpass already sets
the swapped mapping flag unconditionally, while the environment variable is
only an explicit A/B override for other graph types. The candidate is already
active on the generic path; applying it again is a no-op and must not be
counted as a new quality gain.

## 2026-08-25 generic reverse-tail-channel smoke

The separate `TFORGE_FSR4_POSTPASS_REVERSE_TAIL_CHANNELS=1` hypothesis was
tested with an explicit `current` composition configuration so that it reached
the learned postpass instead of the checked-in severe-upscale `base_only`
branch. On the completed 426x240 tier (four real scenes, 24 warm-up and 24
scored frames), SSIM changes were mixed and effectively zero: `-0.000003`
daylight, `-0.000006` debris, `0.000000` rooftop, and `+0.000002` cave.
Temporal absolute-error changes were also tiny (`-0.000098`, `-0.000029`,
`-0.000086`, and `+0.000038`). This is not a meaningful quality gain, so the
candidate is rejected for promotion. The 1280x720 tier did not complete and is
explicitly not counted as evidence.

## 2026-08-25 generic recurrent-bias applicability smoke

The opt-in `TFORGE_FSR4_EXPERIMENTAL_LEGACY_RECURRENT_BIAS=1` path was tested
with the same explicit current-composition generic setup on fresh daylight and
cave captures. It produced byte-identical CSV results to the control in both
scenes (`0.857542` SSIM / `1.493213` temporal error for daylight and `0.932946`
SSIM / `0.634208` temporal error for cave). The recurrent-bias arm is therefore
not an observable quality lever in this path and was not promoted.

The same A/B was repeated with `TFORGE_FSR4_ENABLE_RECURRENT=1` explicitly set
for both control and candidate. The candidate remained effectively neutral:
daylight SSIM `0.857475` and temporal error `1.494160` versus control
`0.857475` / `1.494104`; cave SSIM `0.932952` and temporal error `0.634306`
versus `0.932950` / `0.634279`. This confirms the recovered bias is not a
hidden gain that appears only when recurrent writes are enabled.

## 2026-08-25 generic FP8 exponent-bias A/B

The generic scalar graph was tested with an opt-in `TFORGE_FSR4_FP8_DECODE_BIAS=7`
decoder against the existing bias-8 control. The switch changes both host-side
packed-weight derivation and scalar shader FP8 boundary decode/quantization; the
default remains unchanged. Captures used the explicit current-composition
configuration, color history, software decode, 12 warm-up frames, and 12 scored
frames at 426x240 -> 1920x1080.

Bias 7 improved spatial SSIM slightly on both completed real scenes: daylight
`0.857542 -> 0.857938` (+`0.000396`) and cave `0.932946 -> 0.932983`
(+`0.000037`). Temporal absolute error regressed on both: daylight
`1.493213 -> 1.493823` (+`0.000610`) and cave `0.634208 -> 0.634226`
(+`0.000018`). Because the spatial improvement came with a consistent temporal
regression, the candidate is rejected for promotion. The raw pair data is kept
in `benchmarks/quality_sweeps/swarm/agent_fp8_bias7/measured_results.json`.

The candidate was then rerun with the normal cooperative generic graph enabled.
It produced the same values as the scalar-only arm for both scenes (daylight
`0.857938` SSIM / `1.493823` temporal error; cave `0.932983` / `0.634226`).
Cooperative-kernel selection therefore does not rescue this hypothesis.

## 2026-08-25 video jitter applicability check

The existing jitter controls (`default`, `off`, `reduced`, and `controlled`
with strength `0.5`) were compared on matched 1280x720 -> 3840x2160 temporal
captures of Tears of Steel daylight and Sintel cave. Each arm used software
decode, color history, 12 warm-up frames, and 12 scored frames. All four modes
produced byte-identical reported metrics on both scenes: daylight SSIM
`0.933880` / temporal error `1.517905`, cave SSIM `0.958580` / temporal error
`0.290007`.

Jitter is therefore not an observable quality lever in this current capture
path. No default or reconstruction behavior was changed.

## 2026-08-25 generic current-composition learned-strength sweep

The explicit generic `current` composition was swept at learned strengths
`0.0`, `0.1`, `0.25`, `0.4`, `0.55`, and `0.7` on fresh 426x240 -> 1920x1080
captures with color history, software decode, 12 warm-up frames, and 12 scored
frames. This run also confirms that the host-side learned-strength override is
now consumed by the current composition rather than being silently ignored.

Tears of Steel daylight regressed spatially at every positive strength:
`0.857654` SSIM at `0.0`, then `0.855546`, `0.841234`, `0.820339`, `0.797356`,
and `0.774236`. Sintel cave improved spatially through `0.55` (`0.932465` at
`0.0` to `0.934903`), but that scene-specific gain does not generalize; the
daylight regression is large and consistent. Higher strengths also trade
spatial quality for lower temporal error, so this is not a universal quality
win. The candidate is rejected and remains opt-in. Raw measurements are in
`benchmarks/quality_sweeps/swarm/agent_current_strength_sweep/measured_results.json`.

## 2026-08-25 generic resolution-boundary readback refresh

The earlier resolution-dependent zero-tensor note is superseded by a fresh
short-clip stage readback. With native INT8 disabled, the generic graph was
truncated after steps 12 and 39 at both 1920x1080 and 1278x720. Step 12's
32->64 expansion was finite and populated at both sizes. The full 39-step
1920x1080 readback was also finite and populated in all sampled decoder
channels; channel 0 had mean `77.55417` and channel 7 had mean `51.37504`.

The current evidence does not support a resolution-dependent dead tensor as
the active quality failure. The remaining generic-path problem is the quality
of the learned composition, which is why the learned-strength sweep above was
run and rejected. The short probe logs remain in `/tmp` only; this conclusion
is recorded here so old zero-readback artifacts are not treated as current
evidence.

## 2026-08-25 generic postpass tail-mapping smoke check

The temporal runner now forwards `TFORGE_FSR4_RE_ROOT`, so temporary weight
blobs used by an A/B capture actually reach the player. A corrected probe also
confirmed that the generic path swaps the two four-float tail groups: indices
218-221 are the active output-bias group for generic dispatch, while 214-217
are the other group. Disabling the tail changed exact output-frame hashes, so
the postpass branch is live.

The opt-in `TFORGE_FSR4_POSTPASS_TAIL_MAPPING=normal` override was compared
with the existing generic `swap` mapping on all four real scenes at
426x240 -> 1920x1080. Swap was marginally higher SSIM on all four scenes and
had a mixed but tiny temporal difference (roughly 0.00002-0.00021). No default
or reconstruction-quality promotion was made; the full measurements are in
`benchmarks/quality_sweeps/swarm/agent_postpass_tail_mapping/measured_results.json`.

## 2026-08-25 generic postpass tail-channel reversal check

The active generic postpass tail group was captured with its four channels
reversed and compared with a matched default run: 426x240 high-CRF input,
1920x1080 output, software decode, two warmup frames, four scored frames, and
the current Quality Lab configuration. The candidate was effectively neutral:
SSIM was unchanged to six decimals on three scenes and lower by 0.000001 on
the other scene; temporal error moved only in the fifth or sixth decimal place.
The result is recorded in
`benchmarks/quality_sweeps/swarm/agent_postpass_tail_reverse/measured_results.json`.
It is diagnostic only and was not promoted.

## 2026-08-25 generic learned-strength recheck

The current generic postpass swap path was rechecked at learned contribution
strengths `0.0` and `0.15` against the current configuration on all four real
scenes at 426x240 -> 1920x1080. The conservative `0.15` value improved SSIM
slightly on debris, rooftop, and cave, but worsened temporal error on debris
and rooftop. Zero learned contribution reduced SSIM on every scene. Neither
candidate is safe as a global default. The paired measurements are recorded in
`benchmarks/quality_sweeps/swarm/agent_learned_strength_recheck/measured_results.json`.
## 2026-08-25 generic postpass source-anchor rounding check

The public FSR 4.0.2 `post_wmma.hlsl` uses `round()` for its odd-sized 3x3
source anchor. The current Temporal Forge generic postpass uses `floor()` by
default and exposes `TFORGE_FSR4_EXPERIMENTAL_LEGACY_ROUND=1` as an opt-in
comparison. A matched real-corpus smoke check (Tears of Steel daylight and
debris, Sintel rooftop and cave; software decode; 2 warmup plus 4 scored
frames; 426x240 to 1920x1080) lowered SSIM on all four scenes. Temporal error
was mixed and only changed slightly. The candidate was rejected and the
default remained unchanged. Detailed values are in
`benchmarks/quality_sweeps/swarm/agent_postpass_round_anchor/measured_results.json`.

## 2026-08-25 campaign CPU-versus-playback timing check

The capture runner now forwards the existing timing and stage-selection trace
controls, so campaign logs can distinguish CPU work from GPU synchronization.
On the current build, a normal single-stream Tears of Steel 426x240 ->
1920x1080 playback sample measured about 0.3-0.6 ms of CPU pipeline work,
0.008 ms decode, 0.018-0.039 ms command recording, and 1.15-1.24 ms of GPU
work per frame. CPU wait-for-GPU time was about 0.001-0.002 ms. The observed
CPU saturation belongs to the multi-worker capture path (software decode,
conversion, image dumping, and metrics), not to the normal FSR reconstruction
path. No reconstruction behavior was changed.

## 2026-08-25 native fixed current-history weight check

The public FSR prepass uses its input alpha directly in the `0.1` current-frame
history weight. Temporal Forge's video prepass has no meaningful input alpha,
so its normal path uses the Gaussian resolve's center-tap weight instead. An
opt-in `TFORGE_FSR4_EXPERIMENTAL_FIXED_HISTORY_WEIGHT=1` probe tested the
reference-style fixed `0.1` value with color history enabled.

The NativeAA 426x240 -> 1920x1080 A/B used the native INT8 graph (the generic
fallback was not disabled). The candidate improved SSIM on three of four
scenes and temporal error on all four in the longer run. At 640x360 and
1280x720 it still improved spatial scores broadly, but temporal error became
scene-dependent.
It is therefore retained as a diagnostic candidate and not promoted globally.
Measurements are summarized in
`benchmarks/quality_sweeps/swarm/agent_fixed_history_weight/measured_results.json`.

## 2026-08-25 bilinear plus fixed-history-weight check

The fixed `0.1` current-frame history interpretation was paired with the
bilinear base filter to test whether it could recover bilinear's temporal
penalty. Both arms used the native INT8 graph, `base_only` composition, color
history, software decode, and 12 warm-up plus 12 scored frames at 426x240 to
1920x1080.

Bilinear improved spatial SSIM on all four real scenes, but increased temporal
absolute error on all four: daylight `1.487515 -> 1.748300`, debris
`0.012909 -> 0.325521`, rooftop `0.544263 -> 1.395563`, and cave
`0.624852 -> 0.709602`. The combination is rejected and neither the base
filter nor the fixed-history interpretation is promoted globally. Full paired
CSVs and provenance are in
`benchmarks/quality_sweeps/swarm/agent_bilinear_fixed_history/`.

## 2026-08-25 native fixed-history clean-default recheck

The earlier fixed-history result was not sufficient to justify a playback
change because both arms had already enabled color history and exercised the
quality-lab composition path. A clean A/B was therefore run against the
current native INT8 default, with the candidate adding
`TFORGE_FSR4_ENABLE_COLOR_HISTORY=1` and
`TFORGE_FSR4_EXPERIMENTAL_FIXED_HISTORY_WEIGHT=1`.

Both arms used the native INT8 graph, software decode, 426x240 input,
1920x1080 output, 12 warm-up frames, and 12 scored frames across Tears of
Steel daylight, Tears of Steel debris, Sintel rooftop, and Sintel cave. The
CSV files were byte-identical for every pair:

| scene | default SSIM | candidate SSIM | default temporal error | candidate temporal error |
| --- | ---: | ---: | ---: | ---: |
| Tears daylight | 0.861529 | 0.861529 | 1.755780 | 1.755780 |
| Tears debris | 0.939253 | 0.939253 | 0.346234 | 0.346234 |
| Sintel rooftop | 0.780647 | 0.780647 | 1.456409 | 1.456409 |
| Sintel cave | 0.936432 | 0.936432 | 0.713605 | 0.713605 |

This candidate is rejected as a no-op on native playback. The generic Quality
Lab result is not promoted or treated as evidence of a native INT8 gain. Full
paired CSVs and the machine-readable decision are in
`benchmarks/quality_sweeps/swarm/agent_native_history_fixed_default/`.

## 2026-08-25 benchmark-config contamination fixed and clean native baseline

The temporal runner was previously allowing the checkout-level
`config/quality_lab.json` to control captures. That file is currently an
interactive `base_only` configuration with learned strength `0.0`, so several
earlier A/B runs were not exercising the Current composition at all. The
runner now supplies an isolated, nonexistent Quality Lab path when no explicit
experiment file is requested. This loads the typed Current defaults while
still honoring deliberate `TFORGE_QUALITY_LAB_CONFIG` experiments.

The corrected native INT8 default was then captured with software decode,
426x240 input, 1920x1080 output, and 8 scored frames:

| scene | FSR SSIM | Lanczos SSIM | FSR temporal delta mean | Lanczos temporal delta mean |
| --- | ---: | ---: | ---: | ---: |
| Tears daylight | 0.855350 | 0.853780 | 1.460254 | 1.475034 |
| Tears debris | 0.937575 | 0.936707 | 4.714316 | 4.713200 |
| Sintel rooftop | 0.800829 | 0.798489 | 17.626157 | 17.816086 |
| Sintel cave | 0.989665 | 0.989304 | 0.013208 | 0.001919 |

The clean Current path is now measurably above Lanczos spatially on all four
scenes, with mixed temporal behavior. Learned-strength `1.0` was rejected on
the daylight smoke scene (`0.853706 -> 0.732351` SSIM); no strength change was
promoted. The detailed clean captures are in
`benchmarks/quality_sweeps/swarm/agent_clean_native_default/`.

## 2026-08-25 clean learned-strength 0.10 candidate

After the runner isolation fix, `TFORGE_FSR4_LEARNED_STRENGTH=0.10` was
verified on the same four-scene native INT8 corpus. It improved spatial SSIM
over the clean 0.05 default on all four scenes, but temporal error moved in
both directions. It remains opt-in rather than becoming the default:

| scene | SSIM at 0.05 | SSIM at 0.10 | temporal error at 0.05 | temporal error at 0.10 |
| --- | ---: | ---: | ---: | ---: |
| Tears daylight | 0.855350 | 0.855564 | 1.445423 | 1.427661 |
| Tears debris | 0.937575 | 0.937731 | 0.017630 | 0.021745 |
| Sintel rooftop | 0.800829 | 0.801139 | 0.583000 | 0.604428 |
| Sintel cave | 0.989665 | 0.989681 | 0.124732 | 0.113794 |

Full CSVs and provenance are in
`benchmarks/quality_sweeps/swarm/agent_clean_learned_strength_010/`.

## 2026-08-25 settled learned-strength confirmation

The promising learned-strength screen was repeated with 36 warm-up frames and
24 scored frames, rather than relying only on the short eight-frame screen.
The native INT8 graph, software decode, 426x240 input, and 1920x1080 output
were held constant. The existing `0.05` value was compared with `0.10`, then
the midpoint `0.075` was checked separately.

`0.10` improved mean SSIM from `0.874470` to `0.874621` and reduced mean
temporal absolute error from `0.789408` to `0.783712`, but the debris pair
still exceeded the per-pair temporal safety gate. The `0.075` midpoint also
improved SSIM on all four scenes, but debris remained over the gate. Neither
value is promoted as a global default.

The focused reduced-input-sharpen runs were worse temporally on the moving
scenes, despite improving still SSIM. This rejects the idea that simply
removing the existing input sharpen is the missing quality fix.

## 2026-08-25 adaptive learned-strength and single-history probes

The opt-in motion-confidence adaptive gate was tested at learned strength
`0.075` with the same settled 36/24 protocol. It improved the debris temporal
error, but reduced spatial quality and worsened temporal error on daylight and
rooftop. It remains rejected.

The opt-in single-history-blend switch was also tested on the native INT8 path.
Its four scene pairs were effectively identical to the control at metric
precision, so it does not explain the native quality gap and was not promoted.

## 2026-08-25 generic postpass tail recheck

The earlier generic-path tail-swap lead was rerun with the corrected isolated
runner and settled 36/24 protocol. The current generic path already enables
the swapped mapping by default, so the explicit `swap` arm and the unset
control were byte-equivalent across the four real scenes. Both generic arms
were substantially below Lanczos spatially, confirming that this is not a
missing promotion opportunity. The native INT8 path remains the maintained
playback path; no default or shader behavior was changed.

## 2026-08-25 native linear-light learned blend candidate

The first clean candidate worth retaining for visual review is the combination
of `TFORGE_FSR4_LEARNED_STRENGTH=0.075` and
`TFORGE_FSR4_CURRENT_BLEND_LINEAR=1`. At 426x240 -> 1920x1080, it improved
temporal absolute error on all four real scenes versus the matched `.075`
sRGB arm, while its SSIM changes stayed within `0.0005` per pair. Relative to
the clean `.05` sRGB default, its mean SSIM was slightly higher and its mean
temporal error was lower.

The second tier was also captured at 1280x720 -> 3840x2160 with 36 warm-up
frames and 24 scored frames. Against the fresh `.05` sRGB control, the
candidate's mean SSIM changed by `+0.00002875` and mean temporal absolute
error by `-0.000035`. It improved the rooftop and cave temporal pairs but was
slightly worse on daylight and debris. This remains an opt-in candidate, not
a default promotion; human visual review of both tiers is still required.

## 2026-08-25 720p learned-strength sweep

The retained lead was tested at 1280x720 -> 3840x2160 with learned strengths
`.075`, `.0875`, and `.10`, holding linear blending, ungated confidence, RCAS
`.12`, CAS `.01`, software decode, and 36/24 temporal capture settings fixed.

Relative to `.075`, `.10` changed mean SSIM by `+0.0000285` and mean temporal
error by `-0.0016633`. It improved temporal error on daylight, debris, and
rooftop, but regressed cave by `+0.003502` temporal error.

The midpoint `.0875` changed mean SSIM by `+0.0000160` and mean temporal error
by `-0.0007540`. Its per-scene temporal changes were `-0.000665`, `-0.000222`,
`-0.003882`, and `+0.001754` for daylight, debris, rooftop, and cave.

Neither setting is promoted globally because cave regresses. `.0875` is
retained as a resolution-specific opt-in candidate for visual review. Raw
captures are under `.quality-tmp/learned075-1280-*-20260825/`,
`.quality-tmp/learned0875-1280-*-20260825/`, and
`.quality-tmp/learned100-1280-*-20260825/`.

The same sweep was run at 640x360 -> 1920x1080. Relative to `.075`, `.0875`
changed mean SSIM by `+0.000047` and mean temporal error by `-0.003570`; the
per-scene temporal changes were `-0.007620`, `+0.000462`, `-0.009038`, and
`+0.001915`. The lighter `.08` point changed mean SSIM by `+0.000019` and
mean temporal error by `-0.001397`, with per-scene temporal changes
`-0.002950`, `+0.000175`, `-0.003556`, and `+0.000745`.

Both points improve the mean while regressing debris or cave, so neither is a
safe global setting. The results support scene-aware or content-aware tuning,
not a blanket strength increase. Raw captures are under
`.quality-tmp/learned075-640-*-20260825/`,
`.quality-tmp/learned080-640-*-20260825/`, and
`.quality-tmp/learned0875-640-*-20260825/`.

As a focused follow-up, `.05` was tested on the two scenes that regressed at
higher learned strength. At 640x360 -> 1920x1080 it improved temporal error
over `.075` by `-0.001251` on Tears debris and `-0.004242` on Sintel cave,
with SSIM changes of `-0.000065` and `-0.000160`. At 1280x720 -> 3840x2160,
it improved cave temporal error by `-0.003670` with a `-0.000102` SSIM change,
while debris moved `+0.000541` temporally with a `-0.000016` SSIM change.

This confirms the tradeoff is content-dependent: cave consistently benefits
from less learned contribution, while debris is near the measurement boundary
and does not justify a blanket reduction. No runtime heuristic or default was
changed. Raw captures are under
`.quality-tmp/learned050-640-*-20260825/` and
`.quality-tmp/learned050-1280-*-20260825/`.

## 2026-08-25 360p confidence-gate follow-up

The real motion-confidence gate was tested at 640x360 -> 1920x1080 with the
`.075` learned-strength lead. The full gate helped the two scenes that favored
less learned contribution: temporal error changed by `-0.001071` on Tears
debris and `-0.001972` on Sintel cave. It hurt the other two scenes by
`+0.005089` and `+0.010126` on daylight and rooftop, while SSIM fell on all
four scenes by `0.000050-0.000106`. It is rejected as a global policy.

A lighter `TFORGE_FSR4_LEARNED_CONFIDENCE_BLEND=0.75` arm reduced the damage
but still changed mean SSIM by `-0.000018` and mean temporal error by
`+0.000821`; daylight and rooftop again regressed while debris and cave
improved slightly. This is also rejected. The results show that the existing
single scalar confidence signal does not safely distinguish the corpus's
content types. Raw captures are under
`.quality-tmp/learned075-gated-640-*-20260825/` and
`.quality-tmp/learned075-conf075-640-*-20260825/`.

## 2026-08-25 live base-filter override check

The retained opt-in lead was rerun with the live current-branch base filter
override set to bilinear. The override is wired into the maintained
composition path; this is not the earlier experimental-only filter check.
The run used 36 warm-up frames and 24 scored frames at 426x240 -> 1920x1080.

| scene | SSIM delta vs retained lead | temporal error delta vs retained lead |
| --- | ---: | ---: |
| Tears daylight | +0.003897 | +0.264306 |
| Tears debris | +0.002733 | +0.338632 |
| Sintel rooftop | +0.017846 | +0.242054 |
| Sintel cave | +0.009252 | +0.323337 |

Bilinear raised still-image SSIM in this short screen, but temporal error
increased on every scene. It is therefore rejected as a quality candidate and
was not promoted or made a default. Raw captures are in
`.quality-tmp/base-filter-bilinear-linear0075-rcas012-cas001-*-20260825/`.

## 2026-08-25 live base-jitter A/B

The retained opt-in lead was compared with the live base-resolve jitter bit
enabled. Both arms used the same software decode, 36 warm-up frames, 24
scored frames, and `.075` ungated linear blending with RCAS `.12` and CAS
`.01`.

| scene | SSIM delta, jitter on - off | temporal error delta, jitter on - off |
| --- | ---: | ---: |
| Tears daylight | -0.002865 | -0.294922 |
| Tears debris | -0.002353 | +0.173467 |
| Sintel rooftop | -0.003010 | +1.221725 |
| Sintel cave | +0.002430 | -0.250881 |

Jitter improved temporal error on daylight and cave, but regressed debris and
rooftop and reduced SSIM on three scenes. It is rejected as a corpus-wide
quality improvement and remains opt-in only. Raw captures are under
`.quality-tmp/base-jittered-*-20260825/` and
`.quality-tmp/base-jitter-control-*-20260825/`.

## 2026-08-25 live Mitchell base-filter check

Mitchell-Netravali was tested in the actual `current` composition rather than
the earlier `base_only` filter setup. The temporary config changed only the
base filter; learned strength, linear blending, RCAS `.12`, CAS `.01`, tone,
and capture protocol were held to the retained lead at 640x360 -> 1920x1080.

Mitchell raised still SSIM by `+0.002274`, `+0.001745`, `+0.005049`, and
`+0.004667` for daylight, debris, rooftop, and cave. Temporal error increased
on every scene by `+0.157860`, `+0.140221`, `+0.195552`, and `+0.188302`.
It is rejected for video and was not promoted. Raw captures are under
`.quality-tmp/mitchell-current-640-*-20260825/`.

## 2026-08-25 live Lanczos2 base-filter check

Lanczos2 was then tested in the same actual `current` composition, with every
other retained-lead setting held fixed at 640x360 -> 1920x1080. Relative to
the Catmull-Rom-like base, SSIM changed by `-0.000135`, `-0.000094`,
`-0.000135`, and `-0.000097` for daylight, debris, rooftop, and cave.
Temporal error improved by `-0.002963`, `-0.001782`, `-0.003621`, and
`-0.003210`, respectively.

Lanczos2 is therefore temporally steadier but spatially worse on every scene;
it is rejected as the overall video-quality winner and remains opt-in only.
Raw captures are under `.quality-tmp/lanczos-current-640-*-20260825/`.

## 2026-08-25 360p linear-versus-sRGB blend check

The retained `.075` blend was rerun at 640x360 -> 1920x1080 without the
linear-light flag, holding RCAS `.12`, CAS `.01`, software decode, and the
36/24 capture protocol fixed. Relative to linear blending, sRGB changed SSIM
by `+0.000062`, `-0.000026`, `+0.000019`, and `+0.000005` for daylight,
debris, rooftop, and cave. Temporal error worsened on every scene by
`+0.002674`, `+0.004268`, `+0.005956`, and `+0.001427`.

The mean SSIM gain was only `+0.000015`, while mean temporal error worsened by
`+0.003581`. sRGB blending is rejected; linear-light blending remains the
retained opt-in lead. Raw captures are under
`.quality-tmp/learned075-srgb-640-*-20260825/`.

## 2026-08-25 360p RCAS 0.16 check

Legacy RCAS strength `.16` was compared with the retained `.12` value at
640x360 -> 1920x1080. SSIM changed by `-0.000006`, `-0.000012`, `-0.000012`,
and `-0.000004`; temporal error changed by `-0.000509`, `-0.000506`,
`-0.001044`, and `-0.000503` for daylight, debris, rooftop, and cave.

The stronger RCAS point improves temporal error on every scene but lowers
SSIM on every scene. Mean changes were `-0.000008` SSIM and `-0.000641`
temporal error, so `.12` remains the better overall balance. Raw captures are
under `.quality-tmp/rcas016-640-*-20260825/`.

## 2026-08-25 color-history interpolation check

Color history was explicitly enabled for both arms at 640x360 -> 1920x1080.
The candidate selected bilinear history interpolation while the control used
the existing history sampler; learned strength, linear blending, RCAS `.12`,
CAS `.01`, software decode, and 36/24 capture settings were identical.
Candidate deltas were SSIM `+0.000001`, `-0.000001`, `+0.000001`, `+0.000001`
and temporal error `-0.000004`, `+0.000058`, `+0.000012`, `+0.000002` for
daylight, debris, rooftop, and cave.

The mean changes were effectively zero (`+0.000001` SSIM and `+0.000017`
temporal error). Bilinear history interpolation is not a quality lever in
this path and was not promoted. Raw captures are under
`.quality-tmp/history-control-640-*-20260825/` and
`.quality-tmp/history-bilinear-640-*-20260825/`.

## 2026-08-25 color-history motion-scale check

With color history explicitly enabled, the decoded motion vectors were tested
at experimental scale factors `0.5` and `2.0` against the unscaled control on
Tears daylight and Sintel cave at 640x360 -> 1920x1080. Relative to the
control, scale `0.5` changed SSIM/temporal error by `+0.000001/-0.000064`
on daylight and `-0.000004/+0.000060` on cave. Scale `2.0` changed them by
`-0.000001/+0.000001` and `+0.000005/+0.000183`.

These are effectively no-ops at measurement precision. The maintained path
does not show a simple motion-magnitude mismatch that explains the quality
gap. The scale overrides remain diagnostic-only. Raw captures are under
`.quality-tmp/history-scale05-640-*-20260825/` and
`.quality-tmp/history-scale20-640-*-20260825/`.

## INVALID 2026-08-25 360p CAS 0.02 check (stale executable)

**Do not use this result for quality decisions.** The capture was accidentally
run with `build/temporal_forge_player`, the older Debug executable that does
not contain the campaign switches. A matched `build-fast` rerun is required.

Final CAS strength `.02` was compared with the retained `.01` value at
640x360 -> 1920x1080. The `.075` ungated linear blend, legacy RCAS `.12`,
software decode, and 36/24 capture settings were held fixed. Relative to
`.01`, SSIM changed by `-0.000007`, `-0.000022`, `-0.000050`, and `-0.000020`
for daylight, debris, rooftop, and cave. Temporal error changed by
`-0.003967`, `-0.002202`, `-0.005058`, and `-0.001360`.

The mean SSIM change was `-0.000025`, while mean temporal error improved by
`-0.003147`. This is a small temporal-only improvement with a consistent
SSIM loss, so `.02` remains opt-in and `.01` stays the retained balance.
Raw captures are under `.quality-tmp/cas002-640-*-20260825/`.

## INVALID 2026-08-25 color-history disable check (stale executable)

**Do not use this result for quality decisions.** The capture used the older
`build/temporal_forge_player` executable rather than the documented
`build-fast/temporal_forge_player` quality binary. Its opt-in switch was not
present in the executable.

The retained `.075` ungated linear blend, legacy RCAS `.12`, CAS `.01`,
software decode, and 36/48 capture protocol were rerun at 640x360 ->
1920x1080 with `TFORGE_FSR4_DISABLE_COLOR_HISTORY=1`. Disabling color history
reduced SSIM by `-0.134169`, `-0.193751`, `-0.198735`, and `-0.045272` and
changed temporal error by `-0.129239`, `+2.696877`, `+8.315562`, and
`+1.104295` for daylight, debris, rooftop, and cave.

This is a large regression on three scenes and a spatial regression on all
four. Color history is therefore required by the retained path; the disable
switch remains diagnostic-only. Raw captures are under
`.quality-tmp/no-color-history-640-*-20260825/`.

## INVALID 2026-08-25 motion-validity gate disable check (stale executable)

**Do not use this result for quality decisions.** The capture used the older
`build/temporal_forge_player` executable rather than the documented
`build-fast/temporal_forge_player` quality binary. Its opt-in switch was not
present in the executable.

The retained lead was rerun with `TFORGE_FSR4_DISABLE_MOTION_VALIDITY=1` at
640x360 -> 1920x1080. The candidate reduced SSIM by `-0.134169`, `-0.193751`,
`-0.198780`, and `-0.045272` and changed temporal error by
`-0.129239`, `+2.696874`, `+8.315568`, and `+1.104296` for daylight, debris,
rooftop, and cave.

Mean SSIM fell by `-0.142993` and mean temporal error worsened by `+2.996875`.
Removing the validity gate is rejected. The nearly identical collapse to the
color-history disable arm also suggests this switch changes the same temporal
resource path rather than simply admitting more vectors; that interaction is
now a follow-up reverse-engineering target, not a quality promotion.
Raw captures are under `.quality-tmp/no-motion-validity-640-*-20260825/`.

## 2026-08-25 build-fast fixed-history-weight recheck

The fixed `0.1` current-frame history-weight interpretation was rerun with
the documented `build-fast/temporal_forge_player` binary at 640x360 ->
1920x1080. The retained `.075` ungated linear blend, RCAS `.12`, CAS `.01`,
software decode, and 36/48 capture protocol were identical between arms.

Relative to the retained lead, SSIM changed by `-0.000057`, `+0.000000`,
`+0.000040`, and `+0.000000`, while temporal error changed by
`+0.000001`, `-0.000005`, `-0.000006`, and `+0.000000` for daylight, debris,
rooftop, and cave. Mean changes were `-0.00000425` SSIM and `-0.00000250`
temporal error, both within measurement noise.

The candidate is not promoted. This clean build-fast recheck supersedes the
stale-executable result above. Raw captures are under
`.quality-tmp/buildfast-fixed-history-640-*-20260825/`; the matched lead is
under `.quality-tmp/buildfast-lead-640-*-20260825/`.

## 2026-08-25 build-fast chroma-bilinear check

Chroma bilinear sampling was compared with the retained chroma path using the
verified `build-fast/temporal_forge_player` binary at 640x360 -> 1920x1080.
All learned-blend, history, RCAS, CAS, decode, warm-up, and frame-count
settings were held fixed.

Relative to the lead, SSIM changed by `-0.000109`, `-0.000275`, `-0.000195`,
and `+0.000072`; temporal error changed by `+0.001019`, `+0.000980`,
`+0.001682`, and `-0.003112` for daylight, debris, rooftop, and cave.
Mean SSIM fell by `-0.000127` and mean temporal error worsened by `+0.000142`.

Chroma bilinear is rejected and remains opt-in only. Raw captures are under
`.quality-tmp/buildfast-chroma-bilinear-640-*-20260825/`.

## 2026-08-25 build-fast nearest-history check

Nearest-neighbor sampling for reprojected color history was compared with the
retained history sampler using `build-fast/temporal_forge_player` at 640x360
-> 1920x1080. All other lead settings and the 36/48 capture protocol were
identical.

Relative to the lead, SSIM changed by `+0.000052`, `-0.000104`, `-0.000304`,
and `+0.000015`; temporal error changed by `-0.000006`, `+0.000009`,
`-0.000003`, and `-0.000003` for daylight, debris, rooftop, and cave. Mean
SSIM fell by `-0.000085`, while mean temporal error improved by only
`-0.000001`.

Nearest history is rejected as a quality improvement and remains opt-in only.
Raw captures are under
`.quality-tmp/buildfast-history-nearest-640-*-20260825/`.

## 2026-08-25 build-fast motion-coordinate rounding check

Motion reprojection source coordinates were rounded with `floor` and `ceil`
against the retained lead, using `build-fast/temporal_forge_player` at
640x360 -> 1920x1080. All other settings and the 36/48 capture protocol were
identical.

`floor` was byte-equivalent to the lead on daylight, rooftop, and cave, but
reduced Tears debris SSIM by `-0.021255`; its mean SSIM change was `-0.005314`
and mean temporal error changed by `+0.000001`.

`ceil` changed SSIM by `+0.000000`, `+0.000000`, `-0.000001`, and `+0.000079`
and temporal error by `+0.000000`, `+0.000008`, `-0.000001`, and `-0.000001`
for daylight, debris, rooftop, and cave. The mean SSIM gain was only
`+0.000020`, while mean temporal error worsened by `+0.0000015`, so it is
measurement noise rather than a promotion candidate.

Neither rounding mode is promoted. Raw captures are under
`.quality-tmp/buildfast-motion-floor-640-*` and
`.quality-tmp/buildfast-motion-ceil-640-*`.

## 2026-08-25 build-fast motion-sign inversion check

The decoded motion field was inverted with
`TFORGE_FSR4_EXPERIMENTAL_MOTION_SIGN=invert` against the retained lead using
`build-fast/temporal_forge_player` at 640x360 -> 1920x1080. All other settings
and the 36/48 capture protocol were identical.

SSIM changed by `+0.000053`, `-0.000104`, `-0.000304`, and `-0.019874`; temporal
error changed by `-0.000002`, `-0.000015`, `+0.000002`, and `-0.000003` for
daylight, debris, rooftop, and cave. Mean SSIM fell by `-0.005057`; the mean
temporal improvement of `-0.000005` does not offset the spatial loss.

Motion-sign inversion is rejected. Raw captures are under
`.quality-tmp/buildfast-motion-invert-640-*-20260825/`.

## 2026-08-25 build-fast motion-scale check

Motion-vector magnitudes were tested at `0.5x` and `2.0x` against the retained
lead using `build-fast/temporal_forge_player` at 640x360 -> 1920x1080. All
other settings and the 36/48 capture protocol were identical.

At `0.5x`, SSIM changed by `+0.000011`, `+0.000000`, `-0.018618`, and
`+0.000079`; temporal error changed by `-0.000005`, `-0.000005`, `-0.000001`,
and `+0.000002` for daylight, debris, rooftop, and cave. The mean SSIM loss
was `-0.004632`.

At `2.0x`, SSIM changed by `-0.000057`, `+0.000000`, `-0.000001`, and
`+0.000079`; temporal error changed by `-0.000001`, `+0.000000`, `+0.000003`,
and `+0.000001`. The mean SSIM gain was only `+0.000005`, with a mean temporal
regression of `+0.000001`.

Neither scale is promoted. Raw captures are under
`.quality-tmp/buildfast-motion-scale-05-640-*` and
`.quality-tmp/buildfast-motion-scale-20-640-*`.

## 2026-08-25 build-fast history-confidence threshold 0.9 check

The history-confidence gate was enabled at threshold `0.9` against the
retained lead using `build-fast/temporal_forge_player` at 640x360 -> 1920x1080.
All other settings and the 36/48 capture protocol were identical.

SSIM changed by `+0.000052`, `-0.000104`, `-0.000304`, and `-0.000096`; temporal
error changed by `-0.000003`, `+0.000008`, `-0.000002`, and `+0.000002` for
daylight, debris, rooftop, and cave. Mean SSIM fell by `-0.000113`, while mean
temporal error worsened by `+0.000001`.

The stricter history gate is rejected. Raw captures are under
`.quality-tmp/buildfast-history-threshold09-640-*-20260825/`.

## 2026-08-25 build-fast bilinear-history interpolation check

History reprojection was switched to bilinear interpolation with
`TFORGE_FSR4_EXPERIMENTAL_HISTORY_INTERPOLATION=bilinear` against the retained
lead using `build-fast/temporal_forge_player` at 640x360 -> 1920x1080. All
other settings and the 36/48 capture protocol were identical.

SSIM changed by `+0.000052`, `-0.000104`, `-0.000304`, and `-0.019874`; temporal
error changed by `-0.000008`, `+0.000013`, `-0.000001`, and `-0.000001` for
daylight, debris, rooftop, and cave. Mean SSIM fell by `-0.005058`, while the
mean temporal error change was only `+0.000001`.

Bilinear history interpolation is rejected because it loses spatial quality,
including a severe Sintel Cave regression. Raw captures are under
`.quality-tmp/buildfast-history-bilinear-640-*-20260825/`.

## 2026-08-25 build-fast recovered-linear-output check

The recovered postpass output was kept in linear space with
`TFORGE_FSR4_EXPERIMENTAL_RECOVERED_LINEAR_OUTPUT=1` against the retained lead
using `build-fast/temporal_forge_player` at 640x360 -> 1920x1080. All other
settings and the 36/48 capture protocol were identical.

SSIM changed by `-0.128368`, `-0.104153`, `-0.088880`, and `-0.012072`; temporal
error changed by `+0.710429`, `+2.274251`, `+1.205792`, and `+1.788286` for
daylight, debris, rooftop, and cave. The candidate regressed both spatial and
temporal quality on every scene.

Recovered linear output is rejected. The normal SDR output transform remains
required. Raw captures are under
`.quality-tmp/buildfast-recovered-linear-640-*-20260825/`.

## 2026-08-25 build-fast Rec.709 input-EOTF check

The input transfer path was forced to the Rec.709 EOTF with
`TFORGE_FSR4_EXPERIMENTAL_REC709_INPUT_EOTF=1` against the retained lead using
`build-fast/temporal_forge_player` at 640x360 -> 1920x1080. All other settings
and the 36/48 capture protocol were identical. The source clips declare an
unknown transfer function; Sintel declares BT.709 matrix metadata, while the
Tears clips leave matrix metadata unspecified.

SSIM changed by `-0.009414`, `-0.019637`, `-0.008311`, and `-0.026022`;
temporal error changed by `+0.038157`, `+0.168216`, `+0.017205`, and
`-0.450754` for daylight, debris, rooftop, and cave. Mean SSIM fell by
`-0.015846`. Cave's temporal improvement came with a severe spatial loss, and
the other three scenes regressed temporally as well.

Rec.709 input EOTF is rejected. Raw captures are under
`.quality-tmp/buildfast-rec709-eotf-640-*-20260825/`.

## 2026-08-25 build-fast unspecified-matrix BT.709 check

Missing matrix metadata was interpreted as BT.709 with
`TFORGE_FSR4_EXPERIMENTAL_UNKNOWN_MATRIX_BT709=1` against the retained lead
using `build-fast/temporal_forge_player` at 640x360 -> 1920x1080. All other
settings and the 36/48 capture protocol were identical. This changes the
Tears clips' SD fallback; Sintel already declares BT.709 matrix metadata and
serves as a control.

SSIM changed by `-0.000313`, `-0.000613`, `-0.000304`, and `+0.000015`;
temporal error changed by `-0.032484`, `+0.004880`, `+0.000003`, and
`-0.000001` for daylight, debris, rooftop, and cave. The daylight temporal
improvement does not offset its spatial loss, while debris worsens on both
metrics. The candidate is not a corpus-wide quality improvement and remains
opt-in only. Raw captures are under
`.quality-tmp/buildfast-unknown-matrix-bt709-640-*-20260825/`.

## 2026-08-25 build-fast wide learned-kernel exponent check

The learned-kernel wide exponent interpretation was enabled with
`TFORGE_FSR4_EXPERIMENTAL_LEARNED_KERNEL_WIDE_EXPONENT=1` against the retained
lead using `build-fast/temporal_forge_player` at 640x360 -> 1920x1080. All
other settings and the 36/48 capture protocol were identical.

SSIM changed by `+0.000052`, `-0.000104`, `-0.000317`, and `-0.002096`;
temporal error changed by `-0.000004`, `-0.000011`, `-0.000002`, and
`-0.000001` for daylight, debris, rooftop, and cave. The small temporal
movement is outweighed by the spatial loss, including a clear Sintel Cave
regression.

The wide exponent is rejected. Raw captures are under
`.quality-tmp/buildfast-wide-exponent-640-*-20260825/`.

## 2026-08-25 build-fast legacy learned-kernel normalization check

The learned-kernel legacy normalization floor was enabled with
`TFORGE_FSR4_EXPERIMENTAL_LEARNED_KERNEL_LEGACY_NORMALIZATION=1` against the
retained lead using `build-fast/temporal_forge_player` at 640x360 -> 1920x1080.
All other settings and the 36/48 capture protocol were identical.

SSIM changed by `+0.000052`, `-0.000104`, `-0.000304`, and `+0.000160`;
temporal error changed by `-0.000001`, `-0.000007`, `+0.000024`, and
`+0.000000` for daylight, debris, rooftop, and cave. Mean SSIM fell by
`-0.000049`, and mean temporal error worsened by `+0.000004`.

Legacy normalization is rejected as a corpus-wide improvement. Raw captures
are under `.quality-tmp/buildfast-legacy-normalization-640-*-20260825/`.

## 2026-08-25 build-fast raw learned-kernel normalization check

The learned-kernel normalization floor was removed with
`TFORGE_FSR4_EXPERIMENTAL_LEARNED_KERNEL_RAW_NORMALIZATION=1` against the
retained lead using `build-fast/temporal_forge_player` at 640x360 -> 1920x1080.
All other settings and the 36/48 capture protocol were identical.

SSIM changed by `+0.000052`, `-0.000189`, `-0.000318`, and `-0.002096`;
temporal error changed by `+0.000000`, `-0.000007`, `-0.000003`, and
`-0.000001` for daylight, debris, rooftop, and cave. Mean SSIM fell by
`-0.000638`; the small mean temporal improvement was not meaningful.

Raw normalization is rejected. Raw captures are under
`.quality-tmp/buildfast-raw-normalization-640-*-20260825/`.

## 2026-08-25 build-fast motion-aware residual check at learned strength 0.075

The motion-aware residual gate was tested with
`TFORGE_FSR4_MOTION_AWARE_RESIDUAL=1` on the retained `.075` lead using
software decode, `build-fast/temporal_forge_player`, and 640x360 -> 1920x1080.
All other settings and the 36/48 capture protocol were identical. Software
decode was used because the hardware path reports no reactive signal.

SSIM changed by `+0.000052`, `-0.000104`, `-0.000318`, and `+0.000033`;
temporal error changed by `-0.000004`, `-0.000016`, `+0.000000`, and
`-0.000001` for daylight, debris, rooftop, and cave. Mean SSIM fell by
`-0.000084`; the small temporal improvement is not enough to offset that
spatial loss.

The motion-aware residual gate remains opt-in and is rejected as a global
quality improvement. Raw captures are under
`.quality-tmp/buildfast-motion-aware-residual-640-*-20260825/`.

## 2026-08-25 build-fast display-base strength 0.25 check

The maintained composition was given a 25% display-space base contribution
with `TFORGE_FSR4_DISPLAY_BASE_STRENGTH=0.25` against the retained lead using
`build-fast/temporal_forge_player` at 640x360 -> 1920x1080. All other settings
and the 36/48 capture protocol were identical.

SSIM improved by `+0.000367`, `+0.000101`, `+0.000312`, and `+0.000368`;
temporal error worsened by `+0.016300`, `+0.018613`, `+0.026874`, and
`+0.012134` for daylight, debris, rooftop, and cave. This is a consistent
still-image gain but a consistent temporal regression.

Display-base strength 0.25 is rejected for video quality and remains opt-in.
Raw captures are under
`.quality-tmp/buildfast-display-base025-640-*-20260825/`.

## 2026-08-25 build-fast postpass current-weight 0.05 check

The learned/current composition was given a 5% current-frame contribution
with `TFORGE_FSR4_POSTPASS_CURRENT_WEIGHT=0.05` against the retained lead
using `build-fast/temporal_forge_player` at 640x360 -> 1920x1080. All other
settings and the 36/48 capture protocol were identical.

SSIM changed by `+0.000047`, `-0.000192`, `-0.000310`, and `-0.000098`;
temporal error changed by `-0.000189`, `-0.000095`, `-0.000238`, and
`-0.000062` for daylight, debris, rooftop, and cave. Temporal error improved
on every scene, but mean SSIM fell by `-0.000138`.

The 5% current-frame blend is a promising temporal-stability candidate but is
not promoted because of the spatial cost. Raw captures are under
`.quality-tmp/buildfast-current-weight005-640-*-20260825/`.

## 2026-08-25 build-fast postpass current-weight 0.02 check

The same learned/current composition was tested at a 2% current-frame
contribution with `TFORGE_FSR4_POSTPASS_CURRENT_WEIGHT=0.02` against the
retained lead using `build-fast/temporal_forge_player` at 640x360 -> 1920x1080.
All other settings and the 36/48 capture protocol were identical.

SSIM changed by `+0.000050`, `-0.000190`, `-0.000306`, and `+0.000014`;
temporal error changed by `-0.000079`, `-0.000042`, `-0.000096`, and
`-0.000024` for daylight, debris, rooftop, and cave. Temporal error improved
on every scene, and mean SSIM fell by only `-0.000108`.

The 2% current-frame blend is the best temporal-stability point tested in
this composition sweep, but remains opt-in pending visual review because it
still loses spatial SSIM. Raw captures are under
`.quality-tmp/buildfast-current-weight002-640-*-20260825/`.

## 2026-08-25 build-fast postpass current-weight 0.01 check

The same learned/current composition was tested at a 1% current-frame
contribution with `TFORGE_FSR4_POSTPASS_CURRENT_WEIGHT=0.01` against the
retained lead using `build-fast/temporal_forge_player` at 640x360 -> 1920x1080.
All other settings and the 36/48 capture protocol were identical.

SSIM changed by `+0.000051`, `-0.000190`, `-0.000017`, and `-0.002096`;
temporal error changed by `-0.000041`, `-0.000015`, `-0.000051`, and
`-0.000017` for daylight, debris, rooftop, and cave. The large Sintel Cave
spatial regression outweighs the small temporal improvement.

The 1% current-frame blend is rejected. Raw captures are under
`.quality-tmp/buildfast-current-weight001-640-*-20260825/`.

## 2026-08-25 build-fast postpass current-weight 0.02 cross-resolution check

The retained 2% postpass blend was rechecked at the 1280x720 input tier with
3840x2160 output using `build-fast/temporal_forge_player`. The control and
candidate used the same four real scenes, software decode, 36-frame warmup,
48-frame capture, and all retained lead settings; the candidate added only
`TFORGE_FSR4_POSTPASS_CURRENT_WEIGHT=0.02`.

For daylight, debris, rooftop, and cave respectively, SSIM changed by
`+0.000000`, `-0.000001`, `-0.000002`, and `-0.000001`; temporal error changed
by `-0.000054`, `+0.000018`, `-0.000058`, and `-0.000001`. Mean SSIM changed
by `-0.000001`; mean temporal error changed by `-0.00002375`.

This cross-resolution check confirms the same small temporal-stability tradeoff
seen at 640x360 -> 1920x1080: the 2% blend remains a useful opt-in candidate,
but it is not promoted as the default because the spatial gain is absent.
Raw captures are under `.quality-tmp/buildfast-*-1280-*-20260825.csv`.

## 2026-08-25 build-fast motion-validity A/B check

The explicit codec-motion validity image was compared against the
benchmark-only `TFORGE_FSR4_DISABLE_MOTION_VALIDITY=1` path at 640x360 input
with 1920x1080 intended output. Both sides used the retained lead settings,
software decode, the same four real scenes, 36-frame warmup, and 48-frame
capture. The validity-enabled side was the default of the matched run.

For daylight, debris, rooftop, and cave respectively, SSIM changed by
`+0.000000`, `-0.000001`, `+0.000000`, and `+0.000000`; temporal error changed
by `-0.000001`, `-0.000019`, `+0.000003`, and `+0.000001`. Mean SSIM changed
by `-0.00000025`, and mean temporal error changed by `-0.000004`.

The validity path is wired and contract-tested, but this corpus slice does
not contain enough exposed sparse/disocclusion behavior to produce a useful
quality gain. It remains the default semantic guard, but is not promoted as
a measurable quality improvement from this A/B. Raw captures are under
`.quality-tmp/buildfast-motion-validity-{on,off}-640-*-20260825.csv`.

## 2026-08-25 build-fast postpass-tail disable check

The opt-in `TFORGE_FSR4_EXPERIMENTAL_DISABLE_POSTPASS_TAIL=1` path was
compared with the fresh validity-enabled retained lead at 640x360 input and
1920x1080 intended output. All other settings, binary, four real scenes,
software decode, warmup, and capture length were identical.

For daylight, debris, rooftop, and cave respectively, SSIM changed by
`+0.000000`, `+0.000070`, `+0.000000`, and `-0.000048`; temporal error changed
by `+0.000004`, `-0.000017`, `-0.000002`, and `+0.000000`. Mean SSIM changed
by `+0.0000055`, and mean temporal error changed by `-0.00000375`.

The result is mixed and effectively neutral. Disabling the postpass tail is
not a quality improvement and remains diagnostic-only. Raw captures are under
`.quality-tmp/buildfast-postpass-tail-disabled-640-*-20260825.csv`.

## 2026-08-25 build-fast full-postpass bypass localization check

The benchmark-only `TFORGE_FSR4_DISABLE_POSTPASS=1` path was compared with
the fresh validity-enabled retained lead at 640x360 input and 1920x1080
intended output. All other settings, binary, four real scenes, software
decode, warmup, and capture length were identical.

Disabling the postpass changed SSIM by `-0.263619`, `-0.210836`, `-0.174503`,
and `-0.021727` and changed temporal error by `+1.705945`, `+7.937136`,
`+3.849694`, and `+2.392879` for daylight, debris, rooftop, and cave.

This is a decisive localization result, not a candidate: the postpass is
essential to the reconstructed output, and removing it makes quality much
worse. Further work should target the postpass's internal decode/composition
semantics rather than bypassing it. Raw captures are under
`.quality-tmp/buildfast-postpass-disabled-640-*-20260825.csv`.

## 2026-08-25 build-fast empty-motion confidence check

The opt-in `TFORGE_FSR4_EXPERIMENTAL_EMPTY_MOTION_CONFIDENCE=1` policy was
checked against the retained build-fast lead at 640x360 -> 1920x1080. The
capture used software decode, learned strength `0.075`, linear current
blending, ungated learned confidence, RCAS `0.12`, CAS `0.01`, 36 warm-up
frames, and 48 captured frames across all four real scenes.

The candidate regressed every scene. SSIM deltas were `-0.043454`,
`-0.027748`, `-0.035976`, and `-0.003501` for Tears daylight, Tears debris,
Sintel rooftop, and Sintel cave. Temporal absolute-error deltas were
`+0.234434`, `+0.077135`, `+0.297421`, and `+0.391672` respectively.

This is a rejected motion-confidence policy, not a quality lead. The empty
motion fallback remains opt-in and the default is unchanged. Raw captures are
under `.quality-tmp/buildfast-empty-motion-confidence1-640-*-20260825/`.

## 2026-08-25 provenance correction for the retained build-fast lead

The earlier `buildfast-lead-640-*` and related experiment CSVs are not valid
quality baselines. A fresh replay with the same nominal player settings does
not reproduce them, and—more importantly—their Lanczos control is also
different: the saved lead reports roughly `0.898` Lanczos SSIM on Tears
daylight, while the fresh matched replay reports `0.853`. Lanczos is generated
by the benchmark script from the same input/reference pair, so this cannot be
explained by a Temporal Forge shader change. It indicates a frame-selection,
reference-alignment, or capture-provenance mismatch.

The old lead, legacy-normalization, motion, and empty-motion comparisons must
therefore be treated as contaminated historical artifacts, not evidence for
promoting or rejecting a reconstruction setting. The clean replay artifacts
under `.quality-tmp/fresh-strength-0.075-*-20260825/` and the paired
`qualitylab-enabled/disabled-daylight-20260825` captures are the valid
provenance for the next campaign step. No reconstruction default was changed
from this audit.

## 2026-08-25 clean replay: jitter and learned-strength follow-up

The contaminated historical lead was not used for these comparisons. Both
arms used the same `build-fast/temporal_forge_player`, software decode,
explicitly disabled Quality Lab file, 36 warm-up frames, 48 scored frames,
and the four real-world scenes.

Disabling jitter reduced SSIM on all four scenes and worsened temporal error
on Tears daylight, Tears debris, and Sintel rooftop. Jitter-off remains an
opt-in diagnostic and is not promoted.

Increasing learned strength from `.075` to `.10` increased SSIM on all four
scenes. It improved temporal error on Tears daylight and Sintel rooftop, but
worsened it on Tears debris and Sintel cave. The mean temporal result improved
slightly, but the per-scene tradeoff is not safe enough for a global default.
The `.10` arm is retained as a review candidate only. Enabling the existing
confidence gate at `.10` removed some of the debris/cave regression but also
gave back spatial quality and worsened daylight/rooftop temporal error, so it
was not promoted either.

Raw captures are under `.quality-tmp/clean-jitter-*`,
`.quality-tmp/clean-strength-0.075-*`, `.quality-tmp/clean-strength-0.10-*`,
and `.quality-tmp/clean-strength010-gated-*`.

The `.0875` midpoint was also replayed under the clean protocol. It gave back
some of `.10`'s spatial gain and had a worse four-scene mean temporal result,
so it is not retained as the preferred strength.

The `.10` endpoint was then checked at the 1280x720 input tier. Mean SSIM
changed from `0.942438` to `0.942475` and mean temporal absolute error changed
from `0.394031` to `0.391580`. The improvement is small, and Sintel Cave
still has a temporal regression, so `.10` remains a review-only candidate
rather than a global default.

The 1280-tier captures are under
`.quality-tmp/clean-strength1280-0.075-*` and
`.quality-tmp/clean-strength1280-0.10-*`.

The two remaining jitter probes were run with metrics only and temporary
artifacts outside the repository. Halton(3,2) reduced temporal error on all
four scenes, but lowered SSIM slightly on all four. Holding each phase for two
frames worsened temporal error on three scenes and was rejected. Halton(3,2)
is retained as a temporal-stability review candidate, not a global quality
promotion.

A matched Tears daylight replay also compared software and hardware decode.
The two paths were effectively identical: SSIM was `0.854204` in both arms,
and temporal absolute error differed by only `0.000010`. The visible quality
gap is not explained by the AMD hardware-decoder upload path for this corpus
sample.

The actual unoverridden 426x240 playback default was compared with an
explicit `.10` ungated arm and an explicit `.075` normally gated arm. The
default uses the severe-upscale learned-strength floor of `.05`. The `.075`
gated arm raised mean SSIM slightly and reduced mean temporal error across
the four scenes versus that default, while the `.10` arm produced the larger
spatial gain but a larger debris/cave temporal tradeoff. The `.075` gated arm
is the best balanced severe-upscale candidate so far, but remains opt-in
until it is repeated on additional real scenes.

The clean `.10` follow-ups were also combined with a 2% postpass
current-frame contribution and with the lighter `learnedConfidenceBlend=0.75`
policy. The current-frame blend was effectively neutral and lost a small
amount of SSIM on three scenes. The lighter confidence blend improved the
debris/cave temporal values slightly, but lost spatial quality and worsened
daylight/rooftop temporal values. Neither combination is promoted.

## 2026-08-25 deterministic 1920x1080 strength replay

Two sequential replays of the same software-decoded Tears of Steel daylight
input were compared with an explicit `TFORGE_FSR4_FORCE_VIEWPORT=1920x1080`.
They differed by only a few pixels per frame (about `1e-6` of the image), and
the reported SSIM values differed by `0.000001`. This is small GPU
floating-point variation, not frame substitution. An invalid one-value
viewport setting instead fell back to the player window and produced
`1278x720`, so that capture is not 1920x1080 evidence.

The conservative strength sweep was then repeated at the intended target with
8 frames per arm and software decode. Relative to the matched severe-tier
default (`0.05` learned strength), explicit `0.055` raised SSIM on all four
real scenes by approximately `+0.00003` to `+0.00005`. Temporal absolute-error
changes were `-0.003587`, `+0.000620`, `+0.003157`, and `-0.002209` for Tears
daylight, Tears debris, Sintel rooftop, and Sintel cave respectively. It is a
small spatial improvement with small scene-dependent temporal tradeoffs, so it
remains an opt-in candidate.

Explicit `0.06` also raised SSIM on all four scenes, but its temporal
tradeoffs were larger on debris and rooftop. Explicit `0.075` produced the
largest spatial gain in the short replay, but likewise traded temporal error
on rooftop. None of these strengths is promoted as a global default yet.

Raw replay artifacts are under `/tmp/tforge-strength0055-corpus.K44Qcp`,
`/tmp/tforge-strength006-corpus.VQRKNF`,
`/tmp/tforge-default8-corpus.1Cg9cP`, and
`/tmp/tforge-strength075-corpus.fj36fY`.

The `0.055 + Halton32` combination reduced temporal error for daylight,
debris, and cave, but increased it slightly for rooftop and lowered SSIM on
all four scenes by roughly `0.000005` to `0.000153`. It is therefore a useful
temporal-stability probe, not a global quality promotion. Its artifacts are
under `/tmp/tforge-strength0055-halton32.ufswT1`.

Enabling display-color history alongside `0.055` was mixed: it improved the
debris temporal error by about `0.001`, but worsened daylight, rooftop, and
cave temporal error and slightly reduced SSIM on three scenes. It is rejected
as a global improvement. Its artifacts are under
`/tmp/tforge-strength0055-colorhistory.Xc2i2f`.

The `0.055` strength with `TFORGE_FSR4_LEARNED_CONFIDENCE_BLEND=0.75` raised
SSIM on all four scenes, but increased rooftop temporal error by about
`0.012` and also worsened debris. The softer confidence mix is rejected; it
does not solve the motion-heavy scene tradeoff. Its artifacts are under
`/tmp/tforge-strength0055-confidence075.P56dLB`.

Fixed `0.1` current-frame history weighting with color history enabled also
failed to improve the 0.055 arm: SSIM fell slightly on three scenes and
temporal error worsened on all four. This history interpretation is rejected
for this severe-upscale path. Its artifacts are under
`/tmp/tforge-strength0055-fixedhistory.dO9YNT`.

Native INT8 recurrent feedback was tested directly with
`TFORGE_FSR4_ENABLE_RECURRENT=1` at 426x240 -> 1920x1080. It raised SSIM by
only `0.000015` to `0.000162`, while temporal error worsened on every scene by
approximately `0.0057` to `0.0127`. Recurrent feedback is rejected for this
video path and remains opt-in. Its artifacts are under
`/tmp/tforge-recurrent-native.FSKG0T`.

## 2026-08-25 build-fast jitter and fixed-history recheck

The jitter A/B was rerun with the authoritative `build-fast/temporal_forge_player`
binary, explicit 1920x1080 output, software decode, and eight scored frames on
the real Tears of Steel daylight clip. The default Halton sequence remained the
best spatial result:

| Policy | SSIM | Temporal absolute error |
| --- | ---: | ---: |
| Default Halton | `0.855350` | `1.445414` |
| Jitter off | `0.855112` | `1.457151` |
| Reduced | `0.855216` | `1.450750` |
| Controlled 0.5 | `0.855215` | `1.450757` |

No jitter policy is promoted. The earlier sandboxed capture using the stale
`build/` binary was discarded and is not campaign evidence.

The reference-style fixed `0.1` current-frame history weight was then checked
with color history enabled on all four real scenes at 426x240 -> 1920x1080.
The reported SSIM values were daylight `0.855351`, debris `0.937575`, rooftop
`0.800829`, and cave `0.989665`, matching the default to the reported
precision. This is not a useful quality lever and remains opt-in only.

The next investigation remains the generic graph's resolution-specific
32->64 pointwise boundary. No default reconstruction behavior or benchmark
image was changed by these checks.

## 2026-08-26 temporal resolve regression audit

The 640x360 -> 1920x1080 campaign was repeated on four real motion scenes with
software decode, no sharpening, learned strength `0.55`, color history and
recurrent state enabled. The existing opt-in
`TFORGE_FSR4_EXPERIMENTAL_SINGLE_HISTORY_BLEND=1` bypasses the postpass blend
of the prepass-published current/history resolve. This is the only tested
change that materially reduced temporal error across the corpus: in the
12-frame capture, absolute temporal error was daylight `0.715638`, debris
`0.002124`, rooftop `0.595837`, and cave `0.042624`, versus matched spatial
Lanczos values `1.083776`, `0.017900`, `0.094528`, and `0.133265`.

The result is directionally useful but not a universal win: rooftop remains
behind Lanczos on temporal error, and the single-blend path is still an opt-in
diagnostic. Its artifacts are under
`/mnt/external/Temporal Forge/quality-campaign/phase1/single_history_blend_640_20260826_12`.

An existing history-confidence threshold was then tested with the single-blend
path. Threshold `0.85` modestly improved the eight-frame temporal error on all
four scenes, including rooftop (`0.574286` to `0.549643`). A threshold of `1.0`
did not improve rooftop further. Cheap block-motion replacement and global
motion inversion were also tested on rooftop; neither improved the result.
These artifacts are under
`/mnt/external/Temporal Forge/quality-campaign/phase1/single_history_confidence_085_20260826`,
`/mnt/external/Temporal Forge/quality-campaign/phase1/single_history_motion_replace_rooftop_20260826`,
and `/mnt/external/Temporal Forge/quality-campaign/phase1/single_history_motion_direction_rooftop_20260826`.

No default reconstruction behavior was promoted by this audit.
