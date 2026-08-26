# Motion-confidence reactive candidate

This candidate tests whether frame-level codec-motion confidence should raise
the reactive signal before the hard history reset threshold. It is opt-in via
`TFORGE_FSR4_MOTION_CONFIDENCE_REACTIVE=1`; the normal path is unchanged.

The policy was tested with software decode, color history, learned strength
`0.15`, and adaptive learned strength enabled. It produced a tiny regression
on the cave scene, so it is retained as a measured candidate but not promoted.
