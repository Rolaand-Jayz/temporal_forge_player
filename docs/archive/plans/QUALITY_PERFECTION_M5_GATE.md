# Temporal Forge quality lab — M5 gate: decoded-video jitter policy

Status: **passed for explicit policy and reset-phase behavior** on 2026-08-22.

## What was locked

Decoded-video jitter is now an explicit policy, not an unexplained scalar:

- `current` preserves the existing default behavior;
- `off` produces no jitter for diagnostic sequences;
- `reduced` uses half the current amplitude;
- `controlled` uses a recorded strength in `[0, 1.5]`.

The Halton phase still resets after seek/new-file/scene-cut reset handling.
Jitter policy selection does not alter motion filtering, history rules, or
reconstruction composition. The quality-sweep wrapper records the mode and
controlled strength in the exact experiment metadata and passes them through
as explicit runtime settings.

## Tests written before implementation

- `tests/jitter_policy_tests.cpp` covers mode behavior, amplitude bounds, and
  reset-phase restart at the `SideBufferSynth` boundary.
- `tests/test_jitter_manifest_contract.py` covers supported modes and rejects
  unknown modes or invalid controlled strengths in capture metadata.

## Gate evidence

- Full `build-fast` build: passed.
- Full CTest: 15 runnable tests passed; one tensor-map test skipped by its
  existing return-code policy; three GPU/diagnostic tests remain disabled by
  the existing suite configuration.
- M0 and M5 Python contract tests: 8 passed.
- Python syntax compilation: passed.
- Shell syntax checks for `benchmarks/video_corpus/*.sh`: passed.
- `git diff --check`: passed.

## Boundary still open

This gate proves policy selection and phase determinism, not which mode wins
sequence quality. The static-region flicker, edge variance,
motion-compensated residual, and occlusion-duration comparisons require the
later capture campaign with the live GPU harness enabled. No new quality
experiment or capture was started by M5.
