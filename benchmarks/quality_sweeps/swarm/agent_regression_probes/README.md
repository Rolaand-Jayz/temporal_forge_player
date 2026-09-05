# Dirty-tree regression probes

These probes compare the current dirty-tree defaults against isolated legacy
behaviors on a forced generic path. They are opt-in diagnostics only.

- Clip: `tos_daylight_426x240_high_crf12.mp4`
- Output: `1918x1080` generic path
- Color history enabled; warmup 8; scored frames 8

```text
control:              ssim 0.858291  temporal_abs_error 1.400845
legacy round:         ssim 0.858291  temporal_abs_error 1.400847
legacy recurrent:     ssim 0.858291  temporal_abs_error 1.400845
disable validity:     ssim 0.858837  temporal_abs_error 1.475668
```

The round and recurrent-bias changes are inert in this probe. Disabling
motion-validity improves SSIM by `+0.000546` but worsens absolute temporal
error by about `5.3%`, so it is not promotable alone.

A combined disable-validity + single-history-blend + confidence-0.75 run
matched the single-history-blend candidate rather than repairing its temporal
gate. No default was changed.
