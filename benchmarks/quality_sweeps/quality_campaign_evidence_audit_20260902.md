# Quality campaign evidence audit

Root: `benchmarks/quality_sweeps`

Image payloads are not required; classifications use durable data products.

| Method | Classification |
|---|---|
| `current_cas20` | RECAPTURE REQUIRED |
| `base_only_bilinear_cas20` | RECAPTURE REQUIRED |
| `fsr_direct_cas20` | RECAPTURE REQUIRED |
| `fsr_200x_downsample_cas20_pre` | RECAPTURE REQUIRED |
| `fsr_200x_downsample_cas20_post` | RECAPTURE REQUIRED |
| `fsr_200x_downsample_no_cas` | RECAPTURE REQUIRED |
| `fsr_225x_downsample_cas20_pre` | RECAPTURE REQUIRED |
| `fsr_225x_downsample_cas20_post` | RECAPTURE REQUIRED |
| `fsr_225x_downsample_no_cas` | RECAPTURE REQUIRED |
| `fsr_250x_downsample_cas20_pre` | RECAPTURE REQUIRED |
| `fsr_250x_downsample_cas20_post` | RECAPTURE REQUIRED |
| `fsr_250x_downsample_no_cas` | RECAPTURE REQUIRED |
| `fsr_275x_downsample_cas20_pre` | RECAPTURE REQUIRED |
| `fsr_275x_downsample_cas20_post` | RECAPTURE REQUIRED |
| `fsr_275x_downsample_no_cas` | RECAPTURE REQUIRED |
| `fsr_300x_downsample_cas20_pre` | RECAPTURE REQUIRED |
| `fsr_300x_downsample_cas20_post` | RECAPTURE REQUIRED |
| `fsr_300x_downsample_no_cas` | RECAPTURE REQUIRED |
| `fsr_nativeaa_downsample_cas20_pre` | RECAPTURE REQUIRED |
| `fsr_nativeaa_downsample_cas20_post` | RECAPTURE REQUIRED |
| `fsr_nativeaa_downsample_no_cas` | RECAPTURE REQUIRED |
| `conventional_lanczos` | RECAPTURE REQUIRED |
| `conventional_bicubic` | RECAPTURE REQUIRED |

## Interim recapture finding — 2026-09-02

The data-only recovery run currently writes evidence under
`quality_campaign_data_only_20260902/`. The three completed 360p-delivery
pairs pass the runner's existing resume validator, but that is not sufficient
for campaign validity.

The persisted runtime traces show that 640x360 input is clamped to a
640x360 reconstruction grid for every requested scale. For example, the
360→1080 pair records requested model resolution `960x540` while the effective
reconstruction remains `640x360`; the effective delivery/reconstruction ratios
are therefore 3.00, 3.375, 3.75, 4.125, and 4.50 rather than the requested
2.00–3.00 reconstruction scales. The 360→1440 pair shows the same defect with
effective ratios 4.00–6.00. These are source-clamped delivery captures, not
independent 2.25x–3.00x reconstruction arms, and remain `RECAPTURE REQUIRED`
until the campaign contract either represents that state explicitly or the
runner obtains genuinely distinct reconstruction resolutions.

The traces also report `integrated_post_reconstruction` for the internal CAS
arm and `none` for the external-post and no-CAS arms. The current record does
not prove the complete end-to-end CAS identity for the external arm, and the
validator does not yet reject a requested/effective CAS-stage mismatch.
Consequently, marker validation is treated as evidence that files are
structurally present—not as proof that the requested experimental matrix ran.

The same completed pairs also contain more than one recorded Git head while
sharing the same binary SHA, and all records report a dirty worktree. This
means the live run crossed repository-state changes and cannot establish one
authoritative source/build identity for the pair. Those rows remain historical
diagnostic evidence only; they must not be promoted or used to freeze the next
baseline.
