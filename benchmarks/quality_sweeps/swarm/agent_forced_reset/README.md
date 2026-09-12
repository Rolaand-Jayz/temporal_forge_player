# Forced-reset diagnostic

The real-corpus smoke compared the current control with
`TFORGE_FSR4_FORCE_RESET=1`, keeping all other capture settings unchanged.

```text
426x240 -> 1920x1080: SSIM delta +0.000000, temporal change +0.000005
1280x720 -> 3840x2160: SSIM delta +0.000000, temporal change +0.000000
```

Forced reset is not a promising quality lever on this smoke. It remains
untouched and unpromoted.
