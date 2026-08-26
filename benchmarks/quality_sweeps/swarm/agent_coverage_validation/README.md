# Motion coverage validation

The runtime now carries codec-block coverage separately from the RG16F motion
vector. A covered static block is valid `(0, 0)` motion; an uncovered pixel is
invalid and falls back to the current resolve instead of sampling same-pixel
history.

The matched evidence is in `measured_results.json`. It used the same binary,
real-world 1280x720 high-quality clips, 1920x1080 output, 36 warmup frames, and
24 scored frames. The four-scene aggregate reduced temporal delta error by
about 1.9% when color history was explicitly enabled, but one scene regressed
and mean SSIM dipped slightly. Therefore the coverage fix stays in the normal
path as a correctness fix, while color-history enablement remains opt-in.

For a legacy A/B control, set
`TFORGE_FSR4_DISABLE_MOTION_VALIDITY=1`. This variable is benchmark-only and
must not be used as a user-facing quality setting.
