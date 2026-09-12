# Motion-aware display-base candidate

This candidate is an opt-in postpass composition probe. It starts from the
existing display-base strength and raises that contribution in source regions
with larger codec motion vectors, while leaving static regions on the existing
learned/history blend.

## Configuration

- `TFORGE_FSR4_DISPLAY_BASE_STRENGTH=0.5`
- `TFORGE_FSR4_MOTION_AWARE_DISPLAY_BASE=1`
- software decode, color history enabled
- learned strength `0.15`
- 8 warmup frames, 12 scored frames
- real 426x240 inputs, 1920x1080 output

The fixed comparison used the same settings without the motion-aware switch.

## Result

Rejected. Daylight gained only `+0.000059` SSIM but worsened temporal absolute
error by `+0.006133`. Cave was effectively neutral spatially and worsened
temporal absolute error by `+0.000039`. It is not promoted and remains
available only for controlled reproduction.

