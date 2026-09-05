# Native base-filter probe

This probe compares the native INT8 Temporal Forge path with its default
Catmull-Rom base reconstruction and an opt-in bilinear base reconstruction.
The purpose is to separate single-frame sharpness from temporal stability
before changing the default path.

## Scope

- Real-world corpus only: Tears of Steel daylight and debris, Sintel rooftop,
  and Sintel cave.
- Native INT8 graph enabled.
- 426x240 input to the intended 1920x1080 output.
- Twelve warm-up frames and twelve scored frames.
- Default and bilinear base filters were captured with and without color
  history; the bilinear follow-ups also tested learned strength 0.0/0.05 and
  legacy RCAS strength 0.0.

## Result

Bilinear raises spatial SSIM on all four scenes, but increases temporal error
on all four scenes. The largest regressions are rooftop and debris. Enabling
color history, reducing learned strength, or disabling legacy RCAS does not
remove that temporal penalty. This is therefore not a safe quality promotion.

The default native Catmull-Rom path remains unchanged. These captures are
evidence for the next investigation, not a new production setting.

See `measured_results.json` for the compact measured summary and the capture
CSV files for the full per-run metrics.
