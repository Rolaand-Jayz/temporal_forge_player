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
