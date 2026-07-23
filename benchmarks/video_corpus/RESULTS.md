# Real-world corpus results

Measured 2026-07-17 through 2026-07-18 on an RX 7900 GRE using the native RDNA3
INT8 path. Quality measurements use frame 48 and the matching lossless native-4K
frame.

## Quality correction

The checkerboard-like edge blending had two spatial causes:

- GPU YUV conversion used approximate normalized limited-range offsets instead
  of the exact 8-bit code values `16/255` and `128/255`. This primarily damaged
  red/blue structural similarity.
- The learned anisotropic postpass fully replaced the stable current-frame
  reconstruction. A 55% learned contribution retains the INT8 model's edge
  recovery while suppressing unstable alternating filter directions.

Centered bilinear 4:2:0 chroma reconstruction now replaces nearest 2x2 chroma
replication. Forced-reset testing changed the worst pre-fix SSIM by only
`-0.00034`, ruling out temporal history as the primary defect.

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
prepass display-color history is now opt-in; causal model history remains
active and presented.

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
