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
longer sequences. A disk-backed multi-frame completion was then run with
`--frames 8 --warmup 8`. Its manifest is
`.campaign_motion_multiframe/manifest.jsonl`; all 288 keys are complete, with
eight captured output frames, dense RG16F motion, R8 validity, codec-motion
records, reprojection dumps, runtime metadata, and quality CSVs per key. The
offline arm has 48 dense-flow validation reports (four tiers × four scenes ×
three confidence settings), each validating seven frame pairs.

The multi-frame SSIM and temporal-error means (12 captures per tier/arm,
covering four scenes and three confidence settings) are:

| source tier | arm | FSR SSIM | SSIM minimum | temporal delta absolute error |
|---|---|---:|---:|---:|
| 426×240 | zero | 0.754441 | 0.712234 | 0.760019 |
| 426×240 | AutoCheap | 0.754409 | 0.712300 | 0.775360 |
| 426×240 | codec | 0.754369 | 0.712288 | 0.774273 |
| 426×240 | refined | 0.754410 | 0.712300 | 0.775382 |
| 426×240 | dense+edge | 0.754390 | 0.712197 | 0.776349 |
| 426×240 | offline dense | 0.754472 | 0.713046 | 0.800721 |
| 640×360 | zero | 0.805643 | 0.765999 | 0.504961 |
| 640×360 | AutoCheap | 0.805525 | 0.765988 | 0.522923 |
| 640×360 | codec | 0.805510 | 0.765992 | 0.520861 |
| 640×360 | refined | 0.805528 | 0.766006 | 0.522931 |
| 640×360 | dense+edge | 0.805517 | 0.765995 | 0.523771 |
| 640×360 | offline dense | 0.805413 | 0.766015 | 0.545965 |
| 1280×720 | zero | 0.919968 | 0.901165 | 0.187077 |
| 1280×720 | AutoCheap | 0.919877 | 0.901105 | 0.204155 |
| 1280×720 | codec | 0.919876 | 0.901140 | 0.202732 |
| 1280×720 | refined | 0.919878 | 0.901106 | 0.204152 |
| 1280×720 | dense+edge | 0.919877 | 0.901100 | 0.205821 |
| 1280×720 | offline dense | 0.920102 | 0.901176 | 0.250720 |
| 1920×1080 | zero | 0.957878 | 0.954765 | 0.244372 |
| 1920×1080 | AutoCheap | 0.957734 | 0.954568 | 0.258820 |
| 1920×1080 | codec | 0.957741 | 0.954570 | 0.257969 |
| 1920×1080 | refined | 0.957733 | 0.954566 | 0.258817 |
| 1920×1080 | dense+edge | 0.957695 | 0.954515 | 0.261558 |
| 1920×1080 | offline dense | 0.957550 | 0.954765 | 0.295964 |

The result is consistent across the full matrix: zero motion is the best or
near-best control at three tiers, while offline flow is only a small SSIM win
at 1280×720 and has worse temporal-delta error there. Better correspondence
therefore does not provide repeatable headroom, and the data does not justify a
heavier real-time estimator or a confidence-threshold change. The dominant
resolution effect is source-detail loss (the absolute SSIM falls sharply at
240p/360p); the arm ordering itself does not reverse into a motion win.

The derived evidence pass in `.campaign_motion_derived/` supplied the optional
event-trace destination for every key without retaining another copy of the
large image/video payloads. It contains 288 valid
`temporal_forge.event_trace.v1` documents (each with eight frame records), 288
quality CSVs, and the same 48 offline-flow reports. The event records expose
per-frame detector inputs, motion confidence, reset cause, scene-cut decision,
and threshold provenance; the runner environment records the confidence sweep
and reconstruction configuration. The current event schema does not emit a
separate FSR history-gate pass/fail bit or per-pixel coverage field, so those
remain an instrumentation gap rather than inferred results. The original
image-rich capture tree remains unchanged for pixel inspection.

### Gate-trace completion

The follow-up minimal-trace pass in `.campaign_gate_trace/manifest.jsonl` is
complete: 288/288 keys succeeded, with 288 event traces, 288 quality CSVs, and
288 player logs containing the FSR dispatch/postpass diagnostics. It retained
no duplicate image, motion-texture, sidecar, or reprojection payloads. The
postpass records expose effective history confidence and the fixed
`currentWeight=0.02000`; they do not claim a gate decision that the runtime
does not serialize.

Across all four source tiers, the logged effective-confidence distributions
show the controlled sweep behaving as configured: the fraction below the
selected threshold is approximately 31–35% at the default threshold, 2–3% at
0.35, and 52–65% at 0.75. This is direct dispatch evidence, not a substitute
for the missing explicit gate-state and per-pixel-coverage fields. The run
consumed 4.7G and completed with approximately 37G free on the capture
filesystem; the two earlier evidence trees were not modified.

For offline analysis, the frame-level admission state is reproducibly derived
from each dispatch record as `historyConfidence >= threshold` (with the
threshold taken from the manifest's confidence selector and the default
profile's documented 0.55). This reconstructs the scalar decision that the
prepass shader applies; it is intentionally labeled derived because the
runtime log does not emit a boolean. The captured R8 validity planes remain
the authoritative per-pixel coverage evidence (2,284 planes in the
image-bearing run), while no new payloads were created for this summary.

The trace schema is now strengthened for subsequent diagnostic runs: when
`TFORGE_FSR4_DISPATCH_TRACE` is enabled, the temporal dispatch line also emits
`historyGateEnabled`, `historyGatePass`, and `threshold`. This is a
trace-only observability change; it does not change the prepass constants or
the reconstruction path, and the completed campaign artifacts remain the
preserved source of results.
