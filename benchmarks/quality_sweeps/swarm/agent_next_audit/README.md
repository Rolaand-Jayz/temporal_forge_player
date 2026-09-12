# Next quality matrix: full-composition postpass CAS strength

Status: captured and not promoted. All 40 captures completed; results are in
`measured_results.json`.

## Why this is next

The current full-composition filter sweep is complete and did not produce a
promotable base-filter change. The next isolated, source-backed control is the
post-reconstruction CAS strength exposed as `TFORGE_FSR4_CAS_STRENGTH`.

This is distinct from the earlier Stage D Quality Lab `sharpen.mode=adaptive`
screen: that screen used `base_only` and measured the Quality Lab adaptive
operator. This matrix keeps the normal full composition and changes only the
postpass CAS control. The current source places CAS after either composition
branch and after each chained FSR pass.

The matrix deliberately does not revisit the already audited history, learned
blend, jitter, color-interpretation, or recovered-output experiments.

## Matrix

Five CAS levels × four real scenes × two input resolutions = 40 captures.
Each capture uses 36 warmup frames and 24 scored frames.

| id | CAS strength | meaning |
| --- | ---: | --- |
| `cas_000` | 0.00 | fixed baseline; no CAS environment override |
| `cas_002` | 0.02 | very restrained postpass CAS |
| `cas_004` | 0.04 | historical low-strength comparison point |
| `cas_008` | 0.08 | moderate postpass CAS |
| `cas_012` | 0.12 | upper bounded screen; not a proposed default |

Only `TFORGE_FSR4_CAS_STRENGTH` varies. Do not set
`TFORGE_FSR4_DISABLE_CAS`: the runner/player default keeps CAS enabled but
with strength zero. Do not set `TFORGE_QUALITY_LAB_CONFIG`; leaving it unset
keeps the normal current composition rather than a diagnostic composition.

Use these real corpus scenes only:

```text
tos_daylight
tos_debris
sintel_rooftop
sintel_cave
```

Use the high-quality clips at `426x240` and `1280x720`. The current graph is
expected to produce `1920x1080` from `426x240` and `3840x2160` from
`1280x720`; record and verify those dimensions rather than resampling them for
metrics.

## Capture command

Run from the repository root after building `build-fast/temporal_forge_player`.
The command is intentionally provided but not executed by this audit:

```bash
set -euo pipefail
player="$PWD/build-fast/temporal_forge_player"
root="$PWD/benchmarks/video_corpus"
out="$PWD/benchmarks/quality_sweeps/swarm/agent_next_audit/captures"
mkdir -p "$out"

for scene in tos_daylight tos_debris sintel_rooftop sintel_cave; do
  reference="$root/references/${scene}_2160p_lossless.mkv"
  for resolution in 426x240 1280x720; do
    input="$root/clips/${scene}_${resolution}_high_crf12.mp4"
    for strength in 000 002 004 008 012; do
      case "$strength" in
        000) unset TFORGE_FSR4_CAS_STRENGTH ;;
        002) export TFORGE_FSR4_CAS_STRENGTH=0.02 ;;
        004) export TFORGE_FSR4_CAS_STRENGTH=0.04 ;;
        008) export TFORGE_FSR4_CAS_STRENGTH=0.08 ;;
        012) export TFORGE_FSR4_CAS_STRENGTH=0.12 ;;
      esac
      result="$out/cas_$strength/${scene}_${resolution}.csv"
      mkdir -p "$(dirname "$result")"
      env -u TFORGE_QUALITY_LAB_CONFIG \
        -u TFORGE_FSR4_ENABLE_COLOR_HISTORY \
        -u TFORGE_FSR4_DISABLE_COLOR_HISTORY \
        -u TFORGE_FSR4_ENABLE_RECURRENT \
        -u TFORGE_FSR4_LEARNED_STRENGTH \
        -u TFORGE_FSR4_LEARNED_CONFIDENCE_BLEND \
        -u TFORGE_FSR4_JITTER_MODE \
        -u TFORGE_FSR4_CONTROLLED_JITTER \
        -u TFORGE_FSR4_EXPERIMENTAL_REC709_INPUT_EOTF \
        -u TFORGE_FSR4_EXPERIMENTAL_UNKNOWN_MATRIX_BT709 \
        -u TFORGE_FSR4_EXPERIMENTAL_RECOVERED_LINEAR_OUTPUT \
        -u TFORGE_FSR4_DISABLE_CAS \
        TFORGE_TEMPORAL_WARMUP_FRAMES=36 \
        benchmarks/video_corpus/run_temporal_quality.sh \
        "$player" "$input" "$reference" "$result" 24
    done
  done
done
```

For reproducibility, use fresh output paths. Do not reuse motion/event
sidecars or inherit `TFORGE_TEMPORAL_*` artifact variables from another run.
The runner's own `-u` list should remain authoritative for capture isolation.

## Decision gate

`cas_000` is the paired baseline for the same scene and input resolution.
Evaluate every CSV row, then aggregate only after the per-case checks.

A CAS level is eligible for promotion only if all of these hold:

1. Every capture is complete, finite, and has the expected output dimensions.
2. On every one of the eight scene/resolution pairs, SSIM is no lower than
   baseline by more than `0.0005` and temporal error is no higher than baseline
   by more than `2%`.
3. The candidate improves at least three of the eight pairs in either SSIM or
   temporal error without violating rule 2.
4. Its mean SSIM is at least the baseline mean and its mean temporal error is
   no greater than the baseline mean.
5. Its measured GPU time does not increase by more than `0.25 ms` on the
   matched capture path.

No level passed all five rules. CAS reduced the temporal-error metric at every
tested level but reduced SSIM at every level as well. Retain `cas_000` and do
not enable sharpening by default. Never promote on aggregate SSIM alone; CAS
can make a single frame look sharper while increasing ringing or compression
artifacts.

## Source and prior-evidence anchors

- `src/render/Fsr4DispatchHarness.cpp`: parses and clamps
  `TFORGE_FSR4_CAS_STRENGTH`, then writes the CAS strength and enable flag into
  the postpass constants.
- `shaders/fsr4/postpass_composite.comp`: applies CAS after both composition
  branches, including intermediate chained passes.
- `benchmarks/video_corpus/run_temporal_quality.sh`: forwards the CAS variable
  and isolates benchmark settings/environment.
- `benchmarks/quality_sweeps/swarm/agent_filter_full_audit/README.md`: the
  preceding full-composition base-filter sweep and its fixed matrix policy.
- `docs/archive/plans/QUALITY_RECONSTRUCTION_PLAN_20260822-20260902.md`: prior adaptive-sharpen
  `base_only` decision; it is not treated as evidence for or against this
  full-composition CAS matrix.

No reconstruction source, shader, model, benchmark image, or default quality
behavior was changed for this audit.
