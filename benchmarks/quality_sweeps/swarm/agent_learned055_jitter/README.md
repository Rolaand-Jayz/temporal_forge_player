# Learned 0.55 controlled-jitter paired matrix

Design-only package. No GPU captures were run. This package changes no source,
defaults, shaders, models, playback behavior, or benchmark images.

## Scope

The matrix has two arms at each controlled jitter value:

| arm | learned strength | jitter mode | jitter strength |
| --- | ---: | --- | ---: |
| `current_default` | unset, host default | `controlled` | `0.0`, `0.5`, `1.0` |
| `learned_055_control` | `0.55` | `controlled` | `0.0`, `0.5`, `1.0` |

The exact environment names are the repository's existing contract:
`TFORGE_FSR4_JITTER_MODE=controlled` and
`TFORGE_FSR4_CONTROLLED_JITTER=<0.0|0.5|1.0>`. The learned arm additionally
sets `TFORGE_FSR4_LEARNED_STRENGTH=0.55`. `current_default` leaves that
variable unset, preserving the host learned-strength default while matching
the learned arm's jitter setting.

There are 2 arms × 3 jitter values × 4 scenes × 2 input resolutions = 48
captures and 24 learned-versus-baseline pairs. Use only these real scenes:
`tos_daylight`, `tos_debris`, `sintel_rooftop`, and `sintel_cave`.

Use the high-quality `crf12` clips. Expected output dimensions are
`1920x1080` for `426x240` input and `3840x2160` for `1280x720` input. Every
fresh player process must run 36 warmup frames followed by 24 scored frames.
`matrix.json` is authoritative.

## Exact isolation and capture shape

Run each cell in a fresh process and fresh output path. Unset every variable in
`matrix.json` under `isolation.mustRemainUnset`, including all temporal
sidecars. Set only the arm's declared environment, plus
`TFORGE_TEMPORAL_WARMUP_FRAMES=36`. Pass `24` as the scored-frame argument to
`benchmarks/video_corpus/run_temporal_quality.sh`.

The command shape is:

```bash
env \
  -u TFORGE_QUALITY_LAB_CONFIG \
  -u TFORGE_TEMPORAL_MOTION_JSON \
  -u TFORGE_TEMPORAL_METRICS_CSV \
  -u TFORGE_TEMPORAL_EVENTS_JSON \
  -u TFORGE_TEMPORAL_STATIC_MASK_JSON \
  -u TFORGE_TEMPORAL_CLASS \
  -u TFORGE_FSR4_ENABLE_COLOR_HISTORY \
  -u TFORGE_FSR4_DISABLE_COLOR_HISTORY \
  -u TFORGE_FSR4_ENABLE_RECURRENT \
  -u TFORGE_FSR4_DISABLE_LEARNED_CONFIDENCE_GATE \
  -u TFORGE_FSR4_LEARNED_CONFIDENCE_BLEND \
  -u TFORGE_FSR4_CAS_STRENGTH \
  -u TFORGE_FSR4_DISABLE_CAS \
  -u TFORGE_FSR4_LEGACY_RCAS_STRENGTH \
  -u TFORGE_FSR4_USE_DISPLAY_BASE \
  -u TFORGE_FSR4_DISPLAY_BASE_STRENGTH \
  -u TFORGE_FSR4_CURRENT_BASE_FILTER \
  -u TFORGE_FSR4_CURRENT_BLEND_LINEAR \
  -u TFORGE_FSR4_CURRENT_BASE_JITTERED \
  -u TFORGE_FSR4_DISABLE_MOTION_VALIDITY \
  -u TFORGE_FSR4_DISABLE_PREPASS \
  -u TFORGE_FSR4_DISABLE_POSTPASS \
  TFORGE_TEMPORAL_WARMUP_FRAMES=36 \
  TFORGE_FSR4_JITTER_MODE=controlled \
  TFORGE_FSR4_CONTROLLED_JITTER=0.5 \
  TFORGE_FSR4_LEARNED_STRENGTH=0.55 \
  benchmarks/video_corpus/run_temporal_quality.sh \
  build-fast/temporal_forge_player \
  benchmarks/video_corpus/clips/tos_daylight_426x240_high_crf12.mp4 \
  benchmarks/video_corpus/references/tos_daylight_2160p_lossless.mkv \
  benchmarks/quality_sweeps/swarm/agent_learned055_jitter/captures/learned_055_control/jitter_050/tos_daylight_426x240.csv \
  24
```

For `current_default`, omit `TFORGE_FSR4_LEARNED_STRENGTH=0.55`. Repeat the
same command shape for every scene, resolution, jitter value, and arm. Do not
reuse CSVs, processes, sidecars, or output directories between arms.

## Strict paired gates

Compare `learned_055_control` only with `current_default` at the identical
jitter value, scene, and input resolution. Before comparison, require finite
metrics, expected dimensions, exactly 24 scored rows, GPU median timing, and
scored-path timing median and p95.

Every one of the 24 pairs must satisfy all of these gates:

- SSIM delta is at least `-0.0005`.
- Temporal-error relative increase is at most `2%`.
- Matched GPU median increase is at most `0.25 ms`.

After all per-pair gates pass, require at least three improved pairs, mean SSIM
delta at least `0.0`, and mean temporal-error delta at most `0.0`. A single
failed pair rejects the result. Aggregate-only approval and waived failed
pairs are forbidden. Cross-jitter comparisons are diagnostic only and cannot
replace the matched-jitter pairs.

## Human visual gate

For every pair, review stills and a scored-frame strip covering all 24 scored
frames. Check fine detail, perceived softness, ringing or halos, edge
overshoot, codec texture or noise, temporal flicker, ghosting, motion-detail
stability, and jitter phase or shimmer. Any visible regression in any pair is
promotion-blocking, even when machine gates pass.

## No promotion

This package defines a capture and review protocol only. Passing this matrix
does not authorize changing a default, source, shader, model, playback
behavior, or benchmark image. Promotion requires a separate decision with
fresh evidence and an explicit review record.

## Measured result

The six-case smoke rejected jitter `0.0` and `0.5` because the learned arm
lost more than the allowed SSIM floor. The matched jitter `1.0` arm was then
captured across all eight real scene/resolution cells. It passed the numeric
quality gates (`+0.000196375` mean SSIM, `-0.105450250` mean temporal error,
zero failed pairs), but remains only a candidate: timing and human visual
review are still outstanding. See `measured_results.json` and the raw CSVs
under `captures/`.

Matched timing on `tos_daylight 426x240` was effectively unchanged: GPU median
`1.323 ms` default versus `1.326 ms` at learned `.55`/jitter `1.0` (`+0.003
ms`, 20 samples each). Human visual review is the remaining gate.

The visual spot-check then blocked promotion: the learned/jitter-`1.0` still
was visibly softer in hair, beard, and facial texture than the matched current
default. No obvious ringing or shimmer was seen in that spot-check. The next
combination test will keep the numerically useful jitter and add a small
legacy-RCAS sweep.
