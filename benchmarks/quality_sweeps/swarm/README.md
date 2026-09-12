# Quality-campaign swarm tools

`run_learned_history_matrix.py` automates the bounded learned/history/jitter/
confidence campaign. It uses fresh player processes and fresh artifact
namespaces, strips inherited `TFORGE_*` variables, retains timing logs, and
compares every arm against a same-scene `current_default` capture.

The matrix has 12 valid arms per input resolution:

- conservative/stronger learned strength
- jitter off/on
- history off, or history on with confidence 0/0.75

Numerical passes remain `review_required`; visual review is still required for
promotion. The runner returns non-zero when no arm passes all numeric gates.

Example:

```bash
python3 benchmarks/quality_sweeps/swarm/run_learned_history_matrix.py \
  --output-root /tmp/tforge-history-matrix-smoke \
  --scene tos_daylight \
  --input-resolution 426x240 \
  --warmup 36 \
  --frames 24
```

The 8-frame real-GPU smoke on `tos_daylight`/`426x240` completed all 12 arms
with zero numeric passes. That is expected campaign evidence, not a claim
that the quality problem is solved.
