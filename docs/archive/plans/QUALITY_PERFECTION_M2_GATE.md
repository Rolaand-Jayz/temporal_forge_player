# Milestone 2 gate: shared reprojection and FP16 temporal state

Status: passed for the source/resource-graph and compiled-path contract on
2026-08-22.

## Implemented slices

- Added one target-grid `rgba16f` `reprojectedColor_` image to
  `GpuImageUploader`, with explicit lifetime, transition, and accessor paths.
- Changed both ping-pong model-history images from RGB10/A2 to `rgba16f` while
  keeping display output as a separate RGBA8 image.
- Added the reprojected-color view/image to `FrameDispatchInput` and wired it
  through primary and chained PlaybackEngine dispatches.
- Extended the prepass descriptor contract to publish the causal resolve at
  binding 9.
- Made the postpass consume that same resource at binding 7 and removed its
  independent history reprojection helper and sampling path.
- Added barriers for previous-frame reprojected-color reuse and current-frame
  prepass-to-postpass visibility.
- Added `tests/fsr4_temporal_contract_tests.cpp` before the implementation.

## Gate evidence

- M2 temporal resource-graph contract test: passed.
- Prepass and postpass GLSL compilation: passed.
- Full build: passed, including the player, uploader, dispatch harness, and
  standalone backend/harness link targets.
- Existing CTest suite: 12 runnable tests passed.
- Existing CTest exclusions: tensor-map test skipped; GPU probe, CM dump, and
  live FSR4 harness remained disabled by their existing configuration.
- `git diff --check`: passed.

## What this gate does not prove

The disabled live GPU harness means this gate does not prove numerical range
preservation on hardware, actual frame-N causality, visual equivalence of the
shared resolve, or reset behavior under a running Vulkan submission. Those are
explicitly still open evidence items, not silently treated as passed.

## Next milestone

M3 is causal codec motion. Its direction/sign, future-reference exclusion,
coordinate scaling, overlap, confidence propagation, and scene-cut tests must
be authored before changing motion semantics.
