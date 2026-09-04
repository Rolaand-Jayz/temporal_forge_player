# Motion-input campaign runner

`run_motion_campaign.py` is the reproducible harness for the full motion
evidence matrix. It covers the four source tiers (`426x240`, `640x360`,
`1280x720`, `1920x1080`) at a fixed `1920x1080` output, four representative
motion clips, six estimator arms, and default/strict confidence consumption.

It is dry-run by default. A run writes one isolated directory per
scene/tier/arm/confidence and appends a JSONL manifest containing input and
reference hashes, the exact command/environment selectors, runtime trace,
motion sidecars, validity maps, reprojection dumps, and quality metrics.

```sh
python3 benchmarks/quality_sweeps/run_motion_campaign.py --no-confidence
python3 benchmarks/quality_sweeps/run_motion_campaign.py --run \
  --output-root /path/to/motion_campaign
```

The `sintel_cave` 2160p reference is a known 458-byte invalid asset; the
runner refuses to pretend it is a reference and records the matching
`1920x1080_high_crf12` clip as an explicit fallback in the manifest.
