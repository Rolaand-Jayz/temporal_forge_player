# Portable harness campaign

The complete three-scene campaign uses Tears of Steel daylight, Sintel
rooftop, and Sintel cave. It is driven by one resumable serial command:

```bash
python3 benchmarks/quality_sweeps/run_harness_campaign.py \
  --player build/temporal_forge_player --resume
```

It captures the pre-CAS, post-CAS, no-CAS, and NativeAA arms through the
player, creates the conventional and bilinear controls from the matched
decoded source frame, exports canonical filenames, and writes a completion
marker only after all 23 methods exist for all three scenes in that resolution
pair. The 540p rows use the derived fixture manifest.

The command is intentionally serial. If Trackmania is running, the existing
guard prevents parallel capture. Re-running with `--resume` skips only pairs
whose completion marker has passed the complete asset check.
