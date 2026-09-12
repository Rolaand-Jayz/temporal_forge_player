# Temporal Forge quality-control inventory

Inspected 2026-08-24. This is a static inventory only. No GPU capture, player
run, build, source edit, default edit, or benchmark-image edit was performed.

## Authoritative typed quality configuration

The player loads JSON from `TFORGE_QUALITY_LAB_CONFIG`; absent that override it
checks `./config/quality_lab.json`, the compiled source-root config, then
`$XDG_CONFIG_HOME/temporal-forge-player/quality_lab.json`, then
`$HOME/.config/temporal-forge-player/quality_lab.json`.
Source: `src/config/QualityLabConfig.cpp:106-148`,
`src/config/QualityLabConfig.hpp:76-79`.

The JSON root may be `qualityLab` or the document itself. `enabled` is boolean.
All numeric fields are finite and clamped to the ranges below. Unknown enum
strings fall back to the parser defaults, not an error.

| JSON path | Type / valid values | Numeric range | Effect |
|---|---|---:|---|
| `qualityLab.composition.mode` | `current`, `base_only`, `learned_only`, `direct_blend`, `detail_residual` (aliases without `_` accepted) | — | composition |
| `qualityLab.composition.learnedStrength` | number | `[0,1]` | learned contribution |
| `qualityLab.composition.residualStrength` | number | `[0,2]` | detail-residual contribution |
| `qualityLab.baseFilter.mode` | `bilinear`, `mitchell`, `catmull_rom`, `lanczos2` (`mitchell_netravali` and `lanczos` aliases accepted) | — | spatial base |
| `qualityLab.baseFilter.b` | number | `[-1,1]` | cubic filter B |
| `qualityLab.baseFilter.c` | number | `[-1,1]` | cubic filter C |
| `qualityLab.residual.lowpassMode` | `box3x3`, `gaussian3x3` (`gaussian` alias accepted) | — | residual low-pass |
| `qualityLab.residual.radius` | number | `[0.25,2]` | residual radius |
| `qualityLab.residual.sigma` | number | `[0.1,4]` | residual Gaussian sigma |
| `qualityLab.sharpen.mode` | `none`, `adaptive` | — | adaptive sharpen |
| `qualityLab.sharpen.strength` | number | `[0,1]` | sharpen strength |
| `qualityLab.sharpen.limit` | number | `[0,1]` | sharpen clamp |
| `qualityLab.sharpen.threshold` | number | `[0,1]` | sharpen threshold |
| `qualityLab.tone.exposureEV` | number | `[-4,4]` | tone exposure |
| `qualityLab.tone.contrast` | number | `[-1,1]` | tone contrast |
| `qualityLab.tone.contrastPivot` | number | `[0,1]` | tone pivot |
| `qualityLab.tone.gamma` | number | `[0.1,3]` | tone gamma |
| `qualityLab.presentation.filter` | `nearest`, `linear`, `bicubic`, `lanczos` (`bilinear` and `lanczos2` aliases accepted) | — | final presentation scaler |

Authoritative declarations/defaults are `src/config/QualityLabConfig.hpp:14-73`;
parsing, aliases, and clamps are `src/config/QualityLabConfig.cpp:61-99` and
`src/config/QualityLabConfig.cpp:152-198`. The checked-in default is
`config/quality_lab.json:2-32`, currently `enabled:false`, `base_only`,
bilinear, box3x3, no sharpen, exposure `-0.015`, bicubic presentation.

## Checked-in quality configurations

The sweep runner accepts either `experiments[].config` or
`experiments[].baseConfig` plus recursive `experiments[].overrides`. Exact
manifest/config families present in this checkout:

- `stage_a_manifest.json`: current, base-only bilinear, learned-only, and
  direct-blend strengths `0,.10,.25,.50,.75,1.00`.
- `stage_b_manifest.json`: base-only bilinear, Mitchell, Catmull-Rom, Lanczos2.
- `stage_c_manifest.json`: detail residual across those four base filters,
  box/gaussian low-pass, residual strengths `.25,.50,.75,1.00,1.25`.
- `stage_d_manifest.json`: none/adaptive sharpen, strength `0,.10,.20,.30,.40,.50`,
  plus limit `.0625/.125/.25` and threshold `.025/.05/.10` probes.
- `stage_e1_manifest.json`, `stage_e23_manifest.json`,
  `stage_e_fine_manifest.json`, `stage_e_refine_manifest.json`: tone exposure,
  contrast, contrast pivot, and gamma sweeps. `stage_f_*_manifest.json` covers
  presentation filters nearest/linear/bicubic/lanczos and local combinations.
- `stage_g/` and `stage_h_temporal_tiny/`: current later-stage combinations;
  `stage_h_temporal_tiny/current_control.json` is explicitly disabled quality lab.
- `swarm/agent_a/manifest.json`: direct blend strengths `.001,.002,.005` and
  residual `.00,.10,.25`.
- `swarm/agent_b/manifest.json`: base-only tone exposure `-.30,-.15,0,+.15`,
  sharpen `0,.03,.05`, plus environment `TFORGE_FSR4_CAS_STRENGTH=0.0`.
- `swarm/agent_e/manifest.json` and `agent_e2/manifest.json`: detail residual
  `.05,.10,.25,.50`, with one `.25` plus sharpen `.03` candidate.

Relevant file set: `benchmarks/quality_sweeps/stage_*_manifest.json`,
`benchmarks/quality_sweeps/stage_{a,b,c,d,e,f,g,h_temporal_tiny}/`, and
`benchmarks/quality_sweeps/swarm/agent_{a,b,e,e2}/`.

## Quality-affecting environment controls

These are the exact names read or forwarded by the current runner/runtime.
Boolean controls are enabled by presence unless noted.

| Name | Valid value / range | Scope |
|---|---|---|
| `TFORGE_BENCHMARK_PRESET` | `Off`, `NativeAA`, `Quality`, `Balanced`, `Performance`, `UltraPerformance`, `AutoMatchDisplay` | preset |
| `TFORGE_BENCHMARK_SHARPNESS` | float `[0,1]` | legacy sharpness |
| `TFORGE_BENCHMARK_JITTER_STRENGTH` | float `[0,1.5]` | benchmark jitter |
| `TFORGE_QUALITY_LAB_CONFIG` | filesystem path | JSON config selector |
| `TFORGE_FSR4_JITTER_MODE` | `off`, `current`, `reduced`, `controlled` | temporal input |
| `TFORGE_FSR4_CONTROLLED_JITTER` | float `[0,1.5]` | temporal input |
| `TFORGE_FSR4_FORCE_VIEWPORT` | `WIDTHxHEIGHT`, each dimension `>=2` | forced output target |
| `TFORGE_FSR4_FORCE_SCALE` | finite float `>=1.0` | forced upscale ratio |
| `TFORGE_FSR4_DRS` | presence boolean | selects DRS blob for generic path |
| `TFORGE_FSR4_CHAIN_PASSES` | positive integer | progressive pass count |
| `TFORGE_FSR4_LEARNED_STRENGTH` | float `[0,1]` | learned temporal strength override |
| `TFORGE_FSR4_DISABLE_LEARNED_CONFIDENCE_GATE` | presence boolean | bypasses motion-confidence weighting |
| `TFORGE_FSR4_CAS_STRENGTH` | float `[0,1]`, empty/unset means `0` | optional CAS |
| `TFORGE_FSR4_DISABLE_CAS` | presence boolean | disables CAS |
| `TFORGE_FSR4_LEGACY_RCAS_STRENGTH` | float `[0,1]` | current-mode RCAS |
| `TFORGE_FSR4_USE_DISPLAY_BASE` | presence boolean | current-mode display base at strength 1 |
| `TFORGE_FSR4_DISPLAY_BASE_STRENGTH` | float `[0,1]` | current-mode display base |
| `TFORGE_FSR4_CURRENT_BASE_FILTER` | `bilinear` or `linear` selects linear; other values retain default | current-mode base |
| `TFORGE_FSR4_CURRENT_BLEND_LINEAR` | presence boolean | current-mode blend |
| `TFORGE_FSR4_CURRENT_BASE_JITTERED` | presence boolean | current-mode base phase |
| `TFORGE_FSR4_EXPERIMENTAL_BASE_UNJITTERED` | presence boolean | experimental composition base phase |
| `TFORGE_FSR4_ENABLE_RECURRENT` / `TFORGE_FSR4_DISABLE_PREPASS` | presence boolean | recurrent/prepass temporal policy |
| `TFORGE_FSR4_ENABLE_COLOR_HISTORY` / `TFORGE_FSR4_DISABLE_COLOR_HISTORY` | presence boolean | color-history policy; disable wins |
| `TFORGE_FSR4_DISABLE_POSTPASS` | presence boolean | removes postpass |
| `TFORGE_FSR4_DISABLE_NATIVE_INT8` | presence boolean | forces non-native fallback |
| `TFORGE_FSR4_DISABLE_FUSED_INT8` / `TFORGE_FSR4_ENABLE_FUSED_INT8` | presence boolean | fused INT8 dispatch selection |

Runtime anchors: `src/main.cpp:99-115`, `src/core/PlaybackEngine.cpp:30-42,
497-550,618-660,1510-1516`, and `src/render/Fsr4DispatchHarness.cpp:2494-2499,
2915-3066,3104-3232`. The capture forwarding lists are
`benchmarks/video_corpus/run_quality.sh:131-152` and
`benchmarks/video_corpus/run_temporal_quality.sh:147-188`.

`run_quality_sweep.py:66-74,119` permits inherited candidate environment names
when they match prefixes `TFORGE_QUALITY_`, `TFORGE_BENCHMARK_`, `TFORGE_FSR4_`,
`TFORGE_UPSCALE_`, `TFORGE_JITTER_`, or `TFORGE_REVIEW_`,
plus the explicit key `TFORGE_DISABLE_HW_DECODE`. This is a forwarding policy,
not proof that every prefixed name is consumed by the player.

## Runner selectors and CLI names

Quality sweep CLI (`benchmarks/quality_sweeps/run_quality_sweep.py:77-105`):
`--manifest PATH`, `--binary PATH`, `--output-root PATH`, optional
`--tag-prefix TEXT`, `--continue-on-error`, `--workers INT`, `--retries INT`.
`--workers` also defaults from `TFORGE_CAPTURE_WORKERS`.

Manifest-level selectors consumed by the runner are `preset`, `dimensions`,
`outputDimensions` (`WIDTHxHEIGHT`), `frame` (integer), `quality`, `clipRegex`,
`corpusManifest`, `jitter.mode`, `jitter.controlledStrength`, `classSelections`,
and `qualityClassAnnotationsPath`; candidate identity/config fields are `id`,
`config` or `baseConfig`, and `overrides`. Anchors:
`run_quality_sweep.py:256-280,300-323,332-372`.

Matrix shell CLIs are:

- `run_quality.sh PLAYER [SELECTOR] [OUTPUT_CSV]`;
- `run_quality_matrix.sh PLAYER SELECTOR OUTPUT_DIRECTORY`;
- `run_temporal_quality.sh PLAYER INPUT REFERENCE OUTPUT_CSV [FRAMES]`;
- `run_temporal_quality_matrix.sh PLAYER INPUT REFERENCE OUTPUT_DIR [FRAMES]
  [--retries N] [--dry-run]`.

Temporal metric-sidecar tools have separate non-quality evidence CLIs, including
`--motion-json`, `--static-mask-json`, `--events-json`, thresholds, and output
paths. They select/measure evidence and do not change rendered quality.

## Build/config findings

There are no quality-specific CMake options or CLI options in `CMakeLists.txt`.
The only build cache default affecting build configuration is
`CMAKE_BUILD_TYPE`, defaulted to `Release` at `CMakeLists.txt:15-17`; the build
also requires Ninja, FFmpeg, Vulkan, and Qt6 at `CMakeLists.txt:23-48,78-79`.
Quality is runtime-configured through the JSON and environment controls above.

## Deliberately excluded from the quality-knob list

`TFORGE_HEADLESS_BENCHMARK`, dump/readback variables, profiling/trace variables,
fence timeouts, logging, capture artifact paths, corpus selectors, and temporal
metric sidecars are benchmark plumbing or diagnostics. They can affect capture
or observability, but are not image-quality configurations. `TFORGE_DISABLE_HW_DECODE`
is forwarded as an explicit capture environment key and changes decode path, not
the reconstruction quality setting.
