# Temporal Forge current state

**Status:** CURRENT
**As of:** 2026-09-02
**Source:** repository commit `269631d` plus the documentation audit

The worktree may also contain untracked capture-generated evidence while the
quality campaign runs. That evidence is outside this documentation snapshot.

## Project

Temporal Forge Player is a GPU-native local-video player. It keeps a strict
one-input-frame to one-output-frame relationship and performs temporal
reconstruction without frame generation, interpolation, or cadence conversion.

## Current implementation

The current code is organized around FFmpeg decode, `PlaybackEngine`, a Vulkan
upload/dispatch path, and a backend cascade. On RDNA3, `BackendSelector` tries
the proof-gated FSR4 INT8 reconstruction path first, then FSR 3.1.5 when
available, then spatial fallback. The implementation and its invariants are
authoritatively described in [`../reference/ARCHITECTURE.md`](../reference/ARCHITECTURE.md).

The quality lab is runtime-configurable through `config/quality_lab.json` and
the `TFORGE_*` environment contract. Diagnostic settings do not silently define
the normal player path.

## Quality phase

The project is in the quality-focused M6 recovery and evidence phase. The
historical M6 matrix and campaign audit show that earlier image payloads are
not sufficient to establish every required campaign method. The current
data-only campaign is active outside this documentation change; its generated
directory is intentionally not managed by this commit.

The authoritative active-work description is
[`../active/QUALITY_CAMPAIGN.md`](../active/QUALITY_CAMPAIGN.md). The primary
measurement source remains `benchmarks/quality_sweeps/`, including manifests,
CSV/JSON metrics, runtime traces, hashes, and campaign sidecars.

## Verified versus unresolved

- Milestone contracts M0 through M5 and the M6 tooling contract are preserved
  as completed historical gates in [`../archive/plans/`](../archive/plans/).
- Existing evidence supports keeping reconstruction and final delivery
  dimensions as separate controls. It does not justify a universal 3x default.
- Current code, intended architecture, and dated evidence are not interchangeable.
  When they diverge, the active plan and audit must name the divergence.
- The quality campaign is not complete merely because the harness or runner
  exists. Required coverage, provenance, measurements, and validation remain
  the completion gate.

## Boundaries

This document records the current repository state. It is not an experiment
journal and does not replace the active plan, reports, or raw evidence.
