# Learned-0.55 detail-residual Quality Lab matrix

Capture-free design for the next isolated quality campaign. The preceding
current-path `learned_055` candidate passed the numeric gates but looked
visually soft; CAS, alternate base filters, and linear blending did not clear
the review bar. This matrix tests only whether a small learned high-frequency
residual can recover useful detail without reintroducing ringing, halos,
staircases, texture instability, or temporal shimmer.

This is an experimental composition study. It does not change the default,
source, shader, model, image, or playback behavior. Do not promote a candidate
from these results alone.

## Candidates and fixed controls

`matrix.json` is authoritative. It contains the complete Quality Lab payload
for each candidate so this package intentionally contains only `README.md` and
`matrix.json`; no per-candidate config files are required.

| id | role | composition | learned strength | residual strength |
| --- | --- | --- | ---: | ---: |
| `learned_055_current_control` | paired baseline | `current` | 0.55 | n/a |
| `detail_residual_002` | experimental | `detail_residual` | 0.55 | 0.02 |
| `detail_residual_005` | experimental | `detail_residual` | 0.55 | 0.05 |
| `detail_residual_010` | experimental | `detail_residual` | 0.55 | 0.10 |

Every row fixes Catmull-Rom (`b=0`, `c=0.5`), box3x3 residual low-pass
(`radius=1`, `sigma=0.85`), Quality Lab adaptive sharpen off, neutral tone,
and bicubic presentation. Set `TFORGE_FSR4_CAS_STRENGTH=0` explicitly for
every process and leave legacy/alternate filter and linear-blend controls
unset. The current control remains the current composition path; its
resolution-aware host behavior is pinned with `TFORGE_FSR4_LEARNED_STRENGTH=0.55`.

## Exact shader path to verify

Before capture, verify the built binary and shader source correspond to this
path; a passing metric file without this wiring check is invalid evidence:

```text
TFORGE_QUALITY_LAB_CONFIG
  -> src/config/QualityLabConfig.cpp (composition/baseFilter/strength parsing)
  -> src/render/Fsr4DispatchHarness.cpp:3209-3247
     slot3.x=composition, slot3.y=baseFilter, slot5.x/y=learned/residual
  -> shaders/fsr4/postpass_composite.comp:626-630
     Quality Lab base-filter selection
  -> shaders/fsr4/postpass_composite.comp:647-710
     current branch or experimental composition branch
  -> shaders/fsr4/postpass_composite.comp:694-701
     detail_residual (slot3.x == 4u): base +
     residualStrength * (learned - learnedLowpass3x3)
  -> shaders/fsr4/postpass_composite.comp:712-716
     CAS remains after composition and is fixed neutral for this matrix
```

For each residual candidate, confirm the runtime trace reports
`composition=detail_residual`, `base=catmull_rom`, `learnedStrength=0.55`, and
the declared residual strength. Confirm the control reports `composition=current`
and uses the current branch. A config parse fallback, wrong mode, or missing
trace is a hard failure, not a result.

## Real corpus and capture policy

Use only these real scenes and high-quality inputs:

```text
tos_daylight
tos_debris
sintel_rooftop
sintel_cave
```

Capture both `426x240 -> 1920x1080` and `1280x720 -> 3840x2160`. Run 36 warmup
frames and score the following 24 frames. The matrix is therefore 4 candidates
× 4 scenes × 2 input resolutions = 32 captures, with eight paired cells per
experimental candidate. Use fresh output directories; do not add motion,
event, static-mask, or class sidecars.

The payload can be materialized outside the repository for a run, preserving
the two-file package:

```bash
set -euo pipefail
plan="$PWD/benchmarks/quality_sweeps/swarm/agent_learned055_residual"
tmp="$(mktemp -d /tmp/tforge-learned055-residual.XXXXXX)"
trap 'rm -rf "$tmp"' EXIT
for candidate in learned_055_current_control detail_residual_002 detail_residual_005 detail_residual_010; do
  jq -c --arg id "$candidate" '.candidates[] | select(.id == $id) | .qualityLabConfig' \
    "$plan/matrix.json" > "$tmp/$candidate.json"
done
```

Run the repository's temporal runner once per candidate, setting
`TFORGE_QUALITY_LAB_CONFIG` to the corresponding temporary JSON path,
`TFORGE_FSR4_LEARNED_STRENGTH=0.55`, `TFORGE_FSR4_CAS_STRENGTH=0`, and
`TFORGE_TEMPORAL_WARMUP_FRAMES=36`; score 24 frames against the matching
`benchmarks/video_corpus/references/{scene}_2160p_lossless.mkv`. The exact
input and output mapping is in `matrix.json`.

## Strict decision gate

Compare every experimental cell with `learned_055_current_control` at the
same scene and input resolution. All of the following are mandatory:

- all eight pairs are complete, finite, correctly dimensioned, and wiring-verified;
- no pair loses more than `0.0005` SSIM;
- no pair increases temporal error by more than `2%` relative;
- matched GPU median does not increase by more than `0.25 ms`;
- at least three pairs improve SSIM or temporal error;
- mean SSIM delta is nonnegative and mean temporal-error delta is nonpositive.

No aggregate-only winner is acceptable. Any failed pair rejects that
candidate. GPU timing must be measured on the same path, not inferred from
the capture runner.

## Human visual review

Review every numerically eligible candidate against the paired control using
full frames and scored-frame strips from all eight pairs. Record pass/fail for
fine detail recovery and softness, edge overshoot, ringing/halos, staircase
return, foliage/text texture loss, ghosting, flicker, and motion-detail
stability. Any visible regression, even if metrics pass, blocks promotion.

The output of this matrix is evidence for a later decision only. It is not a
default change and must not trigger source, shader, config-default, or image
edits.

## Smoke result

The five-case real-scene smoke rejected all residual candidates: residual
strengths `0.02`, `0.05`, and `0.10` lost `0.003297`, `0.003387`, and `0.003580`
SSIM respectively against the current default. The remaining captures were
not run. See `measured_results.json`.
