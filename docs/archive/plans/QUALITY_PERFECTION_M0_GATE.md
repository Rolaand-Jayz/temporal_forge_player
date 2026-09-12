# Milestone 0 gate: trustworthy baseline and observability

Status: passed on 2026-08-22.

## Implemented slices

- Added `benchmarks/quality_sweeps/quality_lab_contract.py` with the M0
  manifest, stage-artifact, dimension, environment, hashing, and immutable
  directory contracts.
- Updated `benchmarks/quality_sweeps/run_quality_sweep.py` so each candidate
  attempt receives a non-overwriting numbered directory and records binary/config
  hashes plus all image-affecting `TFORGE_*` environment settings passed to the
  runner.
- Added the valid baseline fixture at
  `tests/fixtures/m0_baseline_valid.json`.
- Added the contract tests in `tests/test_quality_lab_contract.py` before the
  implementation was written.

## Gate evidence

- M0 contract tests: 6 passed.
- Python syntax checks: passed for both quality-sweep modules.
- Shell syntax checks: passed for all quality-sweep and video-corpus scripts.
- Existing CTest suite: 10 runnable tests passed.
- Existing CTest exclusions: the tensor-map test skipped; GPU probe, CM dump,
  and live FSR4 harness remained disabled by their existing configuration.
- `git diff --check`: passed.
- `run_quality_sweep.py --help`: launched successfully after the metadata
  integration.

## What this gate does not prove

This gate proves the audit and provenance layer only. It does not prove a new
reconstruction result, GPU stage dumps, quality improvement, or production
performance. No capture or image-quality experiment was started for M0.

## Next milestone

M1 is the FSR4 postpass parameter contract. Its tests and fixtures must be
written and reviewed before any postpass implementation changes begin.
