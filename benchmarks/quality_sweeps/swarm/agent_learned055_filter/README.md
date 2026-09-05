# Learned 0.55 current-composition base-filter matrix

Captured review evidence. This isolates the current-composition base filter while
holding `TFORGE_FSR4_LEARNED_STRENGTH=0.55`. The run completed 48 captures
including the explicit Catmull control. No source, default, or benchmark image
changes were made.

## Candidates

| id | role | filter | learned strength |
| --- | --- | --- | ---: |
| `current_default` | paired reference | runtime default, Catmull-Rom | unset |
| `learned_055_default` | paired learned control | `catmull_rom` | `0.55` |
| `learned_055_bilinear` | candidate | `bilinear` | `0.55` |
| `learned_055_mitchell` | candidate | `mitchell` | `0.55` |
| `learned_055_catmull_rom` | candidate | `catmull_rom` | `0.55` |
| `learned_055_lanczos2` | candidate | `lanczos2` | `0.55` |

`learned_055_default` is required to separate the learned-strength effect
from the filter effect. Compare each filter candidate both to that paired
control and to `current_default`; do not credit a filter for the learned-only
delta.

The four filter values are Quality Lab values, not guesses at the legacy
environment switch. `TFORGE_FSR4_CURRENT_BASE_FILTER=bilinear` (or its alias
`linear`) selects linear sampling; any other value retains the legacy
Catmull-Rom fallback. It cannot select Mitchell or Lanczos2. Therefore each
learned candidate uses `TFORGE_QUALITY_LAB_CONFIG` with
`composition.mode=current`, fixed `learnedStrength=0.55`, neutral residual,
sharpen, tone, and presentation settings, and only `baseFilter.mode` changes.
The config's `baseFilter` coefficients are `bilinear: 1/3,1/3`,
`mitchell: 1/3,1/3`, `catmull_rom: 0,0.5`, and `lanczos2: 0,0`.

The source consumer chain is:

```text
TFORGE_QUALITY_LAB_CONFIG
  -> src/config/QualityLabConfig.cpp:163-167
  -> src/render/Fsr4DispatchHarness.cpp:3209-3214 (slot3.y/slot4.x/y)
  -> shaders/fsr4/postpass_composite.comp:320-324, 626-630
     sampleSourceBase(...)
```

The legacy environment is consumed at
`src/render/Fsr4DispatchHarness.cpp:3124-3128` and only sets the bilinear bit;
the postpass fallback is at `shaders/fsr4/postpass_composite.comp:631-635`.

## Corpus and capture policy

Use only `tos_daylight`, `tos_debris`, `sintel_rooftop`, and `sintel_cave`.
Capture `426x240 -> 1920x1080` and `1280x720 -> 3840x2160`, using 36 warmup
frames followed by 24 scored frames. Use fresh output directories and unset
all temporal sidecars and every control listed in `matrix.json` as
`mustRemainUnset`. `matrix.json` is authoritative for candidate settings.

## Strict decision gate

Against the matched baseline, every candidate must have finite metrics and
expected dimensions on all eight scene/resolution pairs, SSIM delta at least
`-0.0005`, temporal-error relative increase at most `2%`, and GPU median time
increase at most `0.25 ms`. It must also improve SSIM or temporal error on at
least three pairs, have nonnegative mean SSIM delta, and have nonpositive mean
temporal-error delta. Aggregate results never waive a failed pair.

## Human review

Review all eight still pairs and scored-frame strips for every numerically
eligible filter. Record pass/fail for softness and fine detail, ringing or
halos, edge overshoot, texture loss, ghosting, flicker, and motion-detail
stability. Review `learned_055_default` versus `current_default`, then each
filter versus both controls. Any visible regression blocks promotion.

## Measured decision

| candidate | mean SSIM delta | mean temporal-error delta | gate failures | improved pairs |
| --- | ---: | ---: | ---: | ---: |
| `learned_055_default` | +0.000196750 | -0.105437500 | 0 | 5 |
| `learned_055_bilinear` | +0.002333125 | +0.026947750 | 6 | 2 |
| `learned_055_mitchell` | +0.002233750 | +0.003629000 | 6 | 2 |
| `learned_055_catmull_rom` | +0.000196500 | -0.105439750 | 0 | 5 |
| `learned_055_lanczos2` | +0.000118125 | -0.106949625 | 2 | 2 |

Bilinear and Mitchell raised spatial SSIM but caused temporal regressions;
Lanczos2 failed two paired gates. Nothing was promoted. The machine-readable
result is in `measured_results.json`.
