# Learned .55 + jitter 1.0 + post-CAS

This matrix tests whether a small post-reconstruction CAS pass restores the
softness observed in learned `.55` + controlled jitter `1.0`. It tests CAS
`.02`, `.04`, and `.08` against a learned/jitter/CAS-0 control and the current
default. It uses only the four real scenes, both resolutions, 36 warmup and 24
scored frames. Every paired cell must pass the strict gates in `matrix.json`,
then timing and human visual review. No result changes defaults.

## Measured result

All 32 captures completed. CAS `.02`, `.04`, and `.08` each fail two
high-resolution temporal pairs, with maximum temporal-error regressions of
`48.3%`, `129.3%`, and `312.4%`. CAS `0` is the same soft,
visual-review-blocked candidate. Nothing from this matrix is promoted. See
`measured_results.json` and `captures/`.
