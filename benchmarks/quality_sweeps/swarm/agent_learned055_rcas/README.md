# Capture-ready learned 0.55 plus legacy RCAS matrix

Status: **capture-ready, not captured**. This package contains only the
matrix definition and capture/review protocol. It changes no captures,
source, shader, model, default, or benchmark image.

The matrix tests fixed learned strength `0.55` with the existing current-path
legacy RCAS values `0.08`, `0.12`, `0.16`, and `0.20`. `current_default` is
the product baseline. `learned_055_rcas_008` is a separate paired control that
holds RCAS at the current default while making learned strength explicit.

## Exact current-path wiring and range

This is the normal current composition, not Quality Lab and not optional
postpass CAS.

- `src/render/Fsr4DispatchHarness.cpp:3155-3178` reads
  `TFORGE_FSR4_LEARNED_STRENGTH`, clamps it to `[0, 1]`, and overrides the
  resolution-aware learned strength. After normal confidence weighting,
  `:3203` writes the effective value to `pp.slot1[3]` (`slot1.w`).
- `shaders/fsr4/postpass_composite.comp:647-658` is the non-experimental
  current branch. It consumes `slot1.w` for the learned/base blend, then
  passes `clamp(slot4.w, 0.0, 1.0)` to `applyLegacyEdgeAwareRcas`.
- `src/render/Fsr4DispatchHarness.cpp:3221-3227` reads
  `TFORGE_FSR4_LEGACY_RCAS_STRENGTH`, defaults it to `0.08`, clamps it to
  `[0, 1]`, and stores it in `pp.slot4[3]` (`slot4.w`).
- `shaders/fsr4/postpass_composite.comp:327-341` implements
  `applyLegacyEdgeAwareRcas`; its current-path call site is `:656-658`.

Both controls therefore accept `[0.0, 1.0]`. The product baseline leaves
both overrides unset, preserving the host's resolution-aware learned default
and legacy RCAS default `0.08`.

## Matrix and corpus

Five candidates × four real scenes × two input resolutions = **40 captures**.
Every capture runs **36 warmup frames followed by 24 scored frames**.

| id | learned | legacy RCAS | role |
| --- | ---: | ---: | --- |
| `current_default` | unset | unset, host default | product baseline |
| `learned_055_rcas_008` | 0.55 | 0.08 | paired learned control |
| `learned_055_rcas_012` | 0.55 | 0.12 | RCAS candidate |
| `learned_055_rcas_016` | 0.55 | 0.16 | RCAS candidate |
| `learned_055_rcas_020` | 0.55 | 0.20 | RCAS candidate, upper point |

Use only `tos_daylight`, `tos_debris`, `sintel_rooftop`, and `sintel_cave`.
Use high-quality `crf12` inputs at `426x240` and `1280x720`, with expected
outputs `1920x1080` and `3840x2160` respectively. Do not resample outputs.

## Capture protocol

Run from the repository root with a freshly built
`build-fast/temporal_forge_player` and a fresh output directory. Unset all
sidecars and unrelated quality controls for every process. Set only the two
matrix controls, plus `TFORGE_TEMPORAL_WARMUP_FRAMES=36`; pass `24` scored
frames to `run_temporal_quality.sh`.

The exact environment contract is in `matrix.json`. The runner must be
`benchmarks/video_corpus/run_temporal_quality.sh`, and each output must be
written to `captures/{candidate}/{scene}_{resolution}.csv`. Do not reuse
existing outputs or sidecars.

## Strict paired gates

Compare every candidate to `current_default` at the identical scene and input
resolution. Also compare each RCAS candidate (`012`, `016`, `020`) to
`learned_055_rcas_008` at the identical cell. The paired control is
diagnostic and does not replace the product baseline.

Before comparison, every capture must have finite metrics, exactly 24 scored
frames, expected dimensions, and timing fields. Record matched GPU median and
the scored-path timing median and p95. Every applicable pair must satisfy:

1. SSIM delta at least `-0.0005`.
2. Temporal-error relative increase no more than `2%`.
3. Matched GPU median increase no more than `0.25 ms`.

After per-pair checks, require at least three of the eight product-baseline
pairs to improve SSIM or temporal error, mean SSIM no lower than baseline,
and mean temporal error no higher. RCAS candidates must also pass the same
per-pair safety checks against the paired learned control. No aggregate-only
pass is allowed, and a failed pair cannot be waived.

## Human review gate

For every candidate passing machine gates, review all eight still pairs
against `current_default`; for RCAS candidates also review all eight against
`learned_055_rcas_008`. Review short strips covering the 24 scored frames for
every scene/resolution cell. Record pass/fail and notes for fine detail,
perceived softness, ringing or halos, edge overshoot, codec texture/noise,
temporal flicker, ghosting, and motion-detail stability. Any visible
regression blocks promotion.

This is capture and review evidence only. Do not promote a default or change
source, shaders, models, playback behavior, captures, or benchmark images
from this matrix alone.

## Measured result

All 40 captures completed. Every RCAS level failed the mandatory paired gates,
so none is promoted and no timing or visual-review pass is claimed. The common
failed cells were `tos_daylight_1280x720` (SSIM floor),
`tos_debris_426x240` (temporal error), and `sintel_cave_1280x720` (temporal
error). See `measured_results.json` for the exact aggregates and capture CSVs
under `captures/` for the raw evidence.

## Evidence anchors

- `src/render/Fsr4DispatchHarness.cpp`
- `shaders/fsr4/postpass_composite.comp`
- `benchmarks/video_corpus/run_temporal_quality.sh`
- `benchmarks/quality_sweeps/swarm/agent_sharpen_audit/README.md`
