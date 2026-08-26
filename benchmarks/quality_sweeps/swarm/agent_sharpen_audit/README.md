# Next quality matrix: legacy current-path RCAS strength

Status: captured, not promoted. All 40 captures completed with the expected
dimensions and finite metrics. No reconstruction source or benchmark image was
changed by this audit.

## Why this is next

The direct-blend/detail-residual composition screen was early-rejected, and the
full-composition postpass CAS sweep was rejected. The next isolated control is
the legacy RCAS-style sharpen already used by the normal current composition.

This is not the Quality Lab `sharpen.mode=adaptive` control and it is not the
optional postpass CAS stage. It is the existing current-path operation named
`applyLegacyEdgeAwareRcas` in `shaders/fsr4/postpass_composite.comp`.

The host reads `TFORGE_FSR4_LEGACY_RCAS_STRENGTH`, clamps it to `[0, 1]`, and
writes it to `slot4.w`. The current branch passes that value to the legacy RCAS
function. When the variable is absent, the current path uses `0.08`, so the
`rcas_008` cell is the paired baseline. The Quality Lab stays disabled for all
cells so this matrix does not change composition, base filtering, tone, history,
jitter, or the separate CAS stage.

## Matrix

Five RCAS levels × four real scenes × two input resolutions = 40 captures.
Each capture uses 36 warmup frames and 24 scored frames.

| id | RCAS strength | meaning |
| --- | ---: | --- |
| `rcas_000` | 0.00 | no legacy RCAS response |
| `rcas_004` | 0.04 | restrained response below the current default |
| `rcas_008` | 0.08 | current-path default, paired baseline |
| `rcas_012` | 0.12 | moderate response above the current default |
| `rcas_016` | 0.16 | bounded upper screen, not a proposed default |

Use only these real corpus scenes:

```text
tos_daylight
tos_debris
sintel_rooftop
sintel_cave
```

Use the high-quality clips at `426x240` and `1280x720`. The expected output
dimensions are `1920x1080` and `3840x2160`, respectively. Record and verify
those dimensions instead of resampling the output for metrics.

## Capture command

Run from the repository root after building `build-fast/temporal_forge_player`.
The command below records the exact capture recipe used for this audit.

```bash
set -euo pipefail
player="$PWD/build-fast/temporal_forge_player"
root="$PWD/benchmarks/video_corpus"
out="$PWD/benchmarks/quality_sweeps/swarm/agent_sharpen_audit/captures"
mkdir -p "$out"

for scene in tos_daylight tos_debris sintel_rooftop sintel_cave; do
  reference="$root/references/${scene}_2160p_lossless.mkv"
  for resolution in 426x240 1280x720; do
    input="$root/clips/${scene}_${resolution}_high_crf12.mp4"
    for level in 000 004 008 012 016; do
      case "$level" in
        000) export TFORGE_FSR4_LEGACY_RCAS_STRENGTH=0.00 ;;
        004) export TFORGE_FSR4_LEGACY_RCAS_STRENGTH=0.04 ;;
        008) unset TFORGE_FSR4_LEGACY_RCAS_STRENGTH ;;
        012) export TFORGE_FSR4_LEGACY_RCAS_STRENGTH=0.12 ;;
        016) export TFORGE_FSR4_LEGACY_RCAS_STRENGTH=0.16 ;;
      esac
      result="$out/rcas_$level/${scene}_${resolution}.csv"
      mkdir -p "$(dirname "$result")"
      env -u TFORGE_QUALITY_LAB_CONFIG \
        -u TFORGE_FSR4_CAS_STRENGTH \
        -u TFORGE_FSR4_DISABLE_CAS \
        -u TFORGE_FSR4_ENABLE_COLOR_HISTORY \
        -u TFORGE_FSR4_DISABLE_COLOR_HISTORY \
        -u TFORGE_FSR4_ENABLE_RECURRENT \
        -u TFORGE_FSR4_LEARNED_STRENGTH \
        -u TFORGE_FSR4_LEARNED_CONFIDENCE_BLEND \
        -u TFORGE_FSR4_DISABLE_LEARNED_CONFIDENCE_GATE \
        -u TFORGE_FSR4_JITTER_MODE \
        -u TFORGE_FSR4_CONTROLLED_JITTER \
        -u TFORGE_FSR4_EXPERIMENTAL_REC709_INPUT_EOTF \
        -u TFORGE_FSR4_EXPERIMENTAL_UNKNOWN_MATRIX_BT709 \
        -u TFORGE_FSR4_EXPERIMENTAL_RECOVERED_LINEAR_OUTPUT \
        -u TFORGE_FSR4_CURRENT_BASE_FILTER \
        -u TFORGE_FSR4_CURRENT_BLEND_LINEAR \
        TFORGE_TEMPORAL_WARMUP_FRAMES=36 \
        benchmarks/video_corpus/run_temporal_quality.sh \
        "$player" "$input" "$reference" "$result" 24
    done
  done
done
```

The runner's own environment isolation remains authoritative. Use a fresh
output directory and do not reuse prior temporal records or sidecars.

## Decision gate

`rcas_008` is the paired baseline for each scene and input resolution. Check
every CSV row before calculating means.

A candidate is eligible only if all of these hold:

1. Every capture is complete, finite, and has the expected dimensions.
2. For all eight scene/resolution pairs, SSIM is no lower than baseline by more
   than `0.0005` and temporal error is no higher than baseline by `2%`.
3. At least three of the eight pairs improve in SSIM or temporal error without
   violating rule 2.
4. Mean SSIM is at least baseline and mean temporal error is no greater than
   baseline.
5. Matched GPU time does not increase by more than `0.25 ms`.

Do not promote on mean SSIM alone. A stronger RCAS response can make a still
look sharper while increasing ringing, codec noise, or temporal instability.

## Source and evidence anchors

- `src/render/Fsr4DispatchHarness.cpp`: reads and clamps
  `TFORGE_FSR4_LEGACY_RCAS_STRENGTH`, defaulting to `0.08`, and stores it in
  `slot4.w` for the current composition.
- `shaders/fsr4/postpass_composite.comp`: `applyLegacyEdgeAwareRcas` consumes
  the strength in the non-experimental current branch.
- `benchmarks/video_corpus/run_temporal_quality.sh`: forwards the environment
  control and isolates benchmark settings.
- `benchmarks/quality_sweeps/swarm/agent_next_audit/README.md`: establishes the
  matched eight-pair corpus, warmup/scored frame policy, and decision gate used
  by the preceding full-composition CAS audit.

No capture, benchmark image, shader, model, default quality behavior, or other
source file is changed by this matrix design.

## Measured decision

The paired baseline is `rcas_008` (the existing default). Aggregate results:

| level | mean SSIM | mean temporal error | gate failures | improved pairs |
| --- | ---: | ---: | ---: | ---: |
| `rcas_000` | 0.909595125 | 0.585892750 | 0 | 2 |
| `rcas_004` | 0.909592625 | 0.585796125 | 0 | 2 |
| `rcas_008` | 0.909590000 | 0.585701000 | 0 | — |
| `rcas_012` | 0.909587250 | 0.585606625 | 1 | 1 |
| `rcas_016` | 0.909584375 | 0.585516000 | 2 | 1 |

Nothing was promoted. The lower strengths slightly improve mean SSIM but
increase temporal error and do not improve the required three pairs. The higher
strengths reduce temporal error but lose SSIM or fail a paired safety gate.
The complete machine-readable result is in `measured_results.json`.
