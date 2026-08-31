# NativeAA true-downscaling probe — 2026-08-31

This is a matched 1920x1080-source to 1280x720-delivery probe. The FSR arm
used `NativeAA` at `TFORGE_FSR4_FORCE_SCALE=1.00`,
`TFORGE_FSR4_FORCE_VIEWPORT=1280x720`, CAS enabled at
`TFORGE_FSR4_CAS_STRENGTH=0.20`, jitter off, software decode, and the
`agent_composition_audit/current_control.json` Quality Lab config. The runner
generated conventional Lanczos and bicubic controls from the same
source/reference pair. `nativeaa.csv` retains the spatial rows; the four
`temporal/*/nativeaa.csv` files retain 12-frame temporal rows after 12 warmup
frames at source cadence.

Provenance:

- player SHA-256: `1b8efb8068d2034df0b7547f02c91c35a89f78f829e5f65c423fccb0ec32c742`
- daylight input SHA-256: `ef8d0e14bc125e3fa4c7ee5e7e711387be3d726de7f9bb74171283c672785562`
- daylight lossless reference SHA-256: `815512666c0c39a8bbd6dfc393bcb018dd4653eddd566e3e9406688f5e2987ac`
- spatial frame: 48; temporal protocol: 12 warmup + 12 scored frames

Result: NativeAA did not beat either conventional reducer on spatial SSIM or
temporal SSIM/error for any of the four real scenes in this slice. This is a
scale-specific rejection, not a claim about every possible downscaling mode.
Large transient PPM/MKV payloads are retained under `/tmp/tforge-downscale-
nativeaa-20260831-artifacts`; the campaign's data-only policy does not require
committing those image payloads.
