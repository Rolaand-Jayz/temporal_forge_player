# Learned-confidence blend reproducibility package

Status: **not-promoted**.

This is a capture-free package for the opt-in `TFORGE_FSR4_LEARNED_CONFIDENCE_BLEND` control. No capture is authorized by this package. The measured intermediate values below are recorded from existing `/tmp` CSV artifacts only.

## Semantics

The control is parsed as a finite float and clamped to `[0, 1]`.

- `0`: legacy confidence-gated behavior. The effective confidence remains the clamped history confidence.
- `1`: ungated behavior. The effective confidence is `1.0`.
- `0.10` and `0.15`: intermediate blends between those endpoints.

The interpolation is `blend + (1 - blend) * historyConfidence`, then the learned strength is multiplied by that effective confidence. The control is opt-in; leaving the variable unset preserves the legacy path.

## Exact runner forwarding

Both `benchmarks/video_corpus/run_quality.sh` and `benchmarks/video_corpus/run_temporal_quality.sh` include the variable in their explicit environment allowlists. When the caller supplies a non-empty value, the runner forwards the exact string as:

```text
TFORGE_FSR4_LEARNED_CONFIDENCE_BLEND=<value>
```

The quality runner invokes the player through `env "${benchmark_env[@]}" ... "$binary" "$path"`; for this 1280x720 selector it also adds `TFORGE_FSR4_FORCE_VIEWPORT=1281x720` when the caller has not supplied a viewport. The temporal runner exports its assembled `player_environment` before invoking the player. No caller environment outside the allowlist is forwarded.

The package's planned command is documentation only and must not be run during capture-free preparation:

```text
python3 benchmarks/quality_sweeps/run_quality_sweep.py --manifest benchmarks/quality_sweeps/swarm/agent_confidence_blend/manifest.json --binary build/temporal_forge_player --output-root <capture-output> --workers 1
```

## Existing high-strength intermediate evidence

The `.10` and `.15` records are 24-frame, 1280x720 results for the four named scenes. The raw source files are listed in `measured_results.json`; they remain outside this package under `/tmp` and are not copied or regenerated.

At `.15` versus `.10`, mean SSIM increased slightly in every scene (`+0.000335`, `+0.000234`, `+0.000034`, `+0.000062` for Sintel Cave, Sintel Rooftop, ToS Daylight, and ToS Debris respectively). Temporal tradeoffs are scene-dependent: the reference temporal-delta absolute error improved on Rooftop and Daylight, worsened on Cave, and worsened slightly on Debris. Minimum SSIM fell at `.15` for all four scenes. These intermediate results are not sufficient for promotion.

The JSON manifest and analysis record preserve the per-scene values and tradeoffs. `analysis.template.json` is a null-valued template for a future rerun; do not fill it from visual impressions or unpaired measurements.

