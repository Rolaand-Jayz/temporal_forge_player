# Next combination sweep

This sweep tested only existing controls on four real scenes. History and
confidence blending can improve aggregate metrics, and the validity-off
ablation was worse than the validity-enabled candidate, but each candidate
regressed at least one scene. Nothing is promoted as a new default.

The best candidate was:

```text
TFORGE_FSR4_ENABLE_COLOR_HISTORY=1
TFORGE_FSR4_LEARNED_CONFIDENCE_BLEND=0.75
TFORGE_FSR4_LEARNED_STRENGTH=0.15
```

Keep the current jitter and base composition unchanged when comparing it. The
candidate is suitable for human review and future content-aware policy work,
not a global quality default yet.
