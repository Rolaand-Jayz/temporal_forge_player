# Next isolated quality matrix: recurrent-state feedback

Status: **captured, not promoted**. All 16 captures completed with expected
dimensions and finite metrics. No reconstruction source or benchmark image was
changed by this audit.

This is the next isolated swarm slice after the rejected composition, history/
blend, base-filter, postpass-CAS, legacy-RCAS, and motion-coverage candidates.
It tests one remaining supported current-path control: recurrent-state
feedback, enabled only by `TFORGE_FSR4_ENABLE_RECURRENT`.

## Why this control is wired

`src/render/Fsr4DispatchHarness.cpp` sets the prepass recurrent-disable flag
unless `TFORGE_FSR4_ENABLE_RECURRENT` is present. The same flag is written into
the postpass control block, while recurrent read/write images are bound in the
dispatch path. `shaders/fsr4/prepass_pq_eotf.comp` samples the prior causal
recurrent state only when that flag allows it and publishes the new state for
the next frame. This is an existing opt-in path, not a proposed implementation.

The A/B leaves color history disabled in both cells. The source handles
recurrent and color-history policy as separate bits, so this isolates recurrent
feedback instead of combining it with the already rejected color/history
behavior. Learned strength, confidence, jitter, composition, filters, CAS/RCAS,
display base, motion-validity ablation, and pre/postpass bypasses are also held
at their ordinary unset defaults.

The source comments warn that recurrent state is not stable for codec-derived
video and that it has not improved measured video quality. That is a reason to
measure the existing opt-in path with strict pair gates, not to claim it works
or promote it.

## Capture matrix

`matrix.json` is authoritative: 2 candidates × 4 real scenes × 2 input
resolutions = 16 captures and 8 recurrent-on/off pairs. Use the existing
high-quality clips and 2160p lossless references:

| Candidate | Environment | Meaning |
| --- | --- | --- |
| `recurrent_off` | `TFORGE_FSR4_ENABLE_RECURRENT` unset | fixed baseline |
| `recurrent_on` | `TFORGE_FSR4_ENABLE_RECURRENT=1` | opt-in recurrent feedback |

Scenes are `tos_daylight`, `tos_debris`, `sintel_rooftop`, and `sintel_cave`.
Run each at `426x240` and `1280x720`; outputs are `1920x1080` and `3840x2160`.
Every capture uses 36 warmup frames and 24 scored frames.

## Exact isolated run

Run from the repository root with an already-built
`build-fast/temporal_forge_player`. The command removes every known
quality-affecting override and temporal sidecar before applying the one
candidate-specific variable. It writes fresh CSV paths and does not reuse
prior artifacts.

```bash
set -euo pipefail
player="$PWD/build-fast/temporal_forge_player"
corpus="$PWD/benchmarks/video_corpus"
out="$PWD/benchmarks/quality_sweeps/swarm/agent_next_recurrent/captures"
mkdir -p "$out"

for scene in tos_daylight tos_debris sintel_rooftop sintel_cave; do
  reference="$corpus/references/${scene}_2160p_lossless.mkv"
  for resolution in 426x240 1280x720; do
    input="$corpus/clips/${scene}_${resolution}_high_crf12.mp4"
    for candidate in recurrent_off recurrent_on; do
      case_env=()
      case "$candidate" in
        recurrent_off) ;;
        recurrent_on) case_env+=(TFORGE_FSR4_ENABLE_RECURRENT=1) ;;
      esac
      result="$out/$candidate/${scene}_${resolution}.csv"
      mkdir -p "$(dirname "$result")"
      env \
        -u TFORGE_QUALITY_LAB_CONFIG \
        -u TFORGE_FSR4_ENABLE_RECURRENT \
        -u TFORGE_FSR4_ENABLE_COLOR_HISTORY \
        -u TFORGE_FSR4_DISABLE_COLOR_HISTORY \
        -u TFORGE_FSR4_LEARNED_STRENGTH \
        -u TFORGE_FSR4_LEARNED_CONFIDENCE_BLEND \
        -u TFORGE_FSR4_DISABLE_LEARNED_CONFIDENCE_GATE \
        -u TFORGE_FSR4_CAS_STRENGTH \
        -u TFORGE_FSR4_LEGACY_RCAS_STRENGTH \
        -u TFORGE_FSR4_USE_DISPLAY_BASE \
        -u TFORGE_FSR4_DISPLAY_BASE_STRENGTH \
        -u TFORGE_FSR4_CURRENT_BASE_FILTER \
        -u TFORGE_FSR4_CURRENT_BLEND_LINEAR \
        -u TFORGE_FSR4_CURRENT_BASE_JITTERED \
        -u TFORGE_FSR4_EXPERIMENTAL_BASE_UNJITTERED \
        -u TFORGE_FSR4_JITTER_MODE \
        -u TFORGE_FSR4_CONTROLLED_JITTER \
        -u TFORGE_FSR4_DISABLE_MOTION_VALIDITY \
        -u TFORGE_FSR4_DISABLE_PREPASS \
        -u TFORGE_FSR4_DISABLE_POSTPASS \
        -u TFORGE_TEMPORAL_MOTION_JSON \
        -u TFORGE_TEMPORAL_METRICS_CSV \
        -u TFORGE_TEMPORAL_EVENTS_JSON \
        -u TFORGE_TEMPORAL_STATIC_MASK_JSON \
        -u TFORGE_TEMPORAL_CLASS \
        TFORGE_TEMPORAL_WARMUP_FRAMES=36 "${case_env[@]}" \
        benchmarks/video_corpus/run_temporal_quality.sh \
        "$player" "$input" "$reference" "$result" 24
    done
  done
done
```

The runner's normal forwarded environment must not be used to add other
controls. If a capture fails, preserve the failure log and use a new output
directory for a retry; do not fill a missing CSV with a synthetic result.

## Pair gates and no-promotion policy

Compare `recurrent_on` with `recurrent_off` for the same scene and input
resolution. All eight pairs must have finite metrics, SSIM delta at least
`-0.0005`, temporal-error relative increase no greater than `2%`, and median
GPU-time increase no greater than `0.25 ms` when GPU timing is present. The
candidate must improve at least three pairs, have aggregate mean SSIM delta at
least zero, and have aggregate mean temporal-error delta at most zero. These
are conjunctive gates; aggregate SSIM cannot waive a pair regression.

Even a passing capture is evidence for review only. Do not edit source,
shaders, model/playback code, defaults, or user-facing settings, and do not
call recurrent feedback a supported default from this matrix. Promotion would
require a separate reviewed decision with fresh CSVs, a per-pair report,
runtime-stability evidence, and an explicit policy choice.

## Evidence inspected

- `src/render/Fsr4DispatchHarness.cpp` and `shaders/fsr4/prepass_pq_eotf.comp`:
  recurrent flag, bindings, causal read, and state publication.
- `benchmarks/video_corpus/run_temporal_quality.sh`: accepted environment
  forwarding and 24-frame temporal capture interface.
- `swarm/agent_inventory/manifest.md`: current control inventory and ranges.
- `swarm/agent_config_audit/README.md` and `measured_results.json`: history /
  blend capture was not promoted.
- `swarm/agent_next_audit/README.md`, `agent_sharpen_audit/README.md`, and
  `agent_composition_audit/README.md`: already audited or rejected controls.

## Measured decision

The recurrent-on output was numerically identical to recurrent-off at CSV
precision for SSIM on all eight pairs. Mean temporal error changed by only
`-0.000000125`, with seven pairs unchanged and one pair changing by `-0.000005`.
That is not a meaningful or established quality gain, so the opt-in path was
not promoted and defaults were left alone. The machine-readable result is in
`measured_results.json`.

No shader, model, playback, or benchmark implementation was edited by this
slice.
