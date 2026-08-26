# `qualityLab.baseFilter.mode` wiring audit

Date: 2026-08-24

## Result

There was no JSON/config forwarding bug. `qualityLab.baseFilter.mode` is parsed,
stored, copied into every FSR pass, and written into the postpass push
constants. The identical full-composition results are caused by the active
`composition.mode = current` shader branch deliberately retaining the legacy
Catmull-Rom base resolve. That branch does not read the Quality Lab base-filter
selector.

The initial audit found the current branch intentionally ignored the selector.
The harness now has a guarded Quality Lab path that consumes the selector only
when `qualityLab.enabled` is true and `composition.mode` is `current`; disabled
lab mode retains the legacy path. The default runtime remains unchanged.

## Exact data path

1. JSON is supplied by `TFORGE_QUALITY_LAB_CONFIG`, forwarded by
   `benchmarks/video_corpus/run_temporal_quality.sh`, and loaded by
   `src/main.cpp`.
2. `src/config/QualityLabConfig.cpp:163-167` reads
   `qualityLab.baseFilter.mode`, maps it to `QualityBaseFilterMode`, and reads
   `b` and `c`. The enum values are:

   ```text
   Bilinear   = 0
   Mitchell   = 1
   CatmullRom = 2
   Lanczos2   = 3
   ```

3. `src/main.cpp:86-95` loads and logs the resolved value. At
   `src/main.cpp:118-120`, the resulting `QualityLabConfig` is passed to
   `PlaybackEngine`.
4. `src/core/PlaybackEngine.hpp:114-116` stores the typed object. Each FSR
   pass created at `src/core/PlaybackEngine.cpp:689-710` receives the same
   object through `setQualityLabConfig` at line 698. This includes later
   chained passes; the value is not lost at pass construction.
5. `src/render/Fsr4DispatchHarness.cpp:3209-3216` writes the resolved enum and
   coefficients to the postpass constant buffer:

   ```text
   slot3.y = qualityLabConfig_.baseFilterMode
   slot4.x = qualityLabConfig_.baseB
   slot4.y = qualityLabConfig_.baseC
   ```

6. `shaders/fsr4/postpass_composite.comp:22-23` declares those fields as the
   `baseFilter`, `baseB`, and `baseC` controls. The compiled artifact
   `build-fast/shaders/fsr4/postpass_composite.spv` and its generated header
   were present and up to date during this audit; `cmake --build build-fast`
   reported `ninja: no work to do`.

## Why the four full-composition captures match

The audit manifest sets `composition.mode` to `current`. The shader computes
`experimentalComposition` at `postpass_composite.comp:610` as
`qualityEnabled && slot3.x != 0u`, so `current` enters the preserved branch at
line 638.

Before that branch, the current path selects its base as follows
(`postpass_composite.comp:622-626`):

```glsl
const vec3 modelBaseColor = (slot0.z & 8u) != 0u
    ? removeMuLaw(sampleSourceBilinear(...))
    : removeMuLaw(sampleSourceBicubic(...));
```

That was a legacy benchmark environment switch (`TFORGE_FSR4_CURRENT_BASE_FILTER`)
for bilinear versus the preserved Catmull-Rom/bicubic path. It was independent
of `slot3.y`; Mitchell and Lanczos2 could not be selected by the current branch
before the guarded Quality Lab wiring was added.
The branch then blends the learned result and applies the legacy edge-aware
RCAS path (`postpass_composite.comp:638-649`). This is why changing only
`baseFilter.mode` in a `current` JSON file does not change the full-composition
image.

The Quality Lab selector is consumed in the experimental branch instead:

```glsl
const vec3 baseColor = removeMuLaw(sampleSourceBase(
    baseSourcePos, sourceTileBase, slot3.y, slot4.x, slot4.y));
```

`sampleSourceBase` maps the four enum values at
`postpass_composite.comp:320-324` to bilinear, Mitchell/B-C cubic,
Catmull-Rom, and Lanczos2. This path is used for `base_only`, `direct_blend`,
and `detail_residual` compositions, not for `current`.

## Capture evidence

The source data is in
`benchmarks/quality_sweeps/swarm/agent_filter_full_audit/`:

- 32 real-world captures: 4 scenes × 2 input resolutions × 4 filter values.
- Every capture used `composition.mode = current`.
- At 1280x720 input, all four corresponding CSVs are byte-identical for the
  measured rows. At 426x240, the few last-digit differences are ordinary
  capture/metric variation; the filter choices still have no systematic effect
  and are not evidence that the selector was consumed.
- Aggregate values recorded in `measured_results.json` are effectively the
  same:

  ```text
  bilinear    mean SSIM 0.909590   mean temporal error 0.585703
  mitchell    mean SSIM 0.909590   mean temporal error 0.585701
  catmull_rom mean SSIM 0.909590   mean temporal error 0.585702
  lanczos2    mean SSIM 0.909590   mean temporal error 0.585701
  ```

## Correct interpretation for future captures

The pre-wiring captures cannot answer “which base filter is best in the full
current composition”; they establish that JSON forwarding was not the cause of
the neutral result. A four-case post-wiring smoke capture now produces distinct
metrics (bilinear SSIM 0.857969, Mitchell 0.858054, Catmull-Rom 0.854193,
Lanczos2 0.854010 on Tears of Steel daylight 426x240), so the selector is now
actually exercised in the controlled Quality Lab path.

To compare the four Quality Lab base filters, the capture manifest must use a
composition that consumes `slot3.y`, such as `base_only`, `direct_blend`, or
`detail_residual`. Changing the current branch to consume the selector would
be a reconstruction behavior change and is outside this audit’s scope.

## Validation

- `cmake --build build-fast -j 16` — passed; no rebuild required.
- `spirv-dis build-fast/shaders/fsr4/postpass_composite.spv` — passed; the
  embedded active shader contains `slot3`, `sampleSourceBilinear`, and
  `sampleSourceBase`.
- Existing focused Quality Lab/config and runner contract tests remain
  unchanged and were not used to claim a nonexistent forwarding fix.
- The guarded postpass wiring and its source-contract test were modified; no
  benchmark image files were modified.
- `git diff --check` — passed after adding this README.
