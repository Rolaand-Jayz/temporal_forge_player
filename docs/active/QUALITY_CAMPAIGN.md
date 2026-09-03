# Active quality campaign

**Status:** READY FOR CAPTURE — NOT STARTED

**As of:** 2026-09-02

**Scope:** quality campaign evidence completion and shared review harness

## Objective

Complete the campaign-required captures and measurements with truthful method
identity, resolution coverage, runtime provenance, and reproducible artifacts.
The same validated images supply the campaign and the review harness; there is
no separate harness capture.

## Current gates

1. Use the current committed player and record its binary/source identity.
2. Use the checked-in
   [`quality_campaign_capture_plan.json`](../../benchmarks/quality_sweeps/quality_campaign_capture_plan.json)
   as the sole resolution and scene authority. It defines four campaign scenes
   and these eleven routes:
   - 360p → 480p, 720p, 1080p
   - 480p → 720p, 1080p, 1440p
   - 720p → 1080p, 1440p, 2160p
   - 1080p → 1440p, 2160p
3. Do not capture or publish 540p inputs or outputs. Every planned route must be
   tagged for both `quality_campaign` and `review_harness` use.
4. Capture three independent downsampling arms: CAS `0.20` before reduction,
   CAS `0.20` only after reduction, and no CAS sharpening. Renderer CAS must be
   disabled in both latter arms, and all three require distinct experiment IDs.
5. Retain metrics, timing, runtime traces, dimensions, timestamps, configuration,
   hashes, and failure/guard events. Image payloads are required for the shared
   campaign/harness routes.
6. Validate every completed route against the manifest before calling the
   campaign complete. A missing asset, ambiguous label, mismatched timestamp,
   wrong dimension, or wrong binary invalidates that route.

## Prepared execution

The entry point is
[`run_quality_campaign_capture.py`](../../benchmarks/quality_sweeps/run_quality_campaign_capture.py).
Its default behavior is plan-only and cannot launch the player. Actual capture
requires both `--execute` and a clean tracked worktree, and remains serial across
renderer arms. The plan currently represents 11 routes, four scenes, 23 review
methods per scene, 72 player launches per route, and 792 launches for the full
matrix. The automation has been prepared but has not been started.

Safe orchestration optimizations are part of the prepared runner: a shared
reference cache per route, direct in-process image publication, one ffmpeg pass
for the three conventional controls per scene, and a single NativeAA capture per
route. Game activity is recorded as provenance but never pauses or stops the
capture or user processes. Performance measurements taken while a game is active
must not be treated as image-quality evidence.

## Evidence and status

The prior-evidence audit is
[`quality_campaign_evidence_audit_20260902.md`](../../benchmarks/quality_sweeps/quality_campaign_evidence_audit_20260902.md).
It concluded that the historic capture roots could not satisfy this campaign;
the superseding plan therefore requires a clean shared recapture. The capture
contract and operating instructions are in
[`benchmarks/quality_sweeps/README_HARNESS_CAMPAIGN.md`](../../benchmarks/quality_sweeps/README_HARNESS_CAMPAIGN.md).

The older all-purpose reconstruction plan is preserved as
[`../archive/plans/QUALITY_RECONSTRUCTION_PLAN_20260822-20260902.md`](../archive/plans/QUALITY_RECONSTRUCTION_PLAN_20260822-20260902.md).
It is historical context, not an active instruction source.

## Completion

Do not promote a quality change from this plan without matching evidence,
appropriate reference comparison, performance measurements, artifact checks,
and a recorded causal decision. On completion, move this plan to the archive,
update current state/reference documents, and preserve the final reports and
primary evidence paths.
