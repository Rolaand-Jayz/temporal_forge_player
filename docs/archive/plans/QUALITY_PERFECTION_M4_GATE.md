# Temporal Forge quality lab — M4 gate: color, transfer, chroma, bit depth

Status: **passed for the decode-to-upload source contract** on 2026-08-22.

## What was locked

M4 keeps color metadata attached to each decoded frame instead of allowing the
uploader to reconstruct it from resolution or assumptions:

- primaries;
- transfer characteristic;
- matrix/color space;
- range;
- chroma sample location;
- decoded component bit depth.

The YUV and DRM PRIME conversion shaders now receive the same explicit chroma
phase. Software high-bit-depth planar frames no longer enter the R8 upload path
as if every sample were one byte; they take the existing explicit normalization
route to an 8-bit GPU upload representation. DRM 10-bit imports retain their
R16 plane views.

## Tests written before implementation

`tests/color_metadata_contract_tests.cpp` was added before the M4 code changes.
It verifies decoder metadata capture, conversion push-constant metadata,
shader chroma-phase consumption, and the high-bit-depth upload boundary.

## Gate evidence

- `color_metadata_contract_tests`: passed.
- Full `build-fast` build: passed.
- GLSL compilation for both YUV conversion shaders: passed.
- CTest: 14 runnable tests passed; one tensor-map test skipped by its existing
  return-code policy; four GPU/diagnostic tests remain disabled by the existing
  suite configuration.
- Python syntax compilation for the quality-lab helpers: passed.
- Shell syntax checks for `benchmarks/video_corpus/*.sh`: passed.
- `git diff --check`: passed.

## Boundary still open

This gate does not claim live GPU numerical color validation. The live FSR4
harness remains disabled, so HDR transfer round trips, chroma siting against
reference frames, and hardware-specific DRM sampling still need a later
runtime-enabled validation gate. No new quality experiment or benchmark
capture was started by M4.
