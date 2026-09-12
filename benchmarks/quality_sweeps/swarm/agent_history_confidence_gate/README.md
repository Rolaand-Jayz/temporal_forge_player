# History-confidence gate candidate

This candidate adds an opt-in threshold to the prepass display-color history
coverage decision. It is intended to reject stale color history when codec
motion is structurally present but the frame-level motion confidence is low.
The normal path is unchanged unless `TFORGE_FSR4_HISTORY_CONFIDENCE_THRESHOLD`
is set.

The candidate was tested with software decode, color history enabled, and
learned strength `0.15` on real 426x240 high-quality clips. It was not
promoted: the debris result improved slightly, but the cave result was neutral
at ordinary thresholds and slightly worse when forced to threshold `1.0`.
