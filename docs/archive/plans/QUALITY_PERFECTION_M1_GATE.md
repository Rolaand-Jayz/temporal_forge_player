# Milestone 1 gate: FSR4 postpass parameter contract

Status: passed for the CPU/schema/compiled-shader contract on 2026-08-22.

## Implemented slices

- Added `src/backend/Fsr4PostpassParams.hpp/.cpp` with the typed v4.1 region:
  offset `130088`, `222` FP32 values, and `888` bytes ending at the padding
  boundary.
- The host decoder uses explicit little-endian byte assembly and rejects short
  data or non-finite values.
- `Fsr4DispatchHarness::uploadWeights()` validates and retains the decoded
  region before accepting the GPU upload.
- The postpass shader now declares matching offset/count constants, reads every
  parameter through `postpassParameterTrace()`, and exposes a diagnostic-only
  checksum through output alpha when `TFORGE_FSR4_POSTPASS_TRACE` is set.
- Added `tests/fsr4_postpass_contract_tests.cpp` before implementation and wired
  the new decoder into standalone harness/backend test targets.

## Gate evidence

- M1 postpass contract test: passed.
- GLSL postpass compilation: passed as part of `cmake --build build-fast`.
- Full build: passed, including the player, library, backend tests, and harness
  target link steps.
- Existing CTest suite: 11 runnable tests passed.
- Existing CTest exclusions: tensor-map test skipped; GPU probe, CM dump, and
  live FSR4 harness remained disabled by their existing configuration.
- Python/shell checks and `git diff --check`: passed.

## What this gate does not prove

This gate proves that the recovered byte region is bounded, decoded, validated,
bound at the existing postpass storage-buffer slot, shader-consumed in an
explicit diagnostic path, and compiled. It does not prove that the recovered
values improve an image or that their semantic roles are correct; the live GPU
parameter trace is still unavailable while the existing GPU harness test is
disabled. No quality experiment was started.

## Next milestone

M2 is shared causal reprojection and FP16 temporal state. Its resource-identity,
coordinate, causality, format, range, and reset tests must be authored before
changing history or recurrent-state resources.
