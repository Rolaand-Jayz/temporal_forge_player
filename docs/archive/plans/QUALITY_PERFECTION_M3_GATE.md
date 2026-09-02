# Milestone 3 gate: causal codec motion

Status: passed for the decoder/playback/GPU-boundary contract on 2026-08-22.

## Implemented slices

The current motion path already contained the required causal policy, so this
milestone formalized and verified it without changing motion behavior:

- FFmpeg reference direction is normalized into `MvEntry::source`.
- Future-reference vectors (`source > 0`) are rejected before confidence or
  texture upload.
- Non-finite and implausibly large vectors are rejected.
- Past vectors retain their direction and are scaled into model coordinates.
- Sparse vector ownership resolves deterministically with the existing atomic
  last-vector-wins rule.
- Motion confidence and reset state reach the FSR dispatch contract.

## Gate evidence

- Added `tests/fsr4_motion_contract_tests.cpp` before running the milestone.
- Full build: passed.
- Causal motion contract test: passed.
- Existing CTest suite: 13 runnable tests passed.
- Existing CTest exclusions: tensor-map test skipped; GPU probe, CM dump, and
  live FSR4 harness remained disabled by their existing configuration.
- `git diff --check`: passed.

## What this gate does not prove

The current CPU/GPU-boundary tests do not replace an on-hardware translated
block, occlusion, or adjacent-frame validation sequence. Those remain required
before promoting motion quality claims, but no new motion experiment was run.

## Next milestone

M4 is color, transfer, chroma, and bit-depth correctness. Its metadata,
limited/full-range, HDR, chroma-siting, precision, and model/display-domain
tests must be authored before changing conversion behavior.
