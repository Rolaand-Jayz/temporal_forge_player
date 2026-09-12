# Opt-in block-motion fallback

These captures test the newly wired `TFORGE_FSR4_EXPERIMENTAL_BLOCK_MOTION=1`
fallback. It estimates sparse motion from the existing small analysis-luma
buffer when codec motion side data is empty, then sends those blocks through
the existing GPU motion expansion pass.

The first hardware-decode capture was intentionally discarded as invalid for
this candidate: hardware frames expose no CPU luma buffer, so the estimator
produced zero blocks. The software-decode A/B generated 336 blocks per frame,
but the native and generic outputs were effectively unchanged on daylight and
cave. It is therefore retained as an opt-in diagnostic, not promoted as a
quality fix. The production default remains codec motion when available and
zero motion otherwise.
