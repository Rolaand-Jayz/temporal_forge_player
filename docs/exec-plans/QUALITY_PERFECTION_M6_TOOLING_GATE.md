# Temporal Forge quality lab — M6 tooling gate

Status: **tooling, artifact, and real-browser harness checks passed** on
2026-08-23. M6 is not promoted: the real-world candidate/class quality matrix
is still incomplete.

## What was locked

The corrected campaign schema now has an explicit bridge to the existing corpus
runner:

- each candidate becomes its own isolated runner plan;
- a declared baseline candidate is paired against every other successful
  candidate using the exact clip/source/output/quality/CRF/frame tuple;
- source dimensions remain the runner selector;
- intended output dimensions become an explicit viewport setting;
- candidate config paths and real-world review assets remain attached;
- legacy 1280x720 fallback logic cannot overwrite an explicit viewport;
- campaign IDs are safe for artifact paths.

The bridge is exposed through `benchmarks/quality_sweeps/run_quality_campaign.py`.
It does not run captures unless explicitly invoked. The strict campaign verifier
`tools/verify_quality_campaign.py` consumes the schema-v2 matrix JSON (the same
`spatial` and `temporal` row arrays accepted by
`tools/verify_quality_matrix.py`). Its old `--metrics CANDIDATE=CSV` input is
legacy evidence and is rejected; a per-candidate CSV is not a class-attributed
matrix.

`tools/assemble_spatial_matrix.py` likewise requires each spatial CSV to carry
the producer's non-empty `class` field. It joins that captured class to the
campaign's selected class key and preserves it in `metricSource`; it does not
copy one scene-level row across multiple classes. Current corpus `quality.csv`
files do not have that field, so assembly fails with an explicit evidence gap
until class-attributed spatial captures exist.

The temporal capture runner now has a separate opt-in bridge to
`tools/measure_temporal_sequence.py`. Set `TFORGE_TEMPORAL_MOTION_JSON`,
`TFORGE_TEMPORAL_METRICS_CSV`, and `TFORGE_TEMPORAL_CLASS` to emit the new
per-class temporal table. Temporal measurement also requires an explicit
static-mask sidecar; event metadata is optional, and event-derived fields stay
blank when it is absent. Motion can be exported from the player and assembled
with `tools/assemble_motion_sidecar.py`. The historical
`run_temporal_quality.sh` frame-delta CSV is left unchanged.

The temporal matrix assembler preserves that distinction. Missing event fields
are recorded under `evidenceGaps` and `temporalEvidence` with status `pending`;
they are not converted into zeroes or treated as identity/provenance defects.
The complete matrix verifier remains strict: a matrix can be assembled for
audit, but it cannot be accepted while any required metric is unavailable.
Identity, dimension, sidecar-presence, and provenance drift remain blocking
`issues` and must not be hidden by the pending state.

The matrix identity contract is one row per
`candidateId/scene/qualityClass` coverage key. An event-spanning capture with
the same key has two legal representations: it may replace that key only when
the event-spanning sequence is the authoritative temporal row, or it may be
stored in `eventEvidence` as `scope=separate_event_evidence`. The latter keeps
the original eight-frame row unchanged, marks the event set as
`strictMatrixInput=false`, and never copies event metrics into the base row.
The event assembler validates the complete campaign key set rather than using
a row-count shortcut.

Captured config identity is retained verbatim. A capture that records the
candidate token (for example `base_only_mitchell`) while the campaign declares
the associated config file is resolved as
`candidate_id_matches_campaign_candidate`; it is not rewritten to the file
path. Any other disagreement remains a blocking provenance issue.

The review-harness contract is now part of this gate. The harness builder
discovers real-world assets into a structured manifest, exposes the same
selection pool to both sides of the draggable split view, excludes synthetic
families, and preserves valid native/reference/baseline/experimental metadata.
The embedded build deduplicates image payloads and can use lossless WebP
sidecars without modifying the source benchmark images. Its default size guard
is 512 MiB (`TFORGE_REVIEW_MAX_MIB`); an oversized temporary output is deleted
and the command fails before replacing the destination.

The safe M6.5 matrix slice now has a capture-free gap reporter at
`tools/report_quality_matrix_gaps.py`, with tests in
`tests/test_quality_matrix_gap_report.py`. It reads the legacy real spatial
manifest and explicitly supplied saved evidence; it does not launch a player,
capture frames, or write metric rows. The grounded spatial-only schemaVersion 2
campaign is `benchmarks/quality_sweeps/m6_schema2_spatial_campaign.json`; it
leaves temporal rows explicitly pending. The report generated from the current
saved evidence is `/tmp/tforge-m6-schema2-gap-report.json`.

The next evidence-unblock slice added
`benchmarks/quality_sweeps/m6_quality_class_annotations.json`, backed by
`tests/test_m6_quality_class_annotations.py`. It contains four visually
verified spatial annotations from existing real-world frame-48 stills:
faces/hair/skin and fine fabric texture in Tears of Steel, high-contrast
architecture in Sintel rooftop, and low-light shadow detail in Sintel cave.
Each entry records its exact asset path, frame, candidate, dimensions, and a
bounded static-region rectangle. The set explicitly excludes synthetic
families and declares temporal evidence unavailable; no motion vectors, event
boundaries, or static masks were inferred from stills.

## Gate evidence

- The M6 campaign contract, temporal metric, motion-sidecar, capture-export,
  runner, and review-harness contract tests passed.
- Full Python discovery passes: 106 tests.
- The runner now fails on swallowed player failures and zero-row success; the
  paired spatial gate rejects missing rows, duplicate keys, missing metrics,
  and changed source/output dimensions.
- Campaign and verifier entrypoints ran `--help` successfully as standalone
  executables.
- The M6.5 matrix verifier is contract-tested and requires complete
  candidate × scene × class coverage, exact spatial/temporal keys, matching
  dimensions, finite required metrics, and matching commit/binary/config
  provenance. Its standalone entrypoint is `tools/verify_quality_matrix.py`.
- The failing-first gap-report pack passes: the legacy manifest grounds five
  candidates, four scenes, `426x240 → 1920x1080`, and frame 48, but it grounds
  zero quality classes. With the saved result/CSV/sidecar evidence supplied,
  the report observes all five spatial candidates and twenty candidate/scene
  pairs, two temporal labels, and motion/static-mask files, while keeping
  `readyForSchemaVersion2=false` and listing 28 missing matrix/campaign keys.
  The temporal labels are not promoted to quality classes because their rows
  have no candidate, scene/class distinction, or candidate-linked provenance.
- The visually grounded annotation pack passes four focused tests. All four
  referenced paths exist under `results/quality_frames`, all are real-world
  frame-48 outputs, all optional rectangles are bounded within 1920x1080, and
  the manifest makes the missing adjacent-frame, candidate-attributed
  temporal, motion-sidecar, static-mask, and event evidence explicit.
- The temporal CLI requires explicit motion and static-mask sidecars and leaves
  event metrics blank when event metadata is absent; it never substitutes
  identity motion or guessed recovery.
- `build-fast` is clean, and CTest passes the runnable tests; the tensor-map
  test is skipped and `gpu_probe`, `cm_dump`, and `fsr4_harness_tests` remain
  disabled in this environment.
- Python compilation passed for all quality-sweep tools.
- Shell syntax checks passed for the video-corpus scripts.
- The real review-harness build contains 353 valid assets from 5 real-world
  scenes and no synthetic families. The standalone folder is
  `temporal-forge-frame55-review-standalone.html` plus its
  `temporal-forge-frame55-review-standalone-assets/` directory. The portable
  single-file artifact is `temporal-forge-frame55-review-single-file.html`.
  The current embedded build is 355.2 MiB, contains 353 asset aliases and 243
  unique PNG payloads, has an empty external asset list, and its
  embedded asset names exactly match the standalone manifest. The HTML's
  embedded JavaScript passed a `vm.Script` syntax check. A real Firefox
  Marionette pass loaded both artifacts, confirmed both mirrored selector
  structures and both image loads, changed LEFT without changing RIGHT,
  swept the divider, opened the split inspection view, exercised 1:1, zoom,
  fit-to-view, pan, and restored the URL hash. This is reproducible with
  `python3 tools/verify_review_harness_browser.py
  temporal-forge-frame55-review-standalone.html` and the same command with
  `temporal-forge-frame55-review-single-file.html`. The sandboxed browser
  launch still crashes before automation starts; the evidence was collected
  with the same installed Firefox outside that sandbox boundary. The runner is
  intentionally standard-library-only and reports an environmental browser
  failure as `blocked`, never as a pass.
- The corrected five-candidate spatial capture completed with 4 rows per
  candidate at 426x240 → 1920x1080. The paired report is generated at
  `/tmp/tforge-m6-2-paired-existing-20260823/paired_rankings.csv` with
  `base_only_bilinear` as the baseline. The current learned candidate is
  below that control on mean FSR SSIM delta (-0.03935225) and worst FSR SSIM
  delta (-0.05774700); this is preserved as evidence and does not promote a
  replacement.
- A real Tears of Steel daylight capture produced the aligned metric row:
  `static_flicker=0.002522044`, `edge_variance=0.000001913`, and
  `motion_compensated_error=0.016936318`. No event sidecar was supplied, so
  ghost and reset fields remain blank.
- A real Sintel cave scene-cut capture produced the aligned metric row:
  `static_flicker=0.010758430`, `edge_variance=0.000022168`,
  `motion_compensated_error=0.004305748`, `ghost_duration_frames=0`, and
  `reset_recovery_frames=0`.
- The bounded M6.3 capture-validation rerun used the existing H.264
  inputs for `base_only_bilinear`, `tos_daylight`, `fine-fabric-texture`, and
  frames 48–55. The wrapper returned 0 after producing eight 1920x1080 FSR
  frames, eight matched 1920x1080 reference frames, an identity-matched
  enhanced row (`static_flicker=0.021877969`,
  `edge_variance=0.000087407`,
  `motion_compensated_error=0.014157010`), and a causal motion sidecar with
  365/374/375/374/375/371/363 vectors on frames 1–7 after the reset frame.
  The complete retained artifact is
  `/tmp/tforge-m6-3-bilinear-fabric-rerun-20260823-escalated`.
- `git diff --check` passed.

## What this does not prove

The five-candidate spatial capture is not the required complete candidate/class
matrix. It proves that real spatial outputs can be captured and paired without
silently comparing different tuples, but it does not prove temporal quality or
justify a default promotion. The full M6 quality gate still requires complete
per-class spatial and temporal metric tables from real-world sequences,
including motion-compensated error, flicker, edge variance, ghost-duration,
and reset-recovery measurements. Those results must be generated and then
passed through the verifier before M6 can be promoted.

The browser gate is closed for the harness artifact. The current combined
assembly has 20 candidate-linked temporal rows, eight-frame 1920x1080
sequences, causal motion sidecars, and static-mask sidecars with zero blocking
identity, dimension, or provenance issues. All twenty base rows still lack the
explicit event metrics needed for strict completion, so the strict verifier
rejects the matrix on the first missing event value. The complete
candidate/class spatial and temporal metric matrix still has to be accepted by
the verifier. M7 remains locked.

## M6 event metadata reconciliation

Status: **event evidence accepted as a separate non-strict set** on
2026-08-23. Five candidate-attributed, 18-frame event-spanning rows for
`sintel_cave/low-light-shadow-detail` are retained under
`eventEvidence.scope=separate_event_evidence`.

The audit covered the twenty rows in the retained combined assembly
(`current`, `base_only_bilinear`, `base_only_mitchell`, `base_only_catmull_rom`,
and `base_only_lanczos2`, across the four real-world scene/class selections).
All twenty base rows have blank `ghost_duration_frames` and
`reset_recovery_frames`. The five accepted event rows have an event trace
joined to the candidate, scene, config, and 18-frame range, and remain separate
from the base rows. They do not populate the 8-frame rows, do not change the
strict matrix key set, and do not make the combined matrix complete.

The captured Mitchell rooftop CSV records `configId=base_only_mitchell`.
Because that raw token equals the candidate identity
`base_only_mitchell`, assembly records
`configIdentityResolution=candidate_id_matches_campaign_candidate` while
preserving the raw `capturedConfigId`; it does not relabel the capture to the
campaign file path. Other config disagreements remain blocking issues.

The inspected motion sidecars do not close that gap. Each retained eight-frame
capture reports `reset` only at frame 0 and frame 1; frame 0 has no previous
transition, and frame 1 is adjacent to the capture initialization. No later
transition is marked as a reset, and the exported record does not distinguish
a forced reset from a detector scene cut. The internal
`SideBufferSynth` detector and its runtime counter are not an event artifact.
Consequently, those fields cannot be relabeled as an authoritative scene-cut
boundary.

The temporary `/tmp/tforge-m6-sintel-cut-events-20260822.json` is also not
usable evidence: it has no candidate/scene/config/frame provenance, its
`analysisFrameIndices` values 6 through 11 are out of range for the retained
eight-frame sequences, and its `0.02` ghost/reset thresholds have no recorded
source. It remains unreferenced; no index or threshold is inferred from it.

The accepted traces retain the exact candidate/config/source/reference tuple,
pre-event and post-event frames, aligned event indices, detector inputs, and
versioned thresholds. Residual error peaks, still-image inspection, and the
existing motion `reset` field remain invalid substitutes for event metadata.

Additional event-spanning captures may replace a strict row only when their
sequence is explicitly declared authoritative for that same coverage key. If
they are additive, they must use the same separate evidence scope and leave
the base temporal row and strict verifier input unchanged.
