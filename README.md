# Temporal Forge Player

> **Experimental research project · Flagship portfolio work**

Temporal Forge investigates adapting **AMD FSR 4.1-style temporal reconstruction/upscaling to ordinary decoded video**. It is not frame interpolation or frame generation:

`one decoded video frame → one reconstructed/upscaled displayed frame`

The source frame count, timestamps, and cadence remain the contract. A high-refresh display may repeat a reconstructed frame; the player does not invent intermediate frames.

## Why this is difficult

A native game renderer can provide motion, jitter, reset/history context, exposure, reactive/composition signals, and other semantic inputs designed for temporal reconstruction. Finished video does not naturally preserve those signals in the same form.

Temporal Forge therefore treats reconstruction quality as a research problem: determine what can be recovered or synthesized from video, what cannot be reconstructed reliably, and which inputs actually change temporal behavior.

## Current status

The player has an operational GPU-native pipeline with:

- **FSR4-RE Experimental** — INT8 reconstruction and the proof-gated default temporal path on supported RDNA3 hardware when the required runtime/compiler assets are available
- **FSR 3.1.5 (SDK) integration tier** — retained in source for future SDK integration, but compiled out of the redistributable clean-clone build
- **Spatial fallback** — always-available reliability path after Vulkan initialization when a temporal backend cannot run

Backend selection attempts FSR4-RE first when its proof gates are satisfied, then the SDK tier when compiled and available, then spatial fallback. Backend failure degrades to spatial scaling with a non-blocking warning instead of silently presenting an unavailable experimental path as successful.

The project remains experimental research and does not claim production readiness or parity with AMD's implementation. Meaningful current research may live on non-default branches; consult branch history and campaign documentation before treating experimental behavior as part of `main`.

The remaining problem is reconstruction quality, not merely moving frames through a pipeline. Current work focuses on expected-input semantics, motion transfer, temporal history, jitter, exposure, masking, reset behavior, composition, and causal diagnostics.

## What this project demonstrates

- Native C++23 / Vulkan / FFmpeg / Qt integration on Linux
- GPU video processing and temporal reconstruction
- FSR 4.1 reverse-engineering-informed interoperability research
- Reproducible experiments with benchmark conditions and provenance
- Explicit separation of measured facts, inferences, hypotheses, and unobserved behavior
- Preservation of negative results when they explain system behavior
- Independent review, adversarial challenge, remediation loops, and verification gates

The methodology matured here from earlier AMD-first application and reverse-engineering work. Those projects are part of the lineage; they should not be read as though this formal process existed from the beginning.

## Core rule

```text
1 decoded input frame → 1 reconstructed/upscaled displayed output frame
same timestamps · same frame count · same source frame rate
```

The player reconstructs each source frame at a higher internal resolution through the selected temporal path, then scales that result to the current window or fullscreen surface. Window resizing changes presentation only; it does not redefine the source cadence contract.

## Scaling model

```text
source frame
  → temporal reconstruction target (source × preset ratio)
  → final presentation scale to window
```

The reconstruction target depends on source size and preset, not window size. Resizing the window changes only presentation scaling and should not recreate the temporal context or reset history.

| Preset | Ratio |
|---|---:|
| NativeAA | 1.0x |
| Quality | 1.5x |
| Balanced | 1.7x |
| Performance | 2.0x |
| Ultra Performance | 3.0x |

## Clean-clone behavior

The ordinary redistributable build does **not** require a locally installed AMD FidelityFX SDK. In this tree, the FSR 3.1.5 SDK tier remains source-visible but is not linked into the clean-clone build; its runtime path reports that the SDK is not linked and selection continues to an available backend.

FSR4-RE has separate runtime/build asset requirements. Native INT8 packs are resolved from executable-relative or repository-relative locations, while generic weight blobs can be provisioned through `TFORGE_FSR4_RE_ROOT` or the documented XDG data location. Missing or invalid assets produce diagnostics and fallback rather than a false-success path.

### Git LFS review evidence

Campaign review images under `review_harness/images/*.png` use Git LFS. They are **not required to build, run, or test** the player; a normal clone can build with the pointer files in place. To retrieve the review-image payloads:

```sh
git lfs install
git lfs pull
```

## Documentation map

Start with the repository's current-state and architecture documents, then descend into the active research campaign, benchmark evidence, technical decisions, and archived or superseded reports. Authoritative dated experiment documentation takes precedence when historical reports differ from the current implementation.

Useful entry points include:

- [`docs/README.md`](docs/README.md) — documentation authority map
- [`docs/FSR4_RE_STATUS.md`](docs/FSR4_RE_STATUS.md) — dated FSR 4.1 RE reconstruction status/history
- [`benchmarks/quality_sweeps/`](benchmarks/quality_sweeps/) — current quality and causal experiment tooling/evidence
- [`docs/active/PORTABILITY_REMEDIATION_20260909.md`](docs/active/PORTABILITY_REMEDIATION_20260909.md) — clean-clone portability/remediation qualification record
- [`PROVENANCE.md`](PROVENANCE.md) — artifact provenance and unresolved-rights records

## Requirements

The runtime requires a **Vulkan 1.3** driver. A Vulkan 1.2-only machine cannot run the player.

| Package | Role | Required? | Behavior when missing |
|---|---|---|---|
| C++23 compiler (GCC 13+/Clang 17+ class) | build | required | configure/build fails |
| CMake ≥ 3.24 | build | required | configure fails |
| Ninja | build | required | configure fails |
| Qt 6.6+ — Core, Gui, Quick, Qml, Widgets, ShaderTools | build + runtime | required | configure fails |
| glslangValidator | build | required | configure fails |
| Vulkan loader + headers, API 1.3 | build + runtime | required | build fails / runtime cannot start |
| FFmpeg ≥ 5.1 development libraries | build + runtime | required | configure/build fails |
| Python 3 | build tooling | required | tooling steps fail |
| Git LFS | review evidence only | optional | review PNGs remain pointer files; build/runtime unaffected |
| ffmpeg executable with libx264 + aac encoders | test fixtures | optional | affected external-data tests SKIP rather than FAIL |
| jq, ImageMagick | research/capture scripts | optional | affected scripts fail with a clear error |
| DXC + SPIR-V tools | native FSR4 pack builds | optional | native pack build tooling cannot run |
| FidelityFX SDK (`TFORGE_ENABLE_FSR1_PROBE=ON`) | optional probe | optional | probe target is not built by default |

Vendored build dependencies include miniaudio v0.11.25 and the repository's Vulkan-header shim; see [`external/README.md`](external/README.md) and [`THIRD_PARTY_LICENSES.md`](THIRD_PARTY_LICENSES.md).

## Build

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
./build/temporal_forge_player
```

Set `TFORGE_VK_VALIDATE=1` to enable the Vulkan validation layer.

## Reliability behavior

If a selected experimental backend cannot initialize or execute safely, playback falls back to spatial scaling with a non-blocking warning rather than silently presenting the experimental path as successful.

## License and provenance

Temporal Forge's original code and documentation are licensed under the [Apache License, Version 2.0](LICENSE). Third-party components and reverse-engineering-derived artifacts can have separate licenses or rights status; see [`THIRD_PARTY_LICENSES.md`](THIRD_PARTY_LICENSES.md) and [`PROVENANCE.md`](PROVENANCE.md).

The tracked native INT8 FSR4 pack data includes reverse-engineering-derived artifacts whose redistribution-rights status is explicitly recorded as **UNRESOLVED / PROVENANCE HOLD**. They are not covered by the project's Apache-2.0 license. This is a provenance record, not a legal conclusion. Generic weight blobs and locally compiled SPIR-V pack modules are not tracked.

## Research status

Temporal Forge is active experimental R&D. Its purpose is not to claim that finished video supplies the same information as a game renderer; it is to determine, through controlled experiments, which missing temporal inputs matter, which useful surrogates can be synthesized, and where the approach reaches a hard information boundary.
