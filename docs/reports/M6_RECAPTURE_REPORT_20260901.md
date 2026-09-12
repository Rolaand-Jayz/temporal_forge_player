# M6 campaign recapture — 2026-09-01

This recapture covers the campaign-only M6 spatial evidence. The 17-pair
portable review-harness matrix was not used and its existing images were not
rewritten.

## Scope and provenance

- Manifest: `benchmarks/quality_sweeps/m6_schema2_spatial_campaign.json`
- Evidence mode: metrics-only
- Candidates: `current`, `base_only_bilinear`, `base_only_mitchell`,
  `base_only_catmull_rom`, and `base_only_lanczos2`
- Corpus: Tears of Steel daylight/debris and Sintel rooftop/cave
- Dimensions: 426x240 source to 1920x1080 output
- Rows: 5 candidates × 4 scenes = 20 spatial rows
- Final artifacts:
  `benchmarks/quality_sweeps/m6_recapture_20260901_final/`
- Guard log:
  `benchmarks/quality_sweeps/m6_recapture_20260901_final/game_guard_events.jsonl`
- Executed commit: `0d36015e817e36526d42115a2e0f46a8eadede5a`
- Player SHA-256:
  `1b8efb8068d2034df0b7547f02c91c35a89f78f829e5f65c423fccb0ec32c742`

The campaign driver ran candidates serially. The guard recorded five command
starts and five completions, with no game pause required during this run. It
does not terminate user processes. Existing harness image count remained
1,202.

## Fresh spatial means

| candidate | PSNR (dB) | SSIM | edge SSIM | low-frequency luma MAE | luma bias |
|---|---:|---:|---:|---:|---:|
| current | 26.775942 | 0.566589 | 0.571939 | 0.020850 | 0.003710 |
| base_only_bilinear | 27.100806 | 0.597513 | 0.579592 | 0.021011 | 0.005217 |
| base_only_mitchell | 27.053566 | 0.597504 | 0.580251 | 0.021120 | 0.005396 |
| base_only_catmull_rom | 26.796235 | 0.590715 | 0.575955 | 0.021503 | 0.005904 |
| base_only_lanczos2 | 26.790253 | 0.590514 | 0.576599 | 0.021511 | 0.005915 |

The fresh spatial data again favors `base_only_bilinear` on aggregate PSNR and
SSIM among these five candidates. This is a metrics result, not a visual
claim. Existing temporal M6 evidence remains the retained campaign temporal
dataset; it was not rerun because this recapture is limited to missing
campaign evidence and does not include review-harness captures.
