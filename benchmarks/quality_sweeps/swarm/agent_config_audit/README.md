# Safe next configuration audit

Inspected and captured 2026-08-24. This is a bounded follow-up after sparse
codec-motion validity. No source, shader, media, or default quality behavior
was changed by this matrix.

## Matrix

| id | color history | learned strength | confidence blend |
| --- | --- | ---: | ---: |
| `coverage_control` | off | runtime default | 0 |
| `history_control` | on | runtime default | 0 |
| `history_blend075_s015` | on | 0.15 | 0.75 |
| `history_blend100_s015` | on | 0.15 | 1.00 |

These are all existing controls. Do not set
`TFORGE_FSR4_DISABLE_MOTION_VALIDITY`; the new sparse-validity path stays on.
Use the four real scenes `tos_daylight`, `tos_debris`, `sintel_rooftop`, and
`sintel_cave`, at `426x240` and `1280x720`, with 36 warmup and 24 scored
frames: 32 captures total.

Do not add jitter, CAS, recurrent, display-base, filter, tone, residual,
multi-pass, or confidence-gate-bypass controls to this test. They would
confound the history/blend question.

## Exact command

Run from the repository root after building `build-fast/temporal_forge_player`:

```bash
set -euo pipefail
player="$PWD/build-fast/temporal_forge_player"
root="$PWD/benchmarks/video_corpus"
out="$PWD/benchmarks/quality_sweeps/swarm/agent_config_audit/captures"
mkdir -p "$out"
for scene in tos_daylight tos_debris sintel_rooftop sintel_cave; do
  reference="$root/references/${scene}_2160p_lossless.mkv"
  for resolution in 426x240 1280x720; do
    input="$root/clips/${scene}_${resolution}_high_crf12.mp4"
    for case in coverage_control history_control history_blend075_s015 history_blend100_s015; do
      env_args=(TFORGE_TEMPORAL_WARMUP_FRAMES=36)
      case "$case" in
        coverage_control) ;;
        history_control) env_args+=(TFORGE_FSR4_ENABLE_COLOR_HISTORY=1) ;;
        history_blend075_s015) env_args+=(TFORGE_FSR4_ENABLE_COLOR_HISTORY=1 TFORGE_FSR4_LEARNED_STRENGTH=0.15 TFORGE_FSR4_LEARNED_CONFIDENCE_BLEND=0.75) ;;
        history_blend100_s015) env_args+=(TFORGE_FSR4_ENABLE_COLOR_HISTORY=1 TFORGE_FSR4_LEARNED_STRENGTH=0.15 TFORGE_FSR4_LEARNED_CONFIDENCE_BLEND=1.0) ;;
      esac
      result="$out/$case/${scene}_${resolution}.csv"
      mkdir -p "$(dirname "$result")"
      env -u TFORGE_TEMPORAL_MOTION_JSON -u TFORGE_TEMPORAL_METRICS_CSV \
        -u TFORGE_TEMPORAL_EVENTS_JSON -u TFORGE_TEMPORAL_STATIC_MASK_JSON \
        -u TFORGE_TEMPORAL_CLASS "${env_args[@]}" \
        benchmarks/video_corpus/run_temporal_quality.sh "$player" "$input" "$reference" "$result" 24
    done
  done
done
```

The runner rejects pre-existing sidecars. Use fresh paths for any later
motion/event sidecar; never reuse an old JSON artifact.

## Decision gate

Use `coverage_control` as the fixed baseline. Do not promote on aggregate SSIM
alone. This capture did not pass the gate: the best aggregate candidate had
better overall CSV metrics but regressed specific scene/resolution pairs.
Retain the default coverage-only path.

## Evidence

- `swarm/agent_coverage_validation/measured_results.json` and `README.md`:
  sparse-coverage A/B results.
- `swarm/agent_next_combo/measured_results.json` and `README.md`: existing
  blend matrix and 1280x720 validation.
- `swarm/agent_confidence_blend/README.md`: confidence-blend tradeoff.
- `swarm/agent_inventory/manifest.md`, `src/render/Fsr4DispatchHarness.cpp`,
  and `benchmarks/video_corpus/run_temporal_quality.sh`: accepted/forwarded
  controls.

The quality variables in the command are parsed and forwarded. The explicit
`-u` clauses prevent an inherited sidecar request from changing this into an
enhanced-metrics run. No new algorithm or source behavior is proposed.
