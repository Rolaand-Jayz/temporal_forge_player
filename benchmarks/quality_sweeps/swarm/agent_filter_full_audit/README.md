# Full-composition base-filter audit

This follow-up tests the base filter where it actually participates in the
current FSR composition. Unlike the earlier diagnostic sweep, composition is
`current` with learned strength 0.55 and residual strength 1.0. Sharpening,
tone, and presentation remain neutral/default. Only `baseFilter.mode` varies.

The matrix is 4 real scenes × 2 input resolutions × 4 filters, with 36 warmup
frames and 24 scored frames. Expected outputs are 1920x1080 from 426x240 and
3840x2160 from 1280x720. Sparse motion validity stays enabled by leaving its
disable switch unset.

The JSON files are passed through `TFORGE_QUALITY_LAB_CONFIG`; `filter_matrix.json`
is the authoritative manifest. The first 32 captures were intentionally
retained as pre-wiring evidence. After the guarded current-composition wiring
fix, a clean second set of 32 captures produced distinct filter results. No
filter passed both spatial and temporal gates; Catmull-Rom remains the default.
