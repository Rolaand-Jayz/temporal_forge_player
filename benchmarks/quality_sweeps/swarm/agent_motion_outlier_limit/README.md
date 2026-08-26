# Codec-motion outlier limit

The cave motion sidecar contained accepted vectors up to roughly 270 source
pixels, despite a frame-level confidence near 0.95. This candidate adds an
opt-in `TFORGE_FSR4_EXPERIMENTAL_MOTION_MAX_BLOCKS` multiplier to reject vectors
outside a configurable block-relative bound.

The limit-4 cave capture was effectively neutral, so it is not promoted. The
control remains available for future combinations and diagnostics; the normal
16-multiplier behavior is unchanged.
