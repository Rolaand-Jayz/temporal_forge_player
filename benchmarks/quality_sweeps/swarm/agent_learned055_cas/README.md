# Learned-strength 0.55 plus mild CAS quality matrix

Status: **captured, not promoted**. All 40 captures completed with expected
dimensions and finite metrics. No source, default, or benchmark image changed.

This is the next paired current-path quality sweep for the learned-strength
0.55 candidate that improved temporal metrics but looked slightly soft in
still frames. It tests only the interaction between the explicit learned
strength and the optional postpass CAS strength. No Quality Lab config is
used, so this remains the normal current composition path.

No captures, source changes, shader changes, benchmark-image changes, or
default changes are part of this package. `matrix.json` is the authoritative
candidate list and declares the exact environment for each case.

## Candidates

| id | learned override | CAS override | role |
| --- | ---: | ---: | --- |
| `current_default` | unset | unset | paired baseline, current defaults |
| `learned_055_cas_000` | `0.55` | `0.00` | learned-effect control |
| `learned_055_cas_002` | `0.55` | `0.02` | mild CAS candidate |
| `learned_055_cas_004` | `0.55` | `0.04` | historical mild CAS point |
| `learned_055_cas_008` | `0.55` | `0.08` | upper mild-range candidate |

The baseline deliberately leaves both variables unset. This preserves the
current path's resolution-dependent learned default and the current CAS
default. `learned_055_cas_000` is a required paired control: it isolates the
learned-strength change from the CAS change. For all candidate cases, the
only declared controls are `TFORGE_FSR4_LEARNED_STRENGTH` and, where shown,
`TFORGE_FSR4_CAS_STRENGTH`.

The source wiring clamps both controls to `[0,1]`; the tested CAS values are
inside the supported range and intentionally stop at `0.08` as a mild sweep.
Do not add `TFORGE_FSR4_DISABLE_CAS`, because CAS is enabled with zero
strength by the current path and disabling it would test a different control.

## Corpus and capture policy

Use these real scenes and no synthetic substitutes:

```text
tos_daylight
tos_debris
sintel_rooftop
sintel_cave
```

For every candidate, run both high-quality inputs:

```text
426x240   -> 1920x1080
1280x720  -> 3840x2160
```

Use 36 warmup frames followed by 24 scored frames. The complete matrix is
`5 candidates × 4 scenes × 2 resolutions = 40 captures`. Use fresh result
paths and leave all temporal sidecar variables unset so the runner creates a
clean ordinary metric run.

Capture preparation is intentionally documented but must not be run while
creating this package:

```bash
set -euo pipefail
player="$PWD/build-fast/temporal_forge_player"
root="$PWD/benchmarks/video_corpus"
plan="$PWD/benchmarks/quality_sweeps/swarm/agent_learned055_cas"
out="$plan/captures"
mkdir -p "$out"

for scene in tos_daylight tos_debris sintel_rooftop sintel_cave; do
  reference="$root/references/${scene}_2160p_lossless.mkv"
  for resolution in 426x240 1280x720; do
    input="$root/clips/${scene}_${resolution}_high_crf12.mp4"
    for candidate in current_default learned_055_cas_000 learned_055_cas_002 \
                    learned_055_cas_004 learned_055_cas_008; do
      env_args=(TFORGE_TEMPORAL_WARMUP_FRAMES=36)
      case "$candidate" in
        current_default) ;;
        learned_055_cas_000)
          env_args+=(TFORGE_FSR4_LEARNED_STRENGTH=0.55 TFORGE_FSR4_CAS_STRENGTH=0.00) ;;
        learned_055_cas_002)
          env_args+=(TFORGE_FSR4_LEARNED_STRENGTH=0.55 TFORGE_FSR4_CAS_STRENGTH=0.02) ;;
        learned_055_cas_004)
          env_args+=(TFORGE_FSR4_LEARNED_STRENGTH=0.55 TFORGE_FSR4_CAS_STRENGTH=0.04) ;;
        learned_055_cas_008)
          env_args+=(TFORGE_FSR4_LEARNED_STRENGTH=0.55 TFORGE_FSR4_CAS_STRENGTH=0.08) ;;
      esac
      result="$out/$candidate/${scene}_${resolution}.csv"
      mkdir -p "$(dirname "$result")"
      env -u TFORGE_TEMPORAL_MOTION_JSON \
        -u TFORGE_TEMPORAL_METRICS_CSV \
        -u TFORGE_TEMPORAL_EVENTS_JSON \
        -u TFORGE_TEMPORAL_STATIC_MASK_JSON \
        -u TFORGE_TEMPORAL_CLASS \
        -u TFORGE_QUALITY_LAB_CONFIG \
        -u TFORGE_FSR4_ENABLE_COLOR_HISTORY \
        -u TFORGE_FSR4_DISABLE_COLOR_HISTORY \
        -u TFORGE_FSR4_ENABLE_RECURRENT \
        -u TFORGE_FSR4_LEARNED_CONFIDENCE_BLEND \
        -u TFORGE_FSR4_DISABLE_LEARNED_CONFIDENCE_GATE \
        -u TFORGE_FSR4_DISABLE_CAS \
        -u TFORGE_FSR4_DISABLE_MOTION_VALIDITY \
        -u TFORGE_FSR4_JITTER_MODE \
        -u TFORGE_FSR4_CONTROLLED_JITTER \
        "${env_args[@]}" \
        benchmarks/video_corpus/run_temporal_quality.sh \
        "$player" "$input" "$reference" "$result" 24
    done
  done
done
```

## Measured decision

`learned_055_cas_000` reproduces the learned-only candidate and passes all
metric gates. Every nonzero CAS level introduces two paired gate failures:

| candidate | mean SSIM delta | mean temporal-error delta | gate failures | improved pairs |
| --- | ---: | ---: | ---: | ---: |
| `learned_055_cas_000` | +0.000196875 | -0.105430250 | 0 | 5 |
| `learned_055_cas_002` | +0.000174250 | -0.106879875 | 2 | 1 |
| `learned_055_cas_004` | +0.000151875 | -0.108592500 | 2 | 1 |
| `learned_055_cas_008` | +0.000102375 | -0.112177500 | 2 | 1 |

No CAS combination was promoted. The full machine-readable result is in
`measured_results.json`.

## Strict paired decision gate

Compare every candidate to `current_default` on the same scene and input
resolution. A candidate is eligible only if every one of its eight pairs is
complete, finite, and has the expected output dimensions, and all of these
per-pair limits hold:

1. SSIM delta is at least `-0.0005`.
2. Temporal-error relative increase is no more than `2%`.
3. GPU time increase is no more than `0.25 ms` on the matched path.

After those per-pair checks, require at least three of eight pairs to improve
SSIM or temporal error, mean SSIM to be no lower than baseline, and mean
temporal error to be no higher than baseline. Aggregate results never waive a
failed pair. The `learned_055_cas_000` control must be reported separately so
any CAS benefit is not credited for the learned-strength effect itself.

## Human visual review gate

Metrics are necessary but not sufficient. Before promotion, a human reviewer
must inspect all eight candidate-versus-baseline still pairs for each eligible
CAS level, plus short scored-frame strips from every scene/resolution pair.
Review must explicitly record pass/fail for fine detail and perceived
softness, ringing or halos, edge overshoot, compression texture, temporal
flicker, ghosting, and motion-detail stability. Any visible regression in a
pair fails promotion, even if its numeric gates pass. The review must include
`learned_055_cas_000` versus `current_default` and each CAS level versus that
paired learned control.

## Evidence anchors

- `src/render/Fsr4DispatchHarness.cpp` — learned and CAS environment parsing,
  clamping, and postpass constant wiring.
- `shaders/fsr4/postpass_composite.comp` — CAS placement after current-path
  reconstruction.
- `benchmarks/video_corpus/run_temporal_quality.sh` — capture isolation,
  environment forwarding, warmup, and scored-frame handling.
- `benchmarks/quality_sweeps/swarm/agent_next_audit/README.md` — prior
  current-path CAS-only sweep and paired gate conventions.
