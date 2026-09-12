# Single-history-blend diagnostic

This opt-in branch tests a specific composition hypothesis: the prepass writes
a current/history resolve to `u_reprojectedColor`, then the postpass blends
that resolve with the learned reconstruction again. The diagnostic bypasses
only that second learned-output blend; it does not change the default path.

## Implementation

Set `TFORGE_FSR4_EXPERIMENTAL_SINGLE_HISTORY_BLEND=1`. The host sets bit 1024
in the postpass flags, and the shader uses the learned reconstruction directly
for `modelColor` while still allowing the prepass resolve to condition the
network features and history output.

The capture runners forward this variable, along with the existing generic
blob and trace controls.

## Initial real-corpus result

- Clip: `tos_daylight_426x240_high_crf12.mp4`
- Forced generic output: `1918x1080`
- Color history: enabled
- Warmup: 8; scored frames: 4

```text
control: ssim=0.859011 min=0.854980 temporal_abs_error=1.306553
candidate: ssim=0.859441 min=0.855390 temporal_abs_error=1.365350
```

The candidate improved SSIM by `+0.000430` and improved the signed temporal
delta error, but worsened absolute temporal error by about `4.5%`, exceeding
the campaign's `2%` per-pair gate. It is therefore a lead for a confidence or
jitter interaction, not a promoted configuration.

Status: **promising but gate-failing diagnostic**.

## Confidence/jitter follow-up

With controlled jitter `1.0`, color history enabled, and confidence blend
`0.75`, an 8-frame smoke produced:

```text
control:   ssim=0.858291 temporal_abs_error=1.400845
candidate: ssim=0.858767 temporal_abs_error=1.444744
```

This preserved the SSIM gain (`+0.000476`) and reduced the temporal penalty,
but still exceeded the `2%` per-pair temporal gate by about `3.1%`. It remains
unpromoted and needs either a better temporal confidence policy or rejection.
