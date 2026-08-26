# Matched full-temporal composition audit

This was a capture-ready plan for the next isolated Quality Lab
matrix. It follows the rejected full-composition CAS sweep and tests only
composition/residual choices that have not yet been compared across the same
real corpus, two input resolutions, and a warm temporal sequence.

Seven smoke captures were run. Every experimental candidate failed the
mandatory per-cell safety gate against the current control on the same real
scene/resolution pair, so the remaining 49 captures were intentionally not
run. No reconstruction source, shader, model, benchmark image, or default
quality behavior was changed.

## What is being tested

All candidates use the same neutral settings:

- Quality Lab enabled
- Catmull-Rom base (`b=0`, `c=0.5`)
- box3x3 residual low-pass
- no Quality Lab adaptive sharpening
- neutral tone
- bicubic presentation filter
- current jitter
- sparse motion-validity path enabled
- postpass CAS explicitly fixed at `0.0`

Only the composition mode and its applicable strength vary:

| id | mode | learned strength | residual strength |
| --- | --- | ---: | ---: |
| `current_control` | `current` | 0.55 | 1.00 |
| `direct_blend_005` | `direct_blend` | 0.05 | 0.00 |
| `direct_blend_010` | `direct_blend` | 0.10 | 0.00 |
| `direct_blend_015` | `direct_blend` | 0.15 | 0.00 |
| `detail_residual_005` | `detail_residual` | 0.00 | 0.05 |
| `detail_residual_010` | `detail_residual` | 0.00 | 0.10 |
| `detail_residual_025` | `detail_residual` | 0.00 | 0.25 |

The current control matches the composition settings used by the preceding
full-composition filter audit. Direct blend and detail residual are evaluated
as separate modes; they are not silently stacked with one another.

## Corpus and frame policy

Use only these real-world scenes:

```text
tos_daylight
tos_debris
sintel_rooftop
sintel_cave
```

For each candidate, capture both `426x240` and `1280x720` high-quality inputs.
The expected output dimensions are `1920x1080` and `3840x2160`, respectively.
Use 36 warmup frames followed by 24 scored frames. The matrix therefore has
7 candidates × 4 scenes × 2 resolutions = 56 captures.

`matrix.json` is the authoritative candidate list and points to the exact
Quality Lab JSON passed to the player. The measured early rejection is in
`measured_results.json`.

## Capture command

Run from the repository root after building `build-fast/temporal_forge_player`.
This command is intentionally not executed by this audit:

```bash
set -euo pipefail
player="$PWD/build-fast/temporal_forge_player"
root="$PWD/benchmarks/video_corpus"
plan="$PWD/benchmarks/quality_sweeps/swarm/agent_composition_audit"
out="$plan/captures"
mkdir -p "$out"

for scene in tos_daylight tos_debris sintel_rooftop sintel_cave; do
  reference="$root/references/${scene}_2160p_lossless.mkv"
  for resolution in 426x240 1280x720; do
    input="$root/clips/${scene}_${resolution}_high_crf12.mp4"
    for candidate in current_control direct_blend_005 direct_blend_010 direct_blend_015 \
                    detail_residual_005 detail_residual_010 detail_residual_025; do
      config="$plan/${candidate}.json"
      quality_config="$(tr -d '\n' < "$config")"
      result="$out/$candidate/${scene}_${resolution}.csv"
      mkdir -p "$(dirname "$result")"
      env \
        -u TFORGE_TEMPORAL_MOTION_JSON \
        -u TFORGE_TEMPORAL_METRICS_CSV \
        -u TFORGE_TEMPORAL_EVENTS_JSON \
        -u TFORGE_TEMPORAL_STATIC_MASK_JSON \
        -u TFORGE_TEMPORAL_CLASS \
        -u TFORGE_FSR4_ENABLE_COLOR_HISTORY \
        -u TFORGE_FSR4_DISABLE_COLOR_HISTORY \
        -u TFORGE_FSR4_ENABLE_RECURRENT \
        -u TFORGE_FSR4_LEARNED_STRENGTH \
        -u TFORGE_FSR4_LEARNED_CONFIDENCE_BLEND \
        -u TFORGE_FSR4_JITTER_MODE \
        -u TFORGE_FSR4_CONTROLLED_JITTER \
        -u TFORGE_FSR4_CAS_STRENGTH \
        -u TFORGE_FSR4_DISABLE_CAS \
        -u TFORGE_FSR4_DISABLE_MOTION_VALIDITY \
        TFORGE_TEMPORAL_WARMUP_FRAMES=36 \
        TFORGE_FSR4_CAS_STRENGTH=0 \
        TFORGE_QUALITY_LAB_CONFIG="$quality_config" \
        benchmarks/video_corpus/run_temporal_quality.sh \
        "$player" "$input" "$reference" "$result" 24
    done
  done
done
```

Use a fresh output directory for every run. The temporal runner rejects
pre-existing sidecars and the command deliberately leaves enhanced sidecar
exports unset.

## Decision gate

`current_control` is the paired baseline for the same scene and input
resolution. A candidate is eligible for promotion only when all of these are
true:

1. All eight paired captures are complete, finite, and have the expected
   output dimensions.
2. No pair loses more than `0.0005` SSIM against its baseline.
3. No pair's temporal error increases by more than `2%` against its baseline.
4. At least three of eight pairs improve either SSIM or temporal error.
5. Mean SSIM is not lower than baseline and mean temporal error is not higher.
6. GPU time does not increase by more than `0.25 ms` on the matched path.

Do not promote an aggregate-only winner. Detail residual must also be checked
for ringing, halos, staircase return, and temporal texture instability in the
review artifacts.

## Why this matrix is not a duplicate

Earlier direct-blend and residual artifacts were single-frame screens,
small/one-resolution runs, or mixed with other controls. This plan is the
first clean comparison in this campaign where `current`, `direct_blend`, and
`detail_residual` share the exact same real scenes, both tested input sizes,
warmup, scored-frame count, neutral postpass settings, and baseline gate.

## Evidence anchors

- `benchmarks/quality_sweeps/swarm/agent_filter_full_audit/README.md` — prior
  full-composition matrix and output-dimension policy.
- `benchmarks/quality_sweeps/swarm/agent_next_audit/README.md` — rejected CAS
  sweep and isolation rules.
- `docs/exec-plans/QUALITY_RECONSTRUCTION_PLAN.md` — definitions and prior
  single-frame evidence for the composition modes.
- `src/config/QualityLabConfig.cpp` — accepted Quality Lab schema.
- `benchmarks/video_corpus/run_temporal_quality.sh` — temporal capture runner.
