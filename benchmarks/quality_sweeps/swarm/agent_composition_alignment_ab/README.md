# Fair composition alignment A/B

This is a diagnostic-only real-corpus comparison. It does not change defaults
or reconstruction code.

## Method

- Real corpus clip: `tos_daylight_426x240_high_crf12.mp4`
- Output: `1920x1080`
- Warmup: 36 frames; measured sequence: 8 frames
- Base source forced unjittered
- CAS disabled
- Catmull-Rom base and bicubic presentation held constant
- Control: Quality Lab `base_only`, learned strength `0.0`
- Candidate: Quality Lab `direct_blend`, learned strength `0.05`

## Result

```text
base:         ssim=0.852937  min=0.847846  temporal=1.537177
direct .05:   ssim=0.852938  min=0.847846  temporal=1.537183
```

The difference is effectively zero for this low-resolution path (`ΔSSIM
0.000001`, temporal change about `+0.000006`). This does not justify a
promotion or a reconstruction change.

Status: **captured diagnostic; not a quality candidate**.
