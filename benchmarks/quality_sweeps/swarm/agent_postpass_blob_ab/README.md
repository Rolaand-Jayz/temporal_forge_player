# Generic postpass parameter A/B

This is a diagnostic-only capture record. It does not promote a quality
configuration and does not modify the original FSR initializer or player
defaults.

## Method

- Real corpus clip: `tos_daylight_426x240_high_crf12.mp4`
- Forced output: `1918x1080`, which bypasses the fixed native INT8 shape
- Generic blob: copied to `/tmp/tforge-postpass-ab` from the original
  `quality.bin`; only one FP32 parameter was changed per variant
- Warmup: 4 frames; measured sequence: 2 frames
- Trace enabled: `TFORGE_FSR4_POSTPASS_TRACE=1`
- Variants: parameter indices `0`, `214`, and `218`

## Result

All three variants produced the same temporal metrics and RGB output as the
control:

```text
fsr,2,1918,1080,0.856471,0.851102,0.853409,0.848443,1.052550,2.854720,1.208750,1.802170,1.645970,1.007470,0.045080
```

The capture PPM path is RGB-only, so this does not establish whether the
trace-only values changed output alpha. It does establish that none of these
mutations changed the visible RGB result in the current generic path.

Status: **captured diagnostic; not a quality candidate**.
