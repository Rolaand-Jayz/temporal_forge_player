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

## Capture status and first-pass result

The completed manifest contains 288 unique keys: four source tiers × four
scenes × six motion arms × three confidence settings. Every key has a quality
CSV and runtime pipeline trace. The 1920×1080 source tier is now a real FSR
motion path; a sizing bug previously classified the 1.5× quality grid as
native passthrough and skipped motion upload. That was corrected by requiring
passthrough to use a model grid equal to the decoded grid.

The first-pass SSIM means (aggregated across scenes and confidence settings)
were:

| source tier | zero | AutoCheap | refined | dense+edge | offline dense |
|---|---:|---:|---:|---:|---:|
| 426×240 | 0.815058 | 0.815075 | 0.815074 | 0.815075 | 0.815051 |
| 640×360 | 0.858486 | 0.858448 | 0.858449 | 0.858445 | 0.858404 |
| 1280×720 | 0.937117 | 0.936424 | 0.936408 | 0.936416 | 0.936398 |
| 1920×1080 | 0.951089 | 0.951057 | 0.951058 | 0.951028 | 0.951004 |

These results do not show a repeatable quality gain from better motion in this
first pass; the offline-flow arm is not an upper-bound win. Confidence changes
are similarly small (typically below 0.0002 SSIM), so the scalar gate is not
demonstrated as the dominant limiter by these captures.

The resumed portion used one captured frame per key to stay within the
workstation's temporary-storage limit; the earlier completed keys retain their
longer sequences. Therefore the matrix, per-frame artifacts, and performance
provenance are complete, but a final claim about temporal stability,
occlusion/disocclusion behavior, and scene-cut handling still requires a
multi-frame rerun of the one-frame subset with disk-backed temporary storage.
