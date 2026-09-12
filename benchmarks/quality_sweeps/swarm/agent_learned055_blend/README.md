# Learned 0.55 current blend-space matrix

This is an isolated, capture-free design for comparing the normal current
composition sRGB blend with the existing opt-in linear-light blend. Learned
strength is fixed at `0.55` for both explicit-strength arms. The package does
not change source, defaults, shaders, benchmark images, or captures.

## Candidates

| id | role | learned strength | blend space | environment |
| --- | --- | ---: | --- | --- |
| `current_default` | baseline | runtime default | current default, sRGB | unset |
| `learned_055_srgb` | paired control | `0.55` | sRGB | `TFORGE_FSR4_LEARNED_STRENGTH=0.55` |
| `learned_055_linear` | candidate | `0.55` | linear-light | `TFORGE_FSR4_LEARNED_STRENGTH=0.55`, presence-only `TFORGE_FSR4_CURRENT_BLEND_LINEAR` |

`current_default` is retained as the product baseline. The explicit
`learned_055_srgb` arm isolates the learned-strength change from the current
resolution-dependent default. The candidate differs from that paired control
only by the blend-space flag.

Do not set a Quality Lab config. Keep current composition, recurrent/history,
confidence, motion, jitter, base filter, CAS, legacy RCAS, tone, presentation,
and all other experimental controls at their existing defaults. The linear
flag is presence-based, not a valued setting. An unset variable is the sRGB
control; setting it to any non-empty value enables the candidate path.

## Exact wiring verified

- `benchmarks/video_corpus/run_temporal_quality.sh:161-188` forwards both
  `TFORGE_FSR4_CURRENT_BLEND_LINEAR` and
  `TFORGE_FSR4_LEARNED_STRENGTH` to each player process.
- `src/render/Fsr4DispatchHarness.cpp:3129-3130` checks presence of
  `TFORGE_FSR4_CURRENT_BLEND_LINEAR` and sets `PostpassCB.slot0[2] |= 16u`.
- `src/render/Fsr4DispatchHarness.cpp:3155-3178` parses and clamps
  `TFORGE_FSR4_LEARNED_STRENGTH` to `[0,1]`, then writes the resolved learned
  blend amount to `PostpassCB.slot1[3]` at `:3203`.
- `shaders/fsr4/postpass_composite.comp:651-655` clamps `slot1.w`; without
  bit `16u` it performs the current sRGB `mix`, and with bit `16u` it performs
  `srgbToLinear` on both inputs, mixes in linear light, then applies
  `linearToSrgb`.

This is the current composition branch only. The shader's separate
Quality-Lab composition branch is not part of this matrix.

## Corpus and capture plan

Use only these four real corpus scenes:

```text
tos_daylight
tos_debris
sintel_rooftop
sintel_cave
```

Run both input resolutions and their established output mappings:

```text
426x240   -> 1920x1080
1280x720  -> 3840x2160
```

For every candidate/scene/resolution pair, capture `36` warmup frames and
`24` scored frames. This gives `3 × 4 × 2 = 24` fresh captures. Use a fresh
result directory outside this package, such as `/tmp/tforge-learned055-blend`,
and do not add captures or image artifacts under this directory.

The runner invocation shape is:

```bash
TFORGE_TEMPORAL_WARMUP_FRAMES=36 \
  benchmarks/video_corpus/run_temporal_quality.sh \
  "$player" "$input" "$reference" "$result" 24
```

For `current_default`, leave both matrix variables unset. For
`learned_055_srgb`, set only `TFORGE_FSR4_LEARNED_STRENGTH=0.55`. For
`learned_055_linear`, set that same strength and set
`TFORGE_FSR4_CURRENT_BLEND_LINEAR=1`. Before each run, explicitly unset any
ambient values for the controls listed in `mustRemainUnset` in `matrix.json`.
Run each arm in a separate player process so the host's process-local static
strength override cannot leak between arms.

## Strict paired decision gate

Compare each arm to `current_default` on the identical scene and input
resolution. Also compare `learned_055_linear` to `learned_055_srgb` to ensure
any candidate result is attributable to blend space. Every pair must be
complete, finite, dimension-correct, and pass all per-pair limits:

1. SSIM delta is at least `-0.0005`.
2. Temporal-error relative increase is at most `2%`.
3. GPU time increase is at most `0.25 ms` on the matched path.

For promotion eligibility, require at least `3` improved pairs among the eight
scene/resolution pairs, mean SSIM delta at least `0`, and mean temporal error
delta at most `0`, with no per-pair failure. Aggregate means never waive a
failed pair. Report the explicit sRGB control separately from the baseline.

## Human visual review gate

Metrics are necessary but insufficient. Review all eight candidate-versus-sRGB
still pairs and scored-frame strips for every scene/resolution pair. Also
review the sRGB control against `current_default` so the strength change is not
mistaken for a blend-space effect. Record pass/fail for fine detail, gradients
and shadow transitions, edge overshoot/halos, compression texture, temporal
flicker, ghosting, and motion-detail stability. Any visible regression in any
pair blocks promotion, even when numeric gates pass.

No result is promoted by this design alone. Do not edit defaults, source,
shaders, captures, or benchmark images from this matrix.

## Measured decision

| candidate | mean SSIM delta | mean temporal-error delta | gate failures | improved pairs |
| --- | ---: | ---: | ---: | ---: |
| `learned_055_srgb` | +0.000196375 | -0.105432875 | 0 | 5 |
| `learned_055_linear` | +0.000112000 | -0.112258250 | 2 | 1 |

Linear blending is not promotable under the paired gates. The complete result
is in `measured_results.json`.
