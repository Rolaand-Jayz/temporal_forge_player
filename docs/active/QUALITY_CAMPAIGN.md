# Active quality campaign

**Status:** ACTIVE
**As of:** 2026-09-02
**Scope:** quality campaign recovery and evidence completion

## Objective

Complete only the campaign-required captures and measurements with truthful
method identity, resolution coverage, runtime provenance, and reproducible
artifacts. Review-harness-only resolution rows are separate from campaign
results, even when the same capture can supply both.

## Current gates

1. Use the current committed player and record its binary/source identity.
2. Capture the four campaign scenes and the campaign-required resolution pairs
   and methods defined by the active manifests.
3. Keep CAS at `0.20`, and record whether CAS is pre-downsample, post-downsample,
   or absent as a distinct method dimension.
4. Retain metrics, timing, runtime traces, dimensions, timestamps, configuration,
   hashes, and failure/guard events. Image payloads are optional only where the
   manifest explicitly permits data-only evidence.
5. Validate every completed row against the manifest before calling the campaign
   complete. A missing row, ambiguous label, mismatched timestamp, or wrong
   binary invalidates that row.

## Evidence and status

The current audit is [`benchmarks/quality_sweeps/quality_campaign_evidence_audit_20260902.md`](../../benchmarks/quality_sweeps/quality_campaign_evidence_audit_20260902.md).
The harness campaign description is
[`benchmarks/quality_sweeps/README_HARNESS_CAMPAIGN.md`](../../benchmarks/quality_sweeps/README_HARNESS_CAMPAIGN.md).
The capture process is outside this documentation commit and must not be
interrupted by documentation maintenance.

The older all-purpose reconstruction plan is preserved as
[`../archive/plans/QUALITY_RECONSTRUCTION_PLAN_20260822-20260902.md`](../archive/plans/QUALITY_RECONSTRUCTION_PLAN_20260822-20260902.md).
It is historical context, not an active instruction source.

## Completion

Do not promote a quality change from this plan without matching evidence,
appropriate reference comparison, performance measurements, artifact checks,
and a recorded causal decision. On completion, move this plan to the archive,
update current state/reference documents, and preserve the final reports and
primary evidence paths.
