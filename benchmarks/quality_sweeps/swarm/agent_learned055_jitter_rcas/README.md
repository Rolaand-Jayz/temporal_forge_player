# Learned .55 + jitter 1.0 + legacy RCAS

This bounded matrix follows the learned `.55`/controlled-jitter `1.0`
candidate. That combination passed numeric gates and matched timing but was
visibly soft, so this sweep tests only small legacy-RCAS additions.

It uses the four real scenes, both input resolutions, 36 warmup frames, and 24
scored frames. Candidates are paired against the current default at identical
scene and resolution; RCAS `.12` and `.16` are also compared with the `.08`
learned/jitter control. Every pair must pass the SSIM, temporal-error, timing,
and human-review gates in `matrix.json`. No result may change defaults.

## Measured result

All 24 candidate captures completed. RCAS `.08` is the existing learned/
jitter control and remains visually soft. RCAS `.12` fails one temporal pair
(`tos_debris 1280x720`, `+11.5%`); `.16` fails two (`tos_debris 1280x720` and
`sintel_rooftop 1280x720`, up to `+22.9%`). No candidate is promoted. See
`measured_results.json` and `captures/`.
