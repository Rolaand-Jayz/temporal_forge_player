# Temporal Forge current state

**Status:** CURRENT
**As of:** 2026-09-09
**Source:** branch `portability/clean-clone-remediation` (portability
remediation worktree)

This is a concise snapshot of what is true now. It is not an experiment
journal; active work is described in
[`../active/QUALITY_CAMPAIGN.md`](../active/QUALITY_CAMPAIGN.md) and
[`../active/PORTABILITY_REMEDIATION_20260909.md`](../active/PORTABILITY_REMEDIATION_20260909.md).

## Project

Temporal Forge Player is a GPU-native local-video player. It keeps a strict
one-input-frame to one-output-frame relationship and performs temporal
reconstruction without frame generation, interpolation, or cadence conversion.
Runtime requires Vulkan 1.3.

## Quality campaign line

The quality-focused campaign closed its lattice fix (FP16 resolve) and
completed the motion-campaign evidence. The authoritative record remains
[`../active/QUALITY_CAMPAIGN.md`](../active/QUALITY_CAMPAIGN.md) with
measurements in `benchmarks/quality_sweeps/`.

## Portability remediation (in flight)

A clean-clone portability audit found public documentation contradicting the
executable. Remediation (this campaign) is correcting documentation, label
consistency, and clean-machine build/test behavior on branch
`portability/clean-clone-remediation`.

## Backend default (truth)

The default backend is FSR4-RE Experimental INT8 (proof-gated;
`SettingsStore` default with `allowExperimentalAsDefault = true`), default
selection on supported RDNA3, falling back on failure. The FSR 3.1.5 SDK tier
is a compiled-out stub in every build of this tree (`TFORGE_HAVE_FSR3_SDK`
is never defined); the reliability floor is the always-available spatial
fallback. See
[`../reference/ARCHITECTURE.md`](../reference/ARCHITECTURE.md).

## Quality-lab policy (truth)

The checked-in `config/quality_lab.json` profile (base-only composition,
bilinear base filter) is loaded at startup and is the **shipped, measured
default playback policy** — applied scale-aware, only at ≥3x scale
(`PlaybackEngine`). `TFORGE_QUALITY_LAB_CONFIG` designates a deliberate
experiment override that is honored at every scale. This is not a hidden
diagnostic.

## Boundaries

Current code, dated evidence, and plans are distinct. Code is executable
truth; where documents disagree with it, the code wins and the documents get
fixed (the subject of this campaign).
