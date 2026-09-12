# Base-filter audit

This is the next isolated quality campaign after the rejected history/blend
matrix. It compares only the existing `qualityLab.baseFilter.mode` values.
Composition is fixed to `base_only`, learned and residual contributions are
zero, sharpening and tone are neutral, and presentation remains bicubic.

The matrix contains four real scenes, two input resolutions, and four filters:
bilinear, Mitchell, Catmull-Rom, and Lanczos2. Each capture uses 36 warmup
frames and 24 scored frames. The expected output sizes are 1920x1080 for
426x240 input and 3840x2160 for 1280x720 input.

`filter_matrix.json` is the authoritative capture manifest. The individual
JSON files are the `TFORGE_QUALITY_LAB_CONFIG` payloads. The 32 captures have
completed and are recorded in `measured_results.json`. No filter passed both
aggregate and per-cell temporal/spatial checks, so none was promoted. Human
review remains required for ringing, stair-stepping, and motion stability.

Before running, clear inherited temporal sidecar variables and set the filter
config per case. Keep the same `build-fast/temporal_forge_player`, corpus,
preset, warmup, and frame count for every case.
