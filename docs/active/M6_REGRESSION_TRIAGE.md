# M6 current-path regression triage

Status: **open; no renderer change made**

## Evidence

The provenance-valid spatial campaign is:

`.m6-captures/m6-spatial-parallel-20260823-v6/`

At 426x240 -> 1920x1080, its high-quality rows report these mean SSIM
values:

| candidate | mean SSIM | mean Lanczos SSIM |
| --- | ---: | ---: |
| current | 0.53526875 | 0.63490250 |
| base-only bilinear | 0.59497525 | 0.63490250 |
| base-only Mitchell | 0.59492700 | 0.63490250 |
| base-only Lanczos2 | 0.58589800 | 0.63490250 |

The current result is therefore a real spatial regression, not a capture
failure. The full five-candidate run completed with matching binary/config
provenance and no shared-reference errors.

A diagnostic run with the preserved `build-fast/temporal_forge_player` binary
and the same current configuration produced high-quality SSIM values of
0.698817 (Tears of Steel daylight), 0.823729 (Tears of Steel debris),
0.625149 (Sintel rooftop), and 0.528879 (Sintel cave). This is materially
different from the current build, but `build-fast` is not promoted as a clean
baseline because its exact source provenance is not established in this
working tree.

## Suspect boundary

The current working diff changes `shaders/fsr4/postpass_composite.comp` and
`src/render/Fsr4DispatchHarness.cpp` in image-affecting areas, including:

- postpass history/reprojection input and resource formats;
- replacement of postpass history reconstruction with `u_reprojectedColor`;
- CAS composition input handling; and
- postpass parameter plumbing.

These are hypotheses only. No change to reconstruction, shaders, model logic,
sharpening, tone, or motion behavior is authorized by this triage record.

## Gate decision

M6 remains open. M7 must not start. Any fix requires an explicit approved
slice, a test written first, and a fresh provenance-valid recapture against the
same controls.
