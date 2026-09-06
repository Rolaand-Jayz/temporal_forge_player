# Shared quality campaign and review harness capture

The quality campaign and review harness use one checked-in capture plan:
[`quality_campaign_capture_plan.json`](quality_campaign_capture_plan.json).
The plan covers four scenes and these eleven routes:

- 360p → 480p, 720p, 1080p
- 480p → 720p, 1080p, 1440p
- 720p → 1080p, 1440p, 2160p
- 1080p → 1440p, 2160p

There are no 540p inputs or outputs. Every completed route supplies both the
campaign evidence and the review harness; a separate harness capture is not
needed.

Preview the exact plan without launching the player:

```bash
python3 benchmarks/quality_sweeps/run_quality_campaign_capture.py
```

Start the resumable capture only with the explicit execution flag:

```bash
python3 benchmarks/quality_sweeps/run_quality_campaign_capture.py \
  --player build-fast/temporal_forge_player \
  --resume \
  --execute
```

For each downsampling test, the runner captures three independent arms: CAS
`0.20` inside the reconstruction before downsampling, renderer CAS disabled
with CAS `0.20` applied only after downsampling, and renderer CAS disabled with
no later sharpening. The same three-arm split is captured for NativeAA.

The runner captures those arms, then creates the conventional, bilinear,
bicubic, and Lanczos controls from the
same decoded source. NativeAA is captured once per route because it has no
multiplier sweep. Each route is published only after all 23 methods exist for
all four scenes and its hashes, dimensions, runtime identity, timings, and
configuration pass validation.

Renderer arms stay serial to protect reproducibility on the shared GPU. The
runner reuses a pair-local reference cache, exports images in-process, and
generates the three conventional controls in one ffmpeg pass per scene. It
observes game activity for provenance but never pauses or stops the capture or
user processes. Re-running with `--resume` skips only routes whose completion
marker passes the complete asset check.
