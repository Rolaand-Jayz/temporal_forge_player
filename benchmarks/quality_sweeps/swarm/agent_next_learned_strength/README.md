# Current-path learned-strength sweep

Captured candidate for review. This package changes no source, shader, model,
default, or playback behavior. It isolates the existing current-composition
learned-strength control for a paired quality campaign.

## Verified wiring

`src/render/Fsr4DispatchHarness.cpp` reads
`TFORGE_FSR4_LEARNED_STRENGTH` once per process, clamps it to `[0, 1]`, and
overrides the current-path resolution-aware learned strength before writing
`pp.slot1[3] = learnedStrength * effectiveConfidence`.

`shaders/fsr4/postpass_composite.comp` consumes that value as
`slot1.w` in the preserved current branch, where it blends `currentBaseColor`
with `learnedColor`. The Quality Lab is disabled, so its separate
`slot5.x` learned-strength field is not involved. Confidence gating remains at
its normal setting and is not part of this sweep.

The paired baseline is `current_default`, with the override unset. The host
default is resolution-aware: with the requested output mapping it is about
`0.05` for `426x240` and `0.55` for `1280x720`. Every non-baseline value is an
explicit override in a fresh player process.

## Matrix and capture policy

Seven levels × four real scenes × two input resolutions = 56 captures. Use only:

```text
tos_daylight
tos_debris
sintel_rooftop
sintel_cave
```

Use high-quality `crf12` inputs at `426x240` and `1280x720`. Expected outputs
are `1920x1080` and `3840x2160`, respectively. Run 36 warmup frames followed
by 24 scored frames. `matrix.json` is authoritative.

## Exact capture command

Run from the repository root after building `build-fast/temporal_forge_player`.
Use a fresh output directory. Do not add temporal motion, event, static-mask,
or class sidecars.

```bash
set -euo pipefail
player="$PWD/build-fast/temporal_forge_player"
root="$PWD/benchmarks/video_corpus"
plan="$PWD/benchmarks/quality_sweeps/swarm/agent_next_learned_strength"
out="$plan/captures"
mkdir -p "$out"

for scene in tos_daylight tos_debris sintel_rooftop sintel_cave; do
  reference="$root/references/${scene}_2160p_lossless.mkv"
  for resolution in 426x240 1280x720; do
    input="$root/clips/${scene}_${resolution}_high_crf12.mp4"
    for level in current_default learned_000 learned_005 learned_015 learned_035 learned_055 learned_075; do
      result="$out/$level/${scene}_${resolution}.csv"
      mkdir -p "$(dirname "$result")"
      case "$level" in
        current_default) strength_args=() ;;
        learned_000) strength_args=(TFORGE_FSR4_LEARNED_STRENGTH=0.00) ;;
        learned_005) strength_args=(TFORGE_FSR4_LEARNED_STRENGTH=0.05) ;;
        learned_015) strength_args=(TFORGE_FSR4_LEARNED_STRENGTH=0.15) ;;
        learned_035) strength_args=(TFORGE_FSR4_LEARNED_STRENGTH=0.35) ;;
        learned_055) strength_args=(TFORGE_FSR4_LEARNED_STRENGTH=0.55) ;;
        learned_075) strength_args=(TFORGE_FSR4_LEARNED_STRENGTH=0.75) ;;
      esac
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
        -u TFORGE_FSR4_LEARNED_STRENGTH \
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
        -u TFORGE_FSR4_JITTER_MODE \
        -u TFORGE_FSR4_CONTROLLED_JITTER \
        -u TFORGE_FSR4_DISABLE_MOTION_VALIDITY \
        -u TFORGE_FSR4_DISABLE_PREPASS \
        -u TFORGE_FSR4_DISABLE_POSTPASS \
        TFORGE_TEMPORAL_WARMUP_FRAMES=36 \
        "${strength_args[@]}" \
        benchmarks/video_corpus/run_temporal_quality.sh \
        "$player" "$input" "$reference" "$result" 24
    done
  done
done
```

## Measured decision

All 56 captures completed with expected dimensions and finite metrics. The only
override that passed the paired SSIM/temporal gates for all eight scene and
resolution pairs was `learned_055`:

| level | mean SSIM delta | mean temporal-error delta | gate failures | improved pairs |
| --- | ---: | ---: | ---: | ---: |
| `learned_000` | -0.000099625 | +0.003766125 | 4 | 0 |
| `learned_005` | +0.000015875 | +0.004918750 | 4 | 2 |
| `learned_015` | +0.000194750 | -0.002848125 | 4 | 3 |
| `learned_035` | +0.000338375 | -0.044385375 | 2 | 3 |
| `learned_055` | +0.000196500 | -0.105429750 | 0 | 5 |
| `learned_075` | -0.000228875 | -0.169155375 | 6 | 1 |

`learned_055` is a review candidate, not a promoted setting. The CSV runner
does not provide the matched GPU-time evidence required by the matrix gate, and
metrics cannot rule out ringing, texture loss, or other human-visible changes.
The full machine-readable result is in `measured_results.json`.

## Strict paired gate and no promotion

Compare each experimental row only with `current_default` at the identical
scene and input resolution. A candidate is eligible for review only if all
eight pairs have finite metrics and expected dimensions, every pair has SSIM
delta at least `-0.0005`, every pair has temporal-error relative increase at
most `2%`, at least three pairs improve SSIM or temporal error, mean SSIM is
not lower, mean temporal error is not higher, and matched GPU median time does
not increase by more than `0.25 ms`.

No aggregate-only decision is allowed. Any per-pair failure rejects that
candidate, even if an aggregate improves. These captures are evidence only:
do not promote a value, change defaults, or edit source, shaders, models, or
playback behavior from this matrix alone. Review ringing, halos, texture
stability, and scene-specific regressions before any separate promotion work.

## Evidence anchors

- `src/render/Fsr4DispatchHarness.cpp`, learned-strength override and
  `pp.slot1[3]` producer.
- `shaders/fsr4/postpass_composite.comp`, `slot1.w` current-branch consumer.
- `benchmarks/video_corpus/run_temporal_quality.sh`, explicit environment
  allowlist and warmup/scored-frame handling.
- `benchmarks/quality_sweeps/swarm/agent_confidence_blend/README.md` and
  `agent_next_combo/README.md`, prior confidence/strength evidence that does
  not replace this isolated current-path matrix.
