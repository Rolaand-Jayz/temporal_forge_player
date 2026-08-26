# Intermediate learned-strength sweep

Status: captured, not promoted. This matrix narrowed the only promising
current-path control after the full sweep: explicit learned strength `0.55`
passed the numeric gates but looked slightly soft, while `0.35` had paired
failures. It tests `0.40`, `0.45`, `0.50`, and `0.55` against the unchanged
resolution-aware default.

Use only the four real scenes `tos_daylight`, `tos_debris`, `sintel_rooftop`,
and `sintel_cave`, at `426x240 -> 1920x1080` and `1280x720 -> 3840x2160`.
Every capture uses 36 warmup frames and 24 scored frames. `matrix.json` is
authoritative: 4 candidates plus the baseline across 8 scene/resolution pairs
equals 40 captures.

For each candidate, unset all listed controls in `matrix.json`, then set only
`TFORGE_FSR4_LEARNED_STRENGTH`. Keep Quality Lab disabled and do not add CAS,
RCAS, filters, history, recurrent, jitter, or confidence overrides.

Each candidate must pass every pair: finite metrics, expected dimensions, SSIM
delta at least `-0.0005`, temporal-error increase no more than `2%`, and GPU
median increase no more than `0.25 ms`. It also needs at least three improved
pairs, nonnegative mean SSIM delta, and nonpositive mean temporal-error delta.
Numeric passage is not promotion: inspect matched stills and short strips for
softness, ringing, texture loss, ghosting, and flicker first.

No captures or source/default/image changes are part of this design.

## Measured decision

The three intermediate values all improved aggregate metrics but each had at
least one paired temporal-error failure, so none is promotable:

| value | mean SSIM delta | mean temporal-error delta | gate failures | improved pairs |
| --- | ---: | ---: | ---: | ---: |
| `0.40` | +0.000329250 | -0.059366250 | 2 | 3 |
| `0.45` | +0.000302750 | -0.074104625 | 1 | 3 |
| `0.50` | +0.000258625 | -0.089151250 | 1 | 3 |

The failing pair is the `tos_debris` low-resolution case for all three values.
The complete machine-readable result is in `measured_results.json`.
