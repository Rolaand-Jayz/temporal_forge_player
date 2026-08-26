# Color-history confidence blend 0.75 matrix

Design-only package. No GPU captures are authorized or run. This package does
not change source, shaders, models, defaults, playback behavior, or benchmark
images. `matrix.json` is authoritative.

## Scope

The requested candidate is tested against the product default and against a
matched fixed-history control:

| id | color history | learned strength | confidence blend |
| --- | --- | ---: | ---: |
| `current_default` | unset, product default | unset, product default | unset, product default |
| `fixed_history_control` | `1` | `0.15` | `0` |
| `history_confidence015` | `1` | `0.15` | `0.75` |

The fixed-history arm is appropriate because it holds color history and
learned strength constant while isolating the confidence-blend change. The
candidate is compared with both controls at the identical scene and input
resolution. The control-to-control comparison is diagnostic only.

Use the four real scenes `tos_daylight`, `tos_debris`, `sintel_rooftop`, and
`sintel_cave`, with `426x240` and `1280x720` inputs. Expected output dimensions
are `1920x1080` and `3840x2160`, respectively. Each of the 24 cells runs 36
warmup frames followed by 24 scored frames.

## Exact isolation and capture command

Run every cell in a fresh player process, with a fresh CSV/output namespace.
Unset all inherited quality, sidecar, and diagnostic variables listed in
`matrix.json`. Set only `TFORGE_TEMPORAL_WARMUP_FRAMES=36` plus the selected
candidate's declared variables. Pass `24` as the temporal runner's scored
frame argument.

The command shape is:

```bash
set -euo pipefail
player="$PWD/build-fast/temporal_forge_player"
root="$PWD/benchmarks/video_corpus"
plan="$PWD/benchmarks/quality_sweeps/swarm/agent_history_confidence015"
out="$plan/captures"
mkdir -p "$out"

for scene in tos_daylight tos_debris sintel_rooftop sintel_cave; do
  reference="$root/references/${scene}_2160p_lossless.mkv"
  for resolution in 426x240 1280x720; do
    input="$root/clips/${scene}_${resolution}_high_crf12.mp4"
    for candidate in current_default fixed_history_control history_confidence015; do
      case "$candidate" in
        current_default) env_args=() ;;
        fixed_history_control)
          env_args=(
            TFORGE_FSR4_ENABLE_COLOR_HISTORY=1
            TFORGE_FSR4_LEARNED_STRENGTH=0.15
            TFORGE_FSR4_LEARNED_CONFIDENCE_BLEND=0
          ) ;;
        history_confidence015)
          env_args=(
            TFORGE_FSR4_ENABLE_COLOR_HISTORY=1
            TFORGE_FSR4_LEARNED_CONFIDENCE_BLEND=0.75
            TFORGE_FSR4_LEARNED_STRENGTH=0.15
          ) ;;
      esac
      result="$out/$candidate/${scene}_${resolution}.csv"
      mkdir -p "$(dirname "$result")"
      env \
        -u TFORGE_QUALITY_LAB_CONFIG \
        -u TFORGE_FSR4_ENABLE_COLOR_HISTORY \
        -u TFORGE_FSR4_DISABLE_COLOR_HISTORY \
        -u TFORGE_FSR4_ENABLE_RECURRENT \
        -u TFORGE_FSR4_DISABLE_LEARNED_CONFIDENCE_GATE \
        -u TFORGE_FSR4_LEARNED_CONFIDENCE_BLEND \
        -u TFORGE_FSR4_LEARNED_STRENGTH \
        -u TFORGE_FSR4_CAS_STRENGTH \
        -u TFORGE_FSR4_DISABLE_CAS \
        -u TFORGE_FSR4_LEGACY_RCAS_STRENGTH \
        -u TFORGE_FSR4_USE_DISPLAY_BASE \
        -u TFORGE_FSR4_DISPLAY_BASE_STRENGTH \
        -u TFORGE_FSR4_CURRENT_BASE_FILTER \
        -u TFORGE_FSR4_CURRENT_BLEND_LINEAR \
        -u TFORGE_FSR4_CURRENT_BASE_JITTERED \
        -u TFORGE_FSR4_EXPERIMENTAL_BASE_UNJITTERED \
        -u TFORGE_FSR4_DISABLE_PREPASS \
        -u TFORGE_FSR4_EXPERIMENTAL_DISABLE_POSTPASS_TAIL \
        -u TFORGE_FSR4_DISABLE_MOTION_VALIDITY \
        -u TFORGE_FSR4_JITTER_MODE \
        -u TFORGE_FSR4_CONTROLLED_JITTER \
        -u TFORGE_TEMPORAL_MOTION_JSON \
        -u TFORGE_TEMPORAL_METRICS_CSV \
        -u TFORGE_TEMPORAL_EVENTS_JSON \
        -u TFORGE_TEMPORAL_STATIC_MASK_JSON \
        -u TFORGE_TEMPORAL_CLASS \
        TFORGE_TEMPORAL_WARMUP_FRAMES=36 \
        "${env_args[@]}" \
        benchmarks/video_corpus/run_temporal_quality.sh \
        "$player" "$input" "$reference" "$result" 24
    done
  done
done
```

The `-u` operations happen before candidate assignments, so an inherited
candidate value cannot leak into another arm. Do not reuse a player process,
CSV, sidecar, or output directory. Leave temporal motion, event, static-mask,
and enhanced-metrics sidecars unset for this base matrix. If those metrics are
later added, they require a separately versioned design and fresh namespaces.

## Paired timing

Timing is a required gate, not an inference from CSV completion time. Run the
existing `benchmarks/video_corpus/run_performance.sh` timing path with the same
candidate environment isolation and the same real clip selector. Record
stage-separated `gpu_p50_ms`, `pipeline_p50_ms`, and `pipeline_p95_ms` for the
matched candidate/control cells. Missing or non-finite timing fails the cell.

The timing run must use a fresh process and must not change the quality tuple.
Do not substitute host CPU load, wall-clock shell duration, or a different
scene/resolution as timing evidence.

## Numeric gates

Before comparison, require finite SSIM and temporal metrics, expected output
dimensions, exactly 24 scored rows, and all required timing fields.

For all 16 candidate/current pairs and all 16 candidate/fixed-history pairs:

- SSIM delta must be at least `-0.0005`.
- Temporal-error relative increase must be at most `2%`.
- GPU median increase must be at most `0.25 ms`.
- Scored-path pipeline median increase must be at most `0.25 ms`.
- Scored-path pipeline p95 increase must be at most `0.50 ms`.

After every pair passes, the candidate must improve SSIM or temporal error in
at least 3 of 16 pairs against `current_default`, have mean SSIM delta at
least `0`, and have mean temporal-error delta at most `0` against
`current_default`. The same mean non-regression requirements apply against
`fixed_history_control`. A failed pair cannot be waived, and aggregate-only
approval is forbidden.

## Human visual gate

Review real scored-frame still pairs and 24-frame scored strips for every
candidate/control pair. Check fine detail and softness, ringing or halos, edge
stability, temporal flicker, ghosting/trails, disocclusion recovery,
motion-detail stability, and color-history smear or lag. Any visible candidate
regression in any scene or resolution blocks promotion, even if the numeric
gates pass.

## No promotion

This directory defines a measurement and review protocol only. A passing
matrix does not authorize changing the default environment, source, shaders,
models, playback behavior, or benchmark images. Promotion requires a separate
decision record with fresh paired metrics, matched timing, and completed human
review. Until then, the candidate remains unpromoted.

## Measured result

The eight real-corpus candidate captures completed. Aggregate SSIM improved by
`+0.000404625` and aggregate temporal error by `-0.026618750`, but three cells
exceeded the paired temporal gate: both Sintel Cave resolutions and ToS Debris
426x240. The candidate is not promoted. See `measured_results.json` and
`captures/`.
