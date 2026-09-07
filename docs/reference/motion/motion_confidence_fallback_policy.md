# Motion confidence fallback policy

`MotionEstimator::aggregateConfidence` returns the caller-provided
`emptyConfidence` whenever the supplied motion list has no valid in-frame
coverage. This includes nonempty lists whose entries are entirely out of
frame, malformed, or non-finite.

This preserves the same policy used for an empty motion field. It prevents
invalid metadata from receiving the historical hard-coded confidence `0.25`
and unintentionally influencing temporal-history trust. Valid covered entries
retain the existing aggregation behavior.

The motion-estimator contract test covers an out-of-frame entry with an
explicit fallback of `0.8` and verifies that the configured value is returned.
This is a correctness and policy-consistency fix, not a spatial-quality
algorithm change.

## Runtime override and sanitization

The production caller (`codecMotionConfidence` in PlaybackEngine) obtains the
fallback from `MotionEstimator::emptyMotionConfidenceFromEnvironment()`, a
single parse/sanitize point shared by the empty-field early return and
`aggregateConfidence`. The value is controlled by
`TFORGE_FSR4_EXPERIMENTAL_EMPTY_MOTION_CONFIDENCE` with a documented default
of `0.5`.

Malformed strings and non-finite values (`nan`, `inf`, `-inf`, ...) are
rejected to the `0.5` default before any clamping. This ordering is required:
`std::clamp` propagates NaN unchanged and saturates infinities to `1.0`/`0.0`,
which would let a malformed override grant or destroy full temporal-history
trust on the empty-motion early-return path that does not enter
`aggregateConfidence`. Finite out-of-range values clamp to `[0, 1]`.
