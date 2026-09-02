# Temporal Forge technical history

This is the causal index for major direction changes. It is intentionally
short. Dated reports and archived plans contain the detailed evidence.

## Initial player architecture

**Problem:** Build a local video player with temporal reconstruction while
preserving frame identity and real-time Vulkan operation.

**Decision:** Keep decoding, playback orchestration, Vulkan upload/dispatch,
backend selection, and Qt presentation as separate responsibilities. The
current invariants live in [`../reference/ARCHITECTURE.md`](../reference/ARCHITECTURE.md).

## FSR4 reconstruction and proof path

**Problem:** The official FSR4 distribution did not provide the needed native
Linux/Vulkan path.

**Investigation:** The project reconstructed the model and dispatch contract
from available evidence, then added typed parameter, graph, and runtime proof
checks.

**Decision:** Treat the FSR4 INT8 path as experimental and proof-gated. Failure
cascades to FSR 3.1.5 or spatial fallback. The dated reconstruction record is
[`../reports/FSR4_RECONSTRUCTION_STATUS_20260709.md`](../reports/FSR4_RECONSTRUCTION_STATUS_20260709.md).

## Temporal contracts and motion

**Problem:** Video does not provide the same motion, jitter, reset, and side
inputs as a rendered game frame.

**Evidence:** M0 through M5 contracts formalized provenance, postpass
parameters, reprojection state, causal motion, color metadata, and jitter.

**Decision:** Keep those inputs explicit and evidence-bound. Codec/refined
motion and synthetic jitter remain separate experimental controls, not assumed
truth. Current contracts are in [`../reference/motion/`](../reference/motion/);
the completed gates are archived in [`../archive/plans/`](../archive/plans/).

## Supersampling and delivery scale

**Question:** Does increasing the FSR reconstruction grid improve output after
delivery reduction?

**Evidence:** The 2026-08-31 supersampling report found aggregate gains for
some real scenes, but losses on other scene slices and higher memory cost.

**Decision:** Retain independent reconstruction and delivery dimensions as
controls. Do not make 3x a universal default.

## Capture and provenance recovery

**Problem:** Earlier campaign and harness outputs did not establish complete,
truthful coverage for every required method and resolution.

**Decision:** Re-capture the required campaign data with explicit method,
resolution, timestamp, binary, and configuration provenance. The audit and raw
benchmark artifacts are primary evidence; old reports remain historical and
must not be rewritten as if they were produced by the recovery run.

## Uncertainty

This index records the causal conclusions supported by repository evidence. It
does not claim that every remaining temporal-quality question is resolved. Open
questions belong in the active plan and their eventual dated reports.
