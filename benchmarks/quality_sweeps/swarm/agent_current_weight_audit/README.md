# Current-frame contribution audit

This audit compares the existing learned/history composition with the same
path plus `TFORGE_FSR4_POSTPASS_CURRENT_WEIGHT=0.10`. The setting mixes a small
amount of the current model resolve before the recovered learned/history
decode.

The matched matrix used software decode, color history, learned strength
`0.55`, eight warmup frames, and twelve scored frames across the four real
scenes at 426x240 and 1280x720 input. Synthetic families were excluded.

## Decision

Not promoted. The candidate stays within the per-pair SSIM and temporal gates,
but its mean temporal absolute error is worse by approximately `0.001183`
across the eight pairs. A saved four-frame daylight pair is available under
`../agent_current_weight_review/`; direct visual inspection found no meaningful
normal-scale difference.

