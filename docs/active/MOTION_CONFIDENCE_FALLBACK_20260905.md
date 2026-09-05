# Motion confidence fallback correction

Date: 2026-09-05  
Branch: `quality-lab-vibecoder`

## Change

`MotionEstimator::aggregateConfidence` now returns its caller-provided
`emptyConfidence` when a nonempty motion list contains no valid, in-frame
coverage. Previously this path returned a hard-coded `0.25`, bypassing the
configured empty-motion policy and allowing malformed or entirely out-of-frame
metadata to receive an unintended confidence value.

The contract is covered by `motion_estimator_tests`: an out-of-frame entry with
an explicit fallback of `0.8` must return `0.8`.

## Validation

- Fresh build directory: `build-competition`, configured from this worktree.
- Binary SHA-256: `01fc924c620fc9d44102790795a21b947724b4ecd218e5a6a3ed158cb7ad4996`.
- Full CTest: 20 runnable tests passed; one test skipped and four tests are
  disabled by the project configuration.
- Real Temporal Forge capture: `.candidate_capture/empty_confidence_fix_640x360`.
- Input: `benchmarks/video_corpus/clips/tos_daylight_640x360_high_crf12.mp4`.
- Reference: `benchmarks/video_corpus/references/tos_daylight_2160p_lossless.mkv`.
- Output: 1920x1080, 8 captured frames, explicit current temporal config.
- Capture metrics: FSR SSIM mean `0.825759`, minimum `0.815313`, temporal
  delta absolute error `1.129259`; these match the valid-motion control.

The unchanged valid-motion result is intentional: this correction is a
fail-safe for invalid metadata and is not claimed as a spatial-quality win.
It preserves the configured policy for the malformed-input case while avoiding
a regression on the normal temporal path.

## Rejected alternatives

Resolution-specific learned-strength fallback, disabling the best-findings
bundle, disabling the photometric history gate, current-frame correction weight
sweeps, and motion-validation threshold sweeps were tested through the real
pipeline. None produced a consistent quality/stability improvement across the
available evidence, so none is promoted.
