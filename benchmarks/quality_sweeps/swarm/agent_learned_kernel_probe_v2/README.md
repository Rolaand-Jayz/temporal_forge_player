# Learned postpass tail-mapping probe

Status: captured and numerically eligible for the generic reconstructed path,
not promoted.

This probe compared the existing postpass tail mapping with the opt-in
`TFORGE_FSR4_POSTPASS_TAIL_MAPPING=swap` mapping. The candidate swaps the
recovered output-bias and recurrent-bias groups before the learned decoder
filter consumes them. It does not change normal playback unless the
environment variable is explicitly set.

The first attempt in `../agent_learned_kernel_probe/captures` was invalid: I
called the player instead of `run_temporal_quality.sh`, so the player treated
the CSV path as another playlist item. It was stopped and is not part of the
results below. The corrected captures are under `captures/`.

## Capture conditions

- real corpus only: Tears of Steel daylight/debris and Sintel rooftop/cave
- generic reconstructed path (`TFORGE_FSR4_DISABLE_NATIVE_INT8=1`) for the
  candidate matrix
- learned strength `0.55`, color history enabled, software decode/motion
- 426x240 input -> 1920x1080 output for the initial matrix
- debris and rooftop were also checked at 1280x720 -> 3840x2160
- 8 warmup / 12 scored frames for the initial daylight/cave smoke
- 12 warmup / 24 scored frames for the debris/rooftop expansion
- default-path smoke was run separately without generic-path overrides; the
  tail mapping was effectively neutral there because the native path does not
  consume this generic postpass control.

## Result

The uniform eight-cell matrix improved SSIM in 6/8 pairs, stayed within the
per-pair SSIM floor in all 8, and reduced temporal absolute error in all 8.
The mean SSIM delta was positive. A preserved daylight frame pair was also
visually inspected; the outputs were effectively identical at normal viewing
scale, with a measured frame MAE of `0.000931611` and PSNR of `50.9518 dB`.

This is strong evidence that the existing generic postpass tail groups may be
mapped incorrectly, but it is not yet a promotion decision. The candidate is
neutral on the native INT8/default playback smoke because that path does not
consume this generic postpass control. Human review of additional scenes and
an explicit generic-vs-native policy decision are still required.

The default playback path was not changed.
